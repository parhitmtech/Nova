/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#ifndef _COMPILER_H_
#define _COMPILER_H_

#include <string>
#include <vector>
#include <map>
#include <set>
#include <string>

extern std::vector<int> mem;  // int memory - grows on demand
extern int next_available;

extern std::vector<std::string> strMem;  // string memory - grows on demand
extern int next_str_available;

extern std::vector<int> freeList;  // recycled int slots

extern std::vector<float> fmem;
extern int next_float_available;

extern std::vector<double> dmem;
extern int next_double_available;

// Slot Allocator
// Use these everywhere instead of raw next_available++
int alloc_slot();  // reuse freed slot grow mem
void free_slot(int idx);  // return slot to free list
int alloc_float_slot();  // reuse freed slot grow fmem
int alloc_double_slot();  // reuse freed slot grow dmem

// Variable type system 
enum VarType {
    TYPE_UNKNOWN = 0,    // untyped (auto-declared) — no type checking
    TYPE_INT,            // int x = 5 ;
    TYPE_BOOL,           // bool flag = true ;
    TYPE_STRING,         // string s = "hello" ;
    TYPE_FLOAT,          // float x = 3.14
    TYPE_DOUBLE,         // double x = 3.14159265358979
    TYPE_CLASS,           // class instance
    TYPE_TENSOR,         // tensor handle (index into tensor_heap)
    TYPE_VECTOR,         // vector<T> handle (index into container_heap); T is in the parser's containerTable
    TYPE_MAP,            // map<K, V> handle
};

// Arithmetic operators 
enum ArithmeticOperatorType {
    OPERATOR_NONE = 123,
    OPERATOR_PLUS,
    OPERATOR_MINUS,
    OPERATOR_MULT,
    OPERATOR_DIV
};

// Condition operators 
enum ConditionalOperatorType {
    CONDITION_GREATER = 345,
    CONDITION_LESS,
    CONDITION_NOTEQUAL,
    CONDITION_EQUAL,  // "==" (added after the others so existing values don't change)
    CONDITION_GREATER_EQUAL,  // ">="
    CONDITION_LESS_EQUAL,     // "<="
};

// Tensor operation codes
enum TensorOp {
    TEN_ALLOC = 0,  // tensor(rows, cols)
    TEN_ZEROS,      // tensor_zeros(rows, cols)
    TEN_ONES,       // tensor_ones(rows, cols)
    TEN_RANDN,      // tensor_randn(rows, cols) - random normal
    TEN_XAVIER,     // tensor_xavier(rows, cols) - xavier normal
    TEN_GET,        // tensor_get(t, i, j) -> scalar in mem[]
    TEN_SET,        // tensor_set(t, i, j, v) -> void
    TEN_ROWS,       // tensor_rows(t) -> scalar
    TEN_COLS,       // tensor_cols(t) -> scalar
    TEN_PRINT,      // tensor_print(t) - void
    TEN_MATMUL,     // matmul(a, b) -> handle
    TEN_ADD,        // tensor_add(a, b) -> handle
    TEN_SUB,        // tensor_sub(a, b) -> handle
    TEN_MUL,        // tensor_mul(a, b) -> Hadamard product
    TEN_SCALE,      // tensor_scale(t, s) -> handle
    TEN_T,          // tensor_T(t) -> transposed handle
    TEN_SUM,        // tensor_sum(t) -> scalar (written as double to dmem)
    TEN_MEAN,       // tensor_mean(t) -> scalar
    TEN_MAX,        // tensor_max(t) -> scalar
    TEN_CLIP,       // tensor_clip(t, lo, hi) -> handle
    TEN_RELU,       // relu(t) -> handle
    TEN_SIGMOID,    // sigmoid(t) -> handle
    TEN_TANH,       // tanh_t(t) -> handle
    TEN_SOFTMAX,    // softmax(t) - row-wise softmax -> handle
    TEN_RELU_GRAD,  // relu_grad(t) -> handle
    TEN_SIGMOID_GRAD,  // sigmoid_grad(t) -> handle
    TEN_MSE_LOSS,   // mse_loss(pred, target) -> scalar (double)
    TEN_MSE_GRAD,   // mse_grad(pred, target) -> handle
    TEN_BCE_GRAD,   // bce_grad(pred, target) -> handle 
    TEN_CE_LOSS,    // cross_entropy_loss(logits, target) -> scalar
    TEN_CE_GRAD,    // cross_entropy_grad(logits, target) -> handle
    TEN_SUM_AXIS0,   // tensor_sum_rows(t) -> 1×N handle (sum along axis 0)
    TEN_RAND_INT,
    TEN_GET_ROW, 
    TEN_SET_ROW,
    TEN_SLICE_ROWS,
    TEN_LOAD_CSV,
    TEN_PROGRESS_BAR,
    TEN_PROGRESS_UPDATE,
    TEN_PROGRESS_DONE,
    //Autograd ops
    TEN_REQUIRES_GRAD, // requires_grad(t) -> void
    TEN_BACKWARD,  // backward(loss) -> void
    TEN_TENSOR_GRAD,  // tensor_grad(t) -> handle
    TEN_ZERO_GRAD,  // zero_grad(t) -> void
    TEN_GRAD_STEP,  // grad_step(t, lr) -> void (updates t in place, clears its grad)
    TEN_LIVE_COUNT, // tensor_live_count() -> int: tensors not yet freed (for leak tests)
    TEN_RANK,  // tensor_rank(t) -> int: number of dimensions
    TEN_DIM,  // tensor_dim(t, i) -> int: size of dimension i (0 = outermost)
    TEN_RESHAPE,  // tensor_reshape(t, d0, d1, ...) -> handle; one dimension may be -1 (inferred)
    TEN_ADD_SCALAR, // t + k: k added to every element (also t - k, k + t, k - t via the parser)
    TEN_DIV,   // a / b element-wise, same shape
    // Built-in containers (src/runtime/interp_containers.cc). Argument 0 is the container handle.
    CON_VEC_NEW,  // vector<T>(): args T -> handle
    CON_MAP_NEW,  // map<K, V>(): args K, V -> handle
    CON_PUSH,     // v.push(x)
    CON_POP,      // v.pop() -> the last element (mem[])
    CON_POP_D,    // the same for a float / double element (dmem[])
    CON_GET,      // v[i], m[k] -> element (mem[])
    CON_GET_D,    // the same for a float / double element (dmem[])
    CON_SET,      // v[i] = x, m[k] = x
    CON_SIZE,     // .size() -> int
    CON_HAS,      // m.has(k) -> bool
    CON_REMOVE,   // m.remove(k)
    CON_CLEAR,    // .clear() 
};

// ── IR instruction types ──────────────────────────────────────────────────────
enum InstructionType
{
    NOOP = 1000,
    IN, OUT, ASSIGN, CJMP, JMP, CALL, RET,
    ARRAY_READ, ARRAY_WRITE, ALLOC, STRCAT, SCMP,
    ASSIGN_F,  // float-arithmetic/assignment 
    ASSIGN_D,  // double arithmetic/assignment
    CAST,    // explicit type cast
    TENSOR_CALL,  // tensor built-in operation
    SPILL_LOAD,   // load spilled slot from stack into temp slot
    SPILL_STORE,  // store temp slot back to stack
    HF_INFER,     // HuggingFace Inference API call
    SOL_CALL,     // SOL HPC cluster operation
    TENSOR_RELEASE,  // drop one refcount on the tensor handle in mem[slot]
};

enum HFOp {
    HF_OP_INFER = 0,   // hf.infer(model, input)
    HF_OP_GENERATE,    // hf.generate(model, prompt, max_tokens)
    HF_OP_CLASSIFY,    // hf.classify(model, text)
    HF_OP_SET_TOKEN,   // hf.set_token(token_str)
    HF_OP_DATASET,     // hf.dataset(name, split) → string handle "name:split"
};

enum SolOp {
    SOL_OP_FINETUNE = 0,  // sol.finetune(model, task, dataset, epochs, lr, batch_size)
    SOL_OP_WAIT,          // sol.wait(job)
    SOL_OP_PREDICT,       // sol.predict(job, input)
    SOL_OP_SAVE,          // sol.save(job, path)
    SOL_OP_SET_KEY,       // sol.set_key(path)
};

struct InstructionNode
{
    InstructionType type;
    int line_no;
    union
    {
        struct
        {
            int left_hand_side_index;
            int operand1_index;
            int operand2_index;
            ArithmeticOperatorType op;
            // Object-field ownership, set by insert_tensor_releases():
            // 0 = ordinary copy, 1 = field store (release the field's old 
            // tensor, retain the new one), 2 = field move (method copy-in / 
            // copy-back: ownership moves with the value , no refcount change).
            int own_kind;
        } assign_inst;

        struct
        {
            int var_index;
        } input_inst;

        struct
        {
            int var_index;
            bool is_string;      // true = string output (not int)
            bool is_string_var;  // true = var_index is a mem slot holding a strMem index
                                 //        (used for string variables)
                                 // false = var_index is a direct strMem index
                                 //        (used for string literals)
            bool newline;
            VarType value_type;  // TYPE_INT, TYPE_FLOAT, TYPE_DOUBLE, TYPE_STRING
        } output_inst;

        struct {
            ConditionalOperatorType condition_op;
            int operand1_index;
            int operand2_index;
            struct InstructionNode* target;
            VarType operand_type;  // TYPE_INT (default), TYPE_FLOAT or TYPE_DOUBLE.
            // Comparisons previously always read mem[] (int space) regardless of operand type, so
            // comparing float/double variables in a condition silently compared the wrong memory, and for
            // doubles, whose slot indices run past mem[]'s size, could read out of bounds. Must be set explitcitly
            // at every CJMP construction site; TYPE_INT is the correct default since int is the common case
            // and matches every pre-existing CJMP's behavior. 
        } cjmp_inst;

        struct {
            struct InstructionNode* target;
        } jmp_inst;

        struct {
            struct InstructionNode* function_head;
            VarType* param_types;
            VarType* arg_types;
            int ret_val_index;
            int func_slot_base;
            int func_slot_count;
            int num_params;
            int* param_slots;
            int* arg_val_slots;
            int* all_ret_slots;
            int num_ret_slots;
            // Type of ret_val_index. mem[], fmem[], dmem[] are seperately slot spaces.
            // TYPE_UNKNOWN (0) = old mem[] behavior.
            VarType ret_type;
            // Type of each slot in all_ret_slots (null when it is null).
            VarType* ret_slot_types;
        } call_inst;

        struct {
            int ret_val_index;
            // Type of ret_val_index (which slot space to read)
            VarType ret_type;
        } ret_inst;

        struct {
            int base_slot;
            int size_slot;
        } alloc_inst;

        struct {
            int dest_slot;  // mem slot to store new strMem index
            int left_slot;  // mem slot holding left string's strMem index
            int right_slot;  // mem slot holding right string's strMem index
        } strcat_inst;

        struct {
            ConditionalOperatorType condition_op;
            int operand1_index;  // mem slot holding left strMem index
            int operand2_index;  // mem slot handling right strMem index
            struct InstructionNode* target;  // jump target on conditoin FAIL
        } scmp_inst;

        struct {
            int left_hand_side_index;   // fmem index
            int operand1_index;  // fmem index
            int operand2_index;  // fmem index
            ArithmeticOperatorType op;
        } assign_f_inst;

        struct {
            int left_hand_side_index;  // dmem index
            int operand1_index;  // dmem index
            int operand2_index;  // dmem index
            ArithmeticOperatorType op;
        } assign_d_inst;

        struct {
            int src_index;  // source slot index
            int dst_index;  // destination slot index
            VarType src_type;  // TYPE_INT / TYPE_FLOAT / TYPE_DOUBLE
            VarType dst_type;  // TYPE_INT / TYPE_FLOAT / TYPE_DOUBLE
        } cast_inst;

        struct {
            int base_index;
            bool dynamic_base;
            int index_slot;
            int target_index;
            int array_size;
            int size_slot;
            int line_no;
        } array_inst;

        struct {
            TensorOp op;
            int result_slot;   // mem[] slot for returned handle/scalar; -1 if void
            int num_args;
            int arg_slots[8];       // slot index per arg (mem[], fmem[], or dmem[] depending on type)
            VarType arg_types[8];   // type tag so executor knows which array to read
        } tensor_call_inst;

        struct {
            int spill_slot;  // the spilled slot index (stack location)
            int temp_slot;   // temporary register slot
        } spill_inst;

        struct {
            HFOp op;
            int model_slot;   // mem slot holding strMem index for model/token string
            int input_slot;   // mem slot holding strMem index for input (-1 if none)
            int result_slot;  // mem slot to receive strMem index result (-1 if void)
            int max_tokens;   // for generate: max new tokens (default 100)
        } hf_inst;

        struct {
            SolOp op;
            int job_slot;      // mem slot for job handle (output for finetune, input for others)
            int model_slot;    // mem slot → strMem for model name (-1 if n/a)
            int task_slot;     // mem slot → strMem for task type (-1 if n/a)
            int dataset_slot;  // mem slot → strMem for "name:split" (-1 if n/a)
            int epochs;        // training epochs
            int batch_size;    // batch size
            float lr;          // learning rate
            int input_slot;    // mem slot → strMem for predict input (-1 if n/a)
            int result_slot;   // mem slot for predict/dataset result (-1 if void)
            int path_slot;     // mem slot → strMem for save path (-1 if n/a)
        } sol_inst;

        struct {
            int slot;  // mem slot holding the tensor handle to release
        } tensor_release_inst;
    };

    struct InstructionNode* next;
};

void debug(const char* format, ...);

struct ClassFieldInfo {
    std::string name;  // dotted for nested: "a.x"
    VarType type;
    std::string struct_type;  // non-empty if field is a struct/class
    int array_size;  // 0 = scalar field, N > 0 = fixed int array of size N
};

struct ClassDef {
    std::string name;
    std::string parent;
    std::vector<ClassFieldInfo> fields;  // flattened: inherited first, then own
    std::vector<std::string> methods;
};

struct Tensor {
    // 2-D view used by every existing op: cols = the last dimension, rows = the product of all the 
    // others (1 for a 1-D tensor). For a 2-D tensor this is exactly rows x cols, as before. Never set 
    // tese directly: use tensor_set_shape() so shape, strides, rows, and cols stay in step.
    int rows, cols;
    std::vector<int> shape;  // full N-D shape, outermost first ({rows, cols}) for 2-D)
    std::vector<int> strides;  // row-major strides in elements (last one is 1)
    std::vector<double> data;
    int refCount = 0;  // number of mem[] slots currently holding this handle
    bool freed = false;  // true once refCount hit 0 and data was released
    double& at(int i, int j)  { return data[i * cols + j]; }
    const double& at(int i, int j) const { return data[i * cols + j]; }
    int ndim() const { return (int)shape.size(); }
};

// Set a tensor's N-D shape: recomputes strides and the rows/cols 2-D view.
void tensor_set_shape(Tensor& t, const std::vector<int>& shape);

extern std::map<std::string, ClassDef> classTable;
extern std::map<std::string, std::string> varClassType;
extern std::map<std::string, std::map<std::string, int>> classFieldSlots;
extern std::map<std::string, std::map<std::string, VarType>> classFieldTypes;
extern std::map<std::string, std::map<std::string, InstructionNode*>> classMethodTable;
extern std::map<std::string, std::map<std::string, std::vector<std::string>>> classMethodParams;
extern std::map<std::string, std::map<std::string, std::vector<VarType>>> classMethodParamTypes;
extern std::map<std::string, std::map<std::string, VarType>> classMethodReturnType;
extern int currentSelfBase;
extern std::string currentSelfName;
extern std::map<std::string, std::map<std::string, std::map<std::string, int>>> classMethodSelfSlots;
extern std::map<std::string, std::map<std::string, std::map<std::string, VarType>>> classMethodSelfTypes;

extern std::vector<Tensor> tensor_heap;
extern std::set<int> g_fieldSlots;  //  instance int fields (parser.cc) 
extern std::set<int> g_selfSlots;  //  method self copies of int fields (parser.cc)
extern std::set<int> g_tensorSlots;  // mem[] slots know to hold a tensor handle (parser.cc)
int alloc_tensor(int rows, int cols);  // allocates and returns handle index; refcount starts at 1
int alloc_tensor_nd(const std::vector<int>& shape);  // same, for any number of dimensions
int alloc_tensor_like(int src);  // new tensor with src's N-D shape (contents zero)
static const int TENSOR_MAX_RANK = 6;  // limit set by the 8-argument tensor-call slots
void tensor_retain(int h);  // a second mem[] slot now also holds this handle
void tensor_release(int h);  // one fewer slot holds this handle; frees data at refcount 0
bool tensor_is_live(int h);  // false if h is out of range or already freed

extern std::map<std::string, InstructionNode*> functionTable;
// funcName -> return type / param types. Used to keep float, double and tensor functions out of the
// int-only memo cache.
extern std::map<std::string, VarType> functionReturnType;
extern std::map<std::string, std::vector<VarType>> functionParamTypes;
 
struct InstructionNode* parse_generate_intermediate_representation();
void execute_program(struct InstructionNode* head);

#endif /* _COMPILER_H_ */