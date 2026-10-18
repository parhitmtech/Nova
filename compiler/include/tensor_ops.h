#ifndef NOVA_TENSOR_OPS_H
#define NOVA_TENSOR_OPS_H
// Tensor operations behind the tensor_* built-ins (src/runtime/tensor.cc). Handles index tensor_heap.
// Allocation, retain/release and tensor_set_shape are declared in compiler.h.

#include <string>
#include <vector>
#include "compiler.h"

using std::string;

size_t tensor_offset(const Tensor& T, const int* idx, int n, const char* op, int line);
void require_2d(int h, const char* op, int line);
int ten_reshape(int a, std::vector<int> shape, int line);  // one dimension may be -1
bool slot_owns(int i);
void set_owns(int i, bool v);
int ten_zeros(const std::vector<int>& shape);
int ten_ones(const std::vector<int>& shape);
int ten_randn(const std::vector<int>& shape);
int ten_rand_int(int lo, int hi);
int ten_xavier(int r, int c);
int ten_matmul(int a, int b);
int ten_add(int a, int b);
int ten_sub(int a, int b);
int ten_sum_axis0(int a);
int ten_mul(int a, int b);
int ten_scale(int a, double s);
int ten_transpose(int a);
int ten_clip(int a, double lo, double hi);
int ten_relu(int a);
int ten_sigmoid(int a);
int ten_tanh(int a);
int ten_softmax(int a);
int ten_relu_grad(int a);
int ten_sigmoid_grad(int a);
double ten_mse_loss(int pred, int target);
int ten_mse_grad(int pred, int target);
int ten_bce_grad(int pred, int target);
double ten_ce_loss(int logits, int target);
int ten_ce_grad(int logits, int target);
int ten_get_row(int a, int row);
void ten_set_row(int dst, int row, int src);
int ten_slice_rows(int a, int start, int end);
int ten_progress_bar(int total, int width);
void ten_progress_update(int handle, int epoch, double loss);
void ten_progress_done(int handle);
int ten_load_csv(const string& path, int rows, int cols);

#endif
