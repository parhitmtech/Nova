/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Parser state: symbol tables, function/array/struct/class tables, tensor built-in tables. Split out of parser.cc.
#include "parser_internal.h"

vector<string> errorList;
bool insideFunction = false;
int  currentFuncRetIdx = -1;

map<string, int> symbolTable;
map<string, int> floatSymbolTable;
map<string, int> doubleSymbolTable;

// mem[] slots known to hold a tensor handle (as opposed to an ordinary int).
// Tensors have their own type (Tensor) in the language, but at run time a handle is an int in a mem[]
// slot, and temporaries have no declared type, so the slots are tracked here; this drives the compiler-inserted tensor-free
// pass (insert_tensor_releases in compiler.cc), which must never release a slot that isn't actually 
// a tensor handle. Populated at every tensor-call result, and propogated through plain copies (declaration and 
// reassignments); extended across function calls by propagate_tensor_slots_across_calls() after 
// parsing completes.  
std::set<int> g_tensorSlots;
// mem[] slots that hold object state:each class instance's int fields (g_fieldSlots) and each 
// method's "self" copies of them (g_selfSlots). The tensor-free pass never releases these by liveness: 
// an object owns what its fields point to until the field is overwritten.
std::set<int> g_fieldSlots;
std::set<int> g_selfSlots;

LexicalAnalyzer lexer;
Token token;

map<string, InstructionNode*> functionTable;
map<string, vector<string>> functionParams;
map<string, int> functionReturnIndex;
map<string, int> arrayTable;
map<string, int> arraySizeTable;
map<string, bool> arrayDynamic;  // true = runtime-sized, base is in a mem slot
map<string, pair<int,int>> functionSlotRange;

map<string, VarType> typeTable;
map<string, VarType> functionReturnType;
map<string, vector<VarType>> functionParamTypes;
map<string, vector<int>> functionParamSlots;  // param slot indices, persisted after symbol table restore

VarType lastExprType = TYPE_UNKNOWN;
VarType currentFuncRetType = TYPE_UNKNOWN;

// Tensor built-in dispatch table
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
    {"tensor_reshape", TEN_RESHAPE}
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
// numbre 4 as if it was handle 4.
extern const set<TensorOp> TENSOR_INT_OPS = {
    TEN_ROWS, TEN_COLS, TEN_RAND_INT, TEN_PROGRESS_BAR, TEN_LIVE_COUNT,
    TEN_RANK, TEN_DIM
};

// ops that return a scalar double (pushed to dmem, result is a dslot index), as opposed to a 
// tensor handle into tensor_heap. mse_loss/cross_entropy_loss are deliberately NOT here: they always
// return a tensor handle (1x1), in both grad and non-grad mode, so their type doesn't depend on
// a runtime flag.
extern const set<TensorOp> TENSOR_DOUBLE_OPS = {
    TEN_SUM, TEN_MEAN, TEN_MAX, TEN_GET
};


map<string, StructDef> structTable;  // struct name -> defnition
map<string, string> varStructType;  // var name -> struct type name
map<string, map<string, int>> structFieldSlots;  // var -> {dotted path -> slot}
map<string, map<string, VarType>> structFieldTypes;  // var -> {dotted path -> type}

// class support - parser local state
map<string, int> currentSelfFieldSlots;
map<string, VarType> currentSelfFieldTypes;
map<string, int> currentSelfFieldArraySizes;
map<string, map<string, map<string, int>>> classMethodArraySizes;
map<string, map<string, vector<int>>> classMethodParamSlots;
string currentSelfClassName = "";

// function multi-return support
map<string, vector<int>>functionReturnSlots;  // multi-return slots per function
map<string, vector<VarType>> functionReturnTypesList;  // multi-return types per function
vector<int> currentFuncRetSlots;  // active function's return slots
vector<VarType> currentFuncRetTypes;  // active function's return types
string currentFunctionLabel;  // the function or method being parsed, for messages: "f" or "Linear.forward"
