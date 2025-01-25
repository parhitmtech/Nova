/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include "compiler.h"

using namespace std;

#define DEBUG 1     // 1 => Turn ON debugging, 0 => Turn OFF debugging

int mem[1000];
int next_available = 0;

std::vector<int> inputs;
int next_input = 0;

void debug(const char* format, ...)
{
    va_list args;
    if (DEBUG)
    {
        va_start (args, format);
        vfprintf (stdout, format, args);
        va_end (args);
    }
}

void execute_program(struct InstructionNode * program)
{
    struct InstructionNode * pc = program;
    int op1, op2, result;

    while(pc != NULL)
    {
        switch(pc->type)
        {
            case NOOP:
                pc = pc->next;
                break;
            case IN:

                mem[pc->input_inst.var_index] = inputs[next_input];
                next_input++;
                pc = pc->next;
                break;
            case OUT:
                printf("%d ", mem[pc->output_inst.var_index]);
                pc = pc->next;
                break;
            case ASSIGN:
                switch(pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 + op2;
                        break;
                    case OPERATOR_MINUS:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 - op2;
                        break;
                    case OPERATOR_MULT:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 * op2;
                        break;
                    case OPERATOR_DIV:
                        op1 = mem[pc->assign_inst.operand1_index];
                        op2 = mem[pc->assign_inst.operand2_index];
                        result = op1 / op2;
                        break;
                    case OPERATOR_NONE:
                        op1 = mem[pc->assign_inst.operand1_index];
                        result = op1;
                        break;
                }
                mem[pc->assign_inst.left_hand_side_index] = result;
                pc = pc->next;
                break;
            case CJMP:
                if (pc->cjmp_inst.target == NULL)
                {
                    debug("Error: pc->cjmp_inst->target is null.\n");
                    exit(1);
                }
                op1 = mem[pc->cjmp_inst.operand1_index];
                op2 = mem[pc->cjmp_inst.operand2_index];
                switch(pc->cjmp_inst.condition_op)
                {
                    case CONDITION_GREATER:
                        if(op1 > op2)
                            pc = pc->next;
                        else
                            pc = pc->cjmp_inst.target;
                        break;
                    case CONDITION_LESS:
                        if(op1 < op2)
                            pc = pc->next;
                        else
                            pc = pc->cjmp_inst.target;
                        break;
                    case CONDITION_NOTEQUAL:
                        if(op1 != op2)
                            pc = pc->next;
                        else
                            pc = pc->cjmp_inst.target;
                        break;
                }
                break;
            case JMP:
  
                if (pc->jmp_inst.target == NULL)
                {
                    debug("Error: pc->jmp_inst->target is null.\n");
                    exit(1);
                }
                pc = pc->jmp_inst.target;
                break;
            default:
                debug("Error: invalid value for pc->type (%d).\n", pc->type);
                exit(1);
                break;
        }
    }
}

int main()
{
    struct InstructionNode * program;
    program = parse_generate_intermediate_representation();
    execute_program(program);
    return 0;
}
