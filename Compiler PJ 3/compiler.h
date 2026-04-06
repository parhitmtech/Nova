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
    TYPE_CLASS           // class instance
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

// ── IR instruction types ──────────────────────────────────────────────────────
enum InstructionType
{
    NOOP = 1000,
    IN, OUT, ASSIGN, CJMP, JMP, CALL, RET,
    ARRAY_READ, ARRAY_WRITE, ALLOC, STRCAT, SCMP,
    ASSIGN_F,  // float-arithmetic/assignment 
    ASSIGN_D,  // double arithmetic/assignment
    CAST    // explicit type cast
};

struct InstructionNode
{
    InstructionType type;

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
            int ret_val_index;
            int func_slot_base;
            int func_slot_count;
            int num_params;
            int* param_slots;
            int* arg_val_slots;
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

struct InstructionNode* parse_generate_intermediate_representation();
void generate_x86(struct InstructionNode* program, const std::string& outputFile);

#endif /* _COMPILER_H_ */