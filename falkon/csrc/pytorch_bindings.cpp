#include <vector>
#include <tuple>

#include <torch/extension.h>

#if (TORCH_VERSION_MAJOR >= 1) && (TORCH_VERSION_MINOR >= 7)
#define NEW_TORCH
#endif

// CPU functions: sparse, squared-norm
#include "cpu/sparse_norm.h"
#include "cpu/sparse_bdot.h"
#ifdef NEW_TORCH
#include "cpu/square_norm_cpu.h"
#endif

// CUDA functions
#ifdef WITH_CUDA
#include <pybind11/stl.h>
#include <cusolverDn.h>
#include <c10/cuda/CUDAStream.h>

// OOC operations
#include "cuda/multigpu_potrf.h"
#include "cuda/lauum.h"

// Utilities
#include "cuda/copy_transpose_cuda.h"
#include "cuda/copy_triang_cuda.h"
#include "cuda/mul_triang_cuda.h"
#include "cuda/vec_mul_triang_cuda.h"

// Sparse
#include "cuda/spspmm_cuda.h"
#include "cuda/csr2dense_cuda.h"

// Square norm
#ifdef NEW_TORCH
#include "cuda/square_norm_cuda.h"
#endif

// CUDA library bindings
#include "cuda/cublas_bindings.h"
#endif

#ifdef WITH_CUDA
static void* ctypes_void_ptr(const py::object& object) {
    PyObject *p_ptr = object.ptr();
    if (!PyObject_HasAttr(p_ptr, PyUnicode_FromString("value"))) {
        return nullptr;
    }
    PyObject *ptr_as_int = PyObject_GetAttr(p_ptr, PyUnicode_FromString("value"));
    if (ptr_as_int == Py_None) {
        return nullptr;
       }
    void *ptr = PyLong_AsVoidPtr(ptr_as_int);
    return ptr;
}
#endif


void _cublas_2d_copy_to_dev_async(
    const int rows,
    const int cols,
    const int elemSize,
    const torch::Tensor& host_tensor,
    const int lda, torch::Tensor& dev_tensor,
    const int ldb,
    const at::cuda::CUDAStream &stream
)
{
    #ifdef WITH_CUDA
        cublas_2d_copy_to_dev_async(rows, cols, elemSize, host_tensor, lda, dev_tensor, ldb, stream);
    #else
        AT_ERROR("Not compiled with CUDA support");
    #endif
}

void _cublas_2d_copy_to_dev(
    const int rows,
    const int cols,
    const int elemSize,
    const torch::Tensor& host_tensor,
    const int lda, torch::Tensor& dev_tensor,
    const int ldb
)
{
    #ifdef WITH_CUDA
        cublas_2d_copy_to_dev(rows, cols, elemSize, host_tensor, lda, dev_tensor, ldb);
    #else
        AT_ERROR("Not compiled with CUDA support");
    #endif
}

void _cublas_2d_copy_to_host_async(
    const int rows,
    const int cols,
    const int elemSize,
    const torch::Tensor& dev_tensor,
    const int lda, torch::Tensor& host_tensor,
    const int ldb,
    const at::cuda::CUDAStream &stream
)
{
    #ifdef WITH_CUDA
        cublas_2d_copy_to_host_async(rows, cols, elemSize, dev_tensor, lda, host_tensor, ldb, stream);
    #else
        AT_ERROR("Not compiled with CUDA support");
    #endif
}

void _cublas_2d_copy_to_host(
    const int rows,
    const int cols,
    const int elemSize,
    const torch::Tensor& dev_tensor,
    const int lda, torch::Tensor& host_tensor,
    const int ldb
)
{
    #ifdef WITH_CUDA
        cublas_2d_copy_to_host(rows, cols, elemSize, dev_tensor, lda, host_tensor, ldb);
    #else
        AT_ERROR("Not compiled with CUDA support");
    #endif
}


torch::Tensor parallel_potrf(
     std::vector<std::tuple<float, py::object, int>> gpu_info,
     std::vector<std::tuple<int, int, int, int, int>> allocations,
     torch::Tensor A)
{
#ifdef WITH_CUDA
    std::vector<blockAlloc> out_allocs;
    for (std::tuple<int, int, int, int, int> ba_tpl : allocations) {
        blockAlloc ba = {
            .start =std::get<0>(ba_tpl),
            .end   =std::get<1>(ba_tpl),
            .size  =std::get<2>(ba_tpl),
            .device=std::get<3>(ba_tpl),
            .id    =std::get<4>(ba_tpl)
        };
        out_allocs.push_back(ba);
    }
    auto ctypes = py::module::import("ctypes");
    std::vector<gpuInfo> out_gpu_info;
    for (auto &gi_tp : gpu_info) {
        // Parse the cusolver handle
        py::object cus_handle_obj = std::get<1>(gi_tp);
        void *cus_handle_vptr = ctypes_void_ptr(cus_handle_obj);
        if (cus_handle_vptr == nullptr) {
            throw std::invalid_argument("cusolver_handle");
        }
        gpuInfo gi = {
            .free_memory = std::get<0>(gi_tp),
            .cusolver_handle = (cusolverDnHandle_t)cus_handle_vptr,
            .id = std::get<2>(gi_tp)
        };
        out_gpu_info.push_back(gi);
    }
    return parallel_potrf_cuda(out_gpu_info, out_allocs, A);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor lauum(const int n, const torch::Tensor &A, const int lda, torch::Tensor &B, const int ldb, const bool lower)
{
#ifdef WITH_CUDA
    return lauum_cuda(n, A, lda, B, ldb, lower);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor copy_triang(torch::Tensor &A,
                          const bool upper)
{
#ifdef WITH_CUDA
    return copy_triang_cuda(A, upper);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor mul_triang(torch::Tensor &A,
                         const bool upper,
                         const bool preserve_diag,
                         const double multiplier)
{
#ifdef WITH_CUDA
    return mul_triang_cuda(A, upper, preserve_diag, multiplier);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor copy_transpose(const torch::Tensor &input,
                             torch::Tensor &output)
{
#ifdef WITH_CUDA
    return copy_transpose_cuda(input, output);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor vec_mul_triang(torch::Tensor &A,
                             const torch::Tensor &v,
                             const bool upper,
                             const int side)
{
#ifdef WITH_CUDA
    return vec_mul_triang_cuda(A, v, upper, side);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

/*
torch::Tensor trtri(torch::Tensor &A, const bool lower, const bool unitdiag) {
#ifdef WITH_CUDA
    return trtri_cuda(A, lower, unitdiag);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}
*/

torch::Tensor square_norm_call(const torch::Tensor &input, int64_t dim, torch::optional<bool> opt_keepdim)
{
#ifdef NEW_TORCH
    if (input.device().is_cuda()) {
    #ifdef WITH_CUDA
        return square_norm_cuda(input, dim, opt_keepdim);
    #else
       TORCH_CHECK(false, "Not compiled with CUDA support");
    #endif
    } else {
        return square_norm_cpu(input, dim, opt_keepdim);
    }
#else
    return at::pow(at::norm(input, 2, dim, opt_keepdim.value_or(false)), 2);
#endif
}

std::tuple<torch::Tensor, torch::Tensor, torch::Tensor>
spspmm(
    const torch::Tensor &rowptrA,
    const torch::Tensor &colA,
    const torch::Tensor &valA,
    const torch::Tensor &rowptrB,
    const torch::Tensor &colB,
    const torch::Tensor &valB,
    int64_t N
) {
#ifdef WITH_CUDA
    return spspmm_cuda(rowptrA, colA, valA, rowptrB, colB, valB, N);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor csr2dense(
    const torch::Tensor &rowptr,
    const torch::Tensor &col,
    const torch::Tensor &val,
    torch::Tensor &out
) {
#ifdef WITH_CUDA
    return csr2dense_cuda(rowptr, col, val, out);
#else
    AT_ERROR("Not compiled with CUDA support");
#endif
}

torch::Tensor sparse_row_norm(
        torch::Tensor indexptr,
        torch::Tensor data,
        torch::optional<torch::Tensor> out=torch::nullopt) {
    return norm(indexptr, data, out);
}

torch::Tensor sparse_row_norm_sq(
        torch::Tensor indexptr,
        torch::Tensor data,
        torch::optional<torch::Tensor> out=torch::nullopt) {
    return norm_sq(indexptr, data, out);
}


torch::Tensor sparse_bdot(
        const torch::Tensor &indexptr1,
        const torch::Tensor &indices1,
        const torch::Tensor &data1,
        const torch::Tensor &indexptr2,
        const torch::Tensor &indices2,
        const torch::Tensor &data2,
        torch::optional<torch::Tensor> out=torch::nullopt) {
    return sparse_bdot_impl(indexptr1, indices1, data1, indexptr2, indices2, data2, out);
}


PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("parallel_potrf", &parallel_potrf, "GPU-Parallel Cholesky Factorization");

  m.def("lauum_cuda", &lauum, "out of place LAUUM operation on CUDA matrices.",
        py::arg("n"), py::arg("A"), py::arg("lda"), py::arg("B"),
	    py::arg("ldb"), py::arg("lower"));

  m.def("copy_triang", &copy_triang, "Make a CUDA tensor symmetric",
        py::arg("A"), py::arg("upper"));

  m.def("mul_triang", &mul_triang, "Multiply the triangular of a CUDA tensor",
        py::arg("A"), py::arg("upper"), py::arg("preserve_diag"), py::arg("multiplier"));

  m.def("copy_transpose", &copy_transpose, "Transpose a matrix out-of-place",
        py::arg("input"), py::arg("output"));

  m.def("vec_mul_triang", &vec_mul_triang, "Multiply a triangular matrix by a vector",
        py::arg("A"), py::arg("v"), py::arg("upper"), py::arg("side"));

  m.def("spspmm", &spspmm, "Sparse*Sparse -> Sparse matrix multiplication (CUDA tensors)",
        py::call_guard<py::gil_scoped_release>());

  m.def("csr2dense", &csr2dense, "Convert CSR matrix to dense matrix (CUDA tensors)",
        py::call_guard<py::gil_scoped_release>());

  m.def("sparse_row_norm_sq", &sparse_row_norm_sq, "Squared row-wise norm of a sparse CPU matrix",
        py::call_guard<py::gil_scoped_release>());

  m.def("sparse_row_norm", &sparse_row_norm, "Row-wise norm of a sparse CPU matrix",
        py::call_guard<py::gil_scoped_release>());

  m.def("square_norm", &square_norm_call, "Squared l2 norm squared. Supports both CUDA and CPU inputs.",
        py::arg("input"), py::arg("dim"), py::arg("keepdim"));

  m.def("sparse_bdot", &sparse_bdot, "Row-wise batch dot-product on sparse tensors",
        py::arg("indexptr1"), py::arg("indices1"), py::arg("data1"), py::arg("indexptr2"), py::arg("indices2"), py::arg("data2"), py::arg("out"),
        py::call_guard<py::gil_scoped_release>()
  );

  m.def("cublas_2d_copy_to_dev_async", &_cublas_2d_copy_to_dev_async, "cuBLAS 2D copy to device asynchronously");
  m.def("cublas_2d_copy_to_dev", &_cublas_2d_copy_to_dev, "cuBLAS 2D copy to device");
  m.def("cublas_2d_copy_to_host_async", &_cublas_2d_copy_to_host_async, "cuBLAS 2D copy to host asynchronously");
  m.def("cublas_2d_copy_to_host", &_cublas_2d_copy_to_host, "cuBLAS 2D copy to host");

}
