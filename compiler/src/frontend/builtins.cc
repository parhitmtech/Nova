// The tensor built-ins: their names, and which give nothing, an int or a double instead of a Tensor. The check pass
// declares and checks them by these tables, and lowering lowers them by them.
#include <map>
#include <set>
#include <string>
#include "compiler.h"

using namespace std;

// The tensor built-ins by name
extern const map<string, TensorOp> TENSOR_OP_MAP = {
    {"tensor", TEN_ALLOC},
    {"tensor_zeros", TEN_ZEROS},
    {"tensor_ones", TEN_ONES},
    {"tensor_randn", TEN_RANDN},
    {"tensor_xavier", TEN_XAVIER},
    {"tensor_get", TEN_GET},
    {"tensor_set", TEN_SET},
    {"tensor_rows", TEN_ROWS},
    {"tensor_cols", TEN_COLS},
    {"tensor_print", TEN_PRINT},
    {"matmul", TEN_MATMUL},
    {"tensor_add", TEN_ADD},
    {"tensor_sub", TEN_SUB},
    {"tensor_mul", TEN_MUL},
    {"tensor_div", TEN_DIV},
    {"tensor_scale", TEN_SCALE},
    {"tensor_T", TEN_T},
    {"tensor_sum", TEN_SUM},
    {"tensor_mean", TEN_MEAN},
    {"tensor_max", TEN_MAX},
    {"tensor_clip", TEN_CLIP},
    {"relu", TEN_RELU},
    {"sigmoid", TEN_SIGMOID},
    {"tanh_t", TEN_TANH},
    {"softmax", TEN_SOFTMAX},
    {"relu_grad", TEN_RELU_GRAD},
    {"sigmoid_grad", TEN_SIGMOID_GRAD},
    {"mse_loss", TEN_MSE_LOSS},
    {"mse_grad", TEN_MSE_GRAD},
    {"bce_grad", TEN_BCE_GRAD},
    {"cross_entropy_loss", TEN_CE_LOSS},
    {"cross_entropy_grad", TEN_CE_GRAD},
    {"tensor_sum_rows",    TEN_SUM_AXIS0},
    {"rand_int", TEN_RAND_INT},
    {"tensor_get_row", TEN_GET_ROW},
    {"tensor_set_row", TEN_SET_ROW},
    {"tensor_slice_rows", TEN_SLICE_ROWS},
    {"tensor_load_csv", TEN_LOAD_CSV},
    {"progress_bar", TEN_PROGRESS_BAR},
    {"progress_update", TEN_PROGRESS_UPDATE},
    {"progress_done", TEN_PROGRESS_DONE},
    {"requires_grad", TEN_REQUIRES_GRAD},
    {"backward", TEN_BACKWARD},
    {"tensor_grad", TEN_TENSOR_GRAD},
    {"zero_grad", TEN_ZERO_GRAD},
    {"grad_step", TEN_GRAD_STEP},
    {"tensor_live_count", TEN_LIVE_COUNT},
    {"tensor_rank", TEN_RANK},
    {"tensor_dim", TEN_DIM},
    {"tensor_reshape", TEN_RESHAPE},
    // short names
    {"zeros", TEN_ZEROS},
    {"ones", TEN_ONES},
    {"randn", TEN_RANDN},
    {"xavier", TEN_XAVIER},
    {"reshape", TEN_RESHAPE}
};

// ops that don't return a meaningful value (void)
extern const set<TensorOp> TENSOR_VOID_OPS = {
    TEN_SET, TEN_PRINT, TEN_SET_ROW, 
    TEN_PROGRESS_UPDATE, TEN_PROGRESS_DONE,
    TEN_REQUIRES_GRAD, TEN_BACKWARD, TEN_ZERO_GRAD,
    TEN_GRAD_STEP  // updates the tensor in place; used as a statement
};

// ops that return a plain int (a size, a random int, a progress-bar id), not a tensor handle.
// Their result must not be tagged as a tensor slot, or the tensor-free pass would "release" the 
// number 4 as if it were handle 4.
extern const set<TensorOp> TENSOR_INT_OPS = {
    TEN_ROWS, TEN_COLS, TEN_RAND_INT, TEN_PROGRESS_BAR, TEN_LIVE_COUNT,
    TEN_RANK, TEN_DIM
};

// ops that return a scalar double (pushed to dmem, result is a dslot index), as opposed to a 
// tensor handle into tensor_heap. mse_loss/cross_entropy_loss are deliberately NOT here: they always
// return a tensor handle (1x1), in both grad and non-grad mode, so their type doesn't depend on
// a runtime flag.
extern const set<TensorOp> TENSOR_DOUBLE_OPS = {
    TEN_SUM, TEN_MEAN, TEN_MAX, TEN_GET, CON_GET_D, CON_POP_D, CON_MIN_D, CON_MAX_D, CON_SUM_D, OBJ_GET_D
};
