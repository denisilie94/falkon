import math
import logging
from collections import defaultdict

import torch

from falkon import la_helpers
from falkon.c_ext import cusolver_potrf, cusolver_potrf_buffer_size, parallel_potrf
from falkon.options import CholeskyOptions, FalkonOptions
from falkon.utils.device_copy import copy
from falkon.utils.devices import DeviceInfo, get_device_info
from falkon.utils.helpers import sizeof_dtype
from falkon.utils.tensor_helpers import copy_same_stride, create_fortran, is_f_contig

from .ooc_utils import calc_block_sizes

__all__ = ("gpu_cholesky",)
logger = logging.getLogger(__name__)


def _ic_cholesky(A, upper, device):
    """Cholesky factorization of matrix `A` on the GPU

    Uses the cuSOLVER library for implementation of the POTRF function.

    Parameters:
    -----------
    A : [n, n] CPU or GPU array (column-contiguous)
        The (positive definite) matrix which should be factorized
    upper : bool
        Whether we need to factorize the upper of lower portion of `A`. The other side
        of the matrix will not be touched.
    device : int
        The GPU device on which to run the factorization
    cusolver_handle
        Pointer to the cuSOLVER context, which needs to be initialized before calling
        the function.

    Returns:
    --------
    A : [n, n] CPU or GPU array (column-contiguous)
        The factorization of A which overwrites the upper (or lower) triangular part
        of the matrix A. This is not a copy of the original matrix.
    """
    if not is_f_contig(A):
        raise RuntimeError("Cholesky input must be F-contiguous")

    n = A.shape[0]

    tc_device = torch.device(f"cuda:{device}")
    tc_stream = torch.cuda.current_stream(tc_device)
    with torch.cuda.device(tc_device), torch.cuda.stream(tc_stream):
        if A.is_cuda:
            Agpu = A
        else:
            Agpu = create_fortran((n, n), dtype=A.dtype, device=tc_device)
            copy(A, Agpu, non_blocking=True)

        # Determine necessary buffer size
        potrf_bsize = cusolver_potrf_buffer_size(A=Agpu, upper=upper, n=n, lda=n)

        # Allocate workspace and info buffers
        potrf_wspace = torch.empty(size=(potrf_bsize,), dtype=A.dtype, device=tc_device)
        dev_info = torch.tensor(4, dtype=torch.int32, device=tc_device)

        # Run cholesky
        cusolver_potrf(
            A=Agpu, workspace=potrf_wspace, workspace_size=potrf_bsize, info=dev_info, upper=upper, n=n, lda=n
        )

        # Copy back to CPU
        if not A.is_cuda:
            copy(Agpu, A, non_blocking=True)
            del Agpu
        del potrf_wspace, dev_info
        tc_stream.synchronize()
    return A


def _parallel_potrf_runner(A: torch.Tensor, opt: CholeskyOptions, gpu_info) -> torch.Tensor:
    num_gpus = len(gpu_info)
    N = A.shape[0]
    dt = A.dtype
    # Calculate the maximum block size such that we don't run out of GPU
    # RAM on **any** available GPU. We need a total of 2 whole columns and 1 tile:
    # block-size^2 * ((N / block-size) * 2 + 1) floats
    # (plus the cuSOLVER buffer which is small).
    # block_size < (sqrt((2*N)^2 + 4R) - 2*N) / 2
    dts = sizeof_dtype(dt)
    avail_ram = min([g.actual_free_mem for g in gpu_info]) / dts
    max_block_size = (math.sqrt(4 * N**2 + 4 * avail_ram) - 2 * N) / 2
    max_block_size = int(math.floor(max_block_size))
    if max_block_size < 1:
        raise RuntimeError(
            "Cannot run parallel POTRF with minimum available memory of %.2fMB" % (avail_ram * dts / 2**20)
        )

    block_sizes = calc_block_sizes(max_block_size, num_gpus, N, opt.chol_par_blk_multiplier)
    block_allocations = defaultdict(list)
    cur_n = 0
    for i, bs in enumerate(block_sizes):
        block_allocations["start"].append(cur_n)
        block_allocations["end"].append(cur_n + bs)
        block_allocations["size"].append(bs)
        block_allocations["device_id"].append(i % num_gpus)
        block_allocations["id"].append(i)
        cur_n += bs

    for g in range(num_gpus):
        torch.cuda.current_stream(g).synchronize()
    parallel_potrf(
        devices=list(range(num_gpus)),
        block_starts=block_allocations["start"],
        block_ends=block_allocations["end"],
        block_sizes=block_allocations["size"],
        block_devices=block_allocations["device_id"],
        block_ids=block_allocations["id"],
        A=A,
    )
    return A


"""
GPU Cholesky, we implement use cuSOLVER as a backend for POTRF.

 - In-core: Can do upper or lower, must be Fortran
 - Out of core: Can only do lower, Fortran

"""


def can_do_ic(A: torch.Tensor, device: DeviceInfo):
    # noinspection PyUnresolvedReferences
    avail_ram = device.actual_free_mem
    # The multiplier here is a bit tricky since setting it too high results
    # in hard-to-debug cuda errors
    avail_ram *= 0.85

    if A.is_cuda:
        needed_ram = 100 * 8  # Not very much indeed
    else:
        needed_ram = A.shape[0] * A.shape[1] * sizeof_dtype(A.dtype)

    return avail_ram >= needed_ram


def gpu_cholesky(A: torch.Tensor, upper: bool, clean: bool, overwrite: bool, opt: FalkonOptions) -> torch.Tensor:
    """
    Parameters
    -----------
    A : torch.Tensor
        2D positive-definite matrix of size (n x n) that will be factorized as
        ``A = U.T @ U`` (if `upper` is True) or ``A = L @ L.T`` if `upper`
        is False.
    upper : bool
        Whether the triangle which should be factorized is the upper or lower of `A`.
    clean : bool
        Whether the "other" triangle of the output matrix (the one that
        does not contain the factorization) will be filled with zeros or
        not.
    overwrite : bool
        Whether to overwrite matrix A or to output the result in a new
        buffer.
    opt : FalkonOptions
        Options forwarded for block calculation, and other knobs in the out-of-core
        parallel POTRF implementation. Useful options are the ones defined in
        :class:`~falkon.options.CholeskyOptions` .

    Notes
    ------
    The factorization will always be the 'lower' version of the factorization
    which could however end up on the upper-triangular part of the matrix
    in case A is not Fortran contiguous to begin with.
    """
    # Handle 'overwrite' option immediately so that its usage is reflected in memory
    # availability (in case A is on GPU).
    if not overwrite:
        # We could change the stride to be more favorable to the POTRF requirements
        # but it gets complicated. We leave such decisions to the user!
        A = copy_same_stride(A, pin_memory=True)

    # Decide which version of the algo we run: can be in-core or parallel.
    # (Note that the original OOC version is not going to run).

    # Determine GPU free RAM
    gpu_info = [v for k, v in get_device_info(opt).items() if k >= 0]
    for g in gpu_info:
        g.actual_free_mem = min((g.free_memory - 300 * 2**20) * 0.95, opt.max_gpu_mem * 0.95)

    if A.is_cuda:
        try:
            device = [d for d in gpu_info if d.Id == A.device.index][0]
        except IndexError as e:
            # This should never happen!
            raise RuntimeError(f"Device of matrix A ({A.device}) is not recognized") from e
    else:
        device = max(gpu_info, key=lambda g_: g_.actual_free_mem)
    ic = can_do_ic(A, device) and not opt.chol_force_ooc
    if opt.chol_force_in_core and not ic:
        raise RuntimeError("Cannot run in-core POTRF but `chol_force_in_core` was specified.")

    f_order = is_f_contig(A)
    transposed = False
    if not f_order:
        A = A.T
        upper = not upper
        transposed = True
    # Now A is always in f_order. So we can only allow upper=False (ooc)
    if upper:
        # Can do only in-core!
        if not ic:
            raise ValueError(
                "GPU POTRF is only implemented on the "
                "lower triangle for Fortran-ordered matrices (or on the upper "
                "triangle for C-ordered matrices)"
            )
    if not ic and A.is_cuda:
        _msg = "Cannot run out-of-core POTRF on CUDA matrix 'A'."
        if opt.chol_force_ooc:
            _msg += " Set the `chol_force_ooc` option to `False` in to allow in-core POTRF."
        raise ValueError(_msg)

    # Handle different implementations for POTRF: in-core and out-of-core
    if ic:
        if opt.debug:
            logger.info("Using in-core POTRF")
        _ic_cholesky(A, upper, device=device.Id)
    else:
        if opt.debug:
            logger.info("Using parallel POTRF")
        _parallel_potrf_runner(A, opt, gpu_info)

    # Perform cleaning of the 'other side' of the matrix
    if clean:
        la_helpers.zero_triang(A, upper=not upper)
    # Undo previous matrix transformations
    if transposed:
        A = A.T

    return A
