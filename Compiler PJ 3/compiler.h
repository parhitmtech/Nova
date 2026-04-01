/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#ifndef _COMPILER_H_
#define _COMPILER_H_

#include <string>
#include <vector>

extern std::vector<int> mem;  // int memory - grows on demand
extern int next_available;

extern std::vector<std::string> strMem;  // string memory - grows on demand
extern int next_str_available;

extern std::vector<int> freeList;  // recycled int slots

// Slot Allocator
// Use these everywhere instead of raw next_available++
int alloc_slot();  // reuse freed slot grow mem
void free_slot(int idx);  // return slot to free list

// Variable type system 
enum VarType {
    TYPE_UNKNOWN = 0,   // untyped (auto-declared) — no type checking
    TYPE_INT,           // int x = 5 ;
    TYPE_BOOL,          // bool flag = true ;
    TYPE_STRING         // string s = "hello" ;
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
    ARRAY_READ,
    ARRAY_WRITE,
    ALLOC,
    STRCAT,
    SCMP
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

struct InstructionNode* parse_generate_intermediate_representation();
void generate_x86(struct InstructionNode* program, const std::string& outputFile);

#endif /* _COMPILER_H_ */