/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#ifndef _COMPILER_H_
#define _COMPILER_H_

#include <string>
#include <vector>

extern int mem[1000];  // global memory
extern int next_available;

extern std::string strMem[1000];  // global string memory
extern int next_str_available;

extern std::vector<int> inputs;
extern int next_input;

enum ArithmeticOperatorType {
    OPERATOR_NONE = 123,
    OPERATOR_PLUS,
    OPERATOR_MINUS,
    OPERATOR_MULT,
    OPERATOR_DIV
};

enum ConditionalOperatorType {
    CONDITION_GREATER = 345,
    CONDITION_LESS,
    CONDITION_NOTEQUAL
};

enum InstructionType
{
    NOOP = 1000,
    IN, OUT, ASSIGN, CJMP, JMP, CALL, RET,
    ARRAY_READ,
    ARRAY_WRITE
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
            
            /*
             * If op == OPERATOR_NONE then only operand1 is meaningful.
             * Otherwise both operands are meaningful
             */
            ArithmeticOperatorType op;
        } assign_inst;
        
        struct
        {
            int var_index;
        } input_inst;
        
        struct
        {
            int var_index;
            bool is_string;  // true = print from strMem, false = print form mem
            bool newline; // for println
        } output_inst;
        
        struct {
            ConditionalOperatorType condition_op;
            int operand1_index;
            int operand2_index;
            struct InstructionNode * target;
        } cjmp_inst;
        
        struct {
            struct InstructionNode * target;
        } jmp_inst;

        struct {
            struct InstructionNode* function_head;
            int ret_val_index;  // index of the local function stack
            int func_slot_base;  // first local slot of function
            int func_slot_count;  // total local slots to save/restore
            int num_params;
            int * param_slots;  // which slots are params
            int * arg_val_slots;  // computed arg values to copy into params 
        } call_inst;

        struct {
            int ret_val_index;  // memory index of the return value in global stack
        } ret_inst;

        struct {
            int base_index;  // start of array in mem[]
            int index_slot;  // mem slot containing the run time index value
            int target_index;  // READ: where to store the result | Write: value to write
            int array_size;  // for full runtime error checking
        } array_inst;
    };

    struct InstructionNode * next; // next statement in the list or NULL
};

void debug(const char* format, ...);

//---------------------------------------------------------
// You should write the following function:

struct InstructionNode * parse_generate_intermediate_representation();

/*
  NOTE:

  You need to write a function with the above signature. This function
  is supposed to parse the input program and generate an intermediate
  representation for it. The output of this function is passed to the
  execute_program function in main().

  Write your code in a separate file and include this header file in
  your code.
*/

#endif /* _COMPILER_H_ */
