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
    CONDITION_NOTEQUAL
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
    TEN_GRAD_STEP,  // grad_step(t, lr) -> handle
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
        } call_inst;

        struct {
            int ret_val_index;
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
    int rows, cols;
    std::vector<double> data;
    double& at(int i, int j)  { return data[i * cols + j]; }
    const double& at(int i, int j) const { return data[i * cols + j]; }
};

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
int alloc_tensor(int rows, int cols);  // allocates and returns handle index

extern std::map<std::string, InstructionNode*> functionTable;

struct InstructionNode* parse_generate_intermediate_representation();
void execute_program(struct InstructionNode* head);
void generate_x86(struct InstructionNode* program, const std::string& outputFile);

#endif /* _COMPILER_H_ */