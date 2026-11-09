// IR inspection: node count and the --dump-ir printer. Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "optimizer.h"

using namespace std;

int count_ir_nodes(struct InstructionNode* program)
{
    int count = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr) 
    { 
        if (pc->type != NOOP)
            count++; 
        pc = pc->next; 
    }
    return count;
}

static const char* cond_str(ConditionalOperatorType op)
{
    switch (op)
    {
        case CONDITION_GREATER:       return ">";
        case CONDITION_LESS:          return "<";
        case CONDITION_EQUAL:         return "==";
        case CONDITION_GREATER_EQUAL: return ">=";
        case CONDITION_LESS_EQUAL:    return "<=";
        default:                      return "!=";
    }
}

void dump_ir(struct InstructionNode* program)
{
    printf("\n");
    printf("===================================================\n");
    printf("  NovaComp IR Dump\n");
    printf("===================================================\n");

    struct InstructionNode* pc = program;
    int nodeIndex = 0;

    while (pc != NULL)
    {
        if (pc->type == NOOP)
        {
            pc = pc->next;
            continue;
        }
        printf("  [%3d]  ", nodeIndex++);
        switch (pc->type)
        {
            case NOOP: printf("NOOP\n"); break;
            case IN:   printf("IN          -> mem[%d]\n", pc->input_inst.var_index); break;
            case OUT:
                if (pc->output_inst.is_string)
                    printf("OUT         strMem[%d] \"%s\"%s\n",
                        pc->output_inst.var_index,
                        strMem[pc->output_inst.var_index].c_str(),
                        pc->output_inst.newline ? "  (newline)" : "");
                else
                    printf("OUT         mem[%d]%s\n",
                        pc->output_inst.var_index,
                        pc->output_inst.newline ? "  (newline)" : "");
                break;
            case ASSIGN:
                if (pc->assign_inst.op == OPERATOR_NONE)
                    printf("ASSIGN      mem[%d] <- mem[%d]\n",
                        pc->assign_inst.left_hand_side_index,
                        pc->assign_inst.operand1_index);
                else
                {
                    const char* opStr =
                        pc->assign_inst.op == OPERATOR_PLUS  ? "+" :
                        pc->assign_inst.op == OPERATOR_MINUS ? "-" :
                        pc->assign_inst.op == OPERATOR_MULT  ? "*" : "/";
                    printf("ASSIGN      mem[%d] <- mem[%d] %s mem[%d]\n",
                        pc->assign_inst.left_hand_side_index,
                        pc->assign_inst.operand1_index, opStr,
                        pc->assign_inst.operand2_index);
                }
                break;
            case CJMP:
            {
                const char* condStr = cond_str(pc->cjmp_inst.condition_op);
                printf("CJMP        if mem[%d] %s mem[%d]  pass->next  fail->node@%p\n",
                    pc->cjmp_inst.operand1_index, condStr,
                    pc->cjmp_inst.operand2_index, (void*)pc->cjmp_inst.target);
                break;
            }
            case JMP:  printf("JMP         -> node@%p\n", (void*)pc->jmp_inst.target); break;
            case CALL:
                printf("CALL        func@%p  ret->mem[%d]  params:%d\n",
                    (void*)pc->call_inst.function_head,
                    pc->call_inst.ret_val_index, pc->call_inst.num_params);
                break;
            case RET:  printf("RET         mem[%d]\n", pc->ret_inst.ret_val_index); break;
            case ARRAY_READ:
                printf("ARRAY_READ  mem[%d + mem[%d]] -> mem[%d]\n",
                    pc->array_inst.base_index, pc->array_inst.index_slot,
                    pc->array_inst.target_index);
                break;
            case ARRAY_WRITE:
                printf("ARRAY_WRITE mem[%d] -> mem[%d + mem[%d]]\n",
                    pc->array_inst.target_index, pc->array_inst.base_index,
                    pc->array_inst.index_slot);
                break;
            case ALLOC:
                printf("ALLOC       base->mem[%d]  size=mem[%d]\n",
                    pc->alloc_inst.base_slot, pc->alloc_inst.size_slot);
                break;
            case STRCAT:
                printf("STRCAT      mem[%d] <- strMem[mem[%d]] + strMem[mem[%d]]\n",
                    pc->strcat_inst.dest_slot,
                    pc->strcat_inst.left_slot,
                    pc->strcat_inst.right_slot);
                break;
            case SCMP:
            {
                const char* condStr = cond_str(pc->scmp_inst.condition_op);
                printf("SCMP        if strMem[mem[%d]] %s strMem[mem[%d]]  pass->next  fail->node@%p\n",
                    pc->scmp_inst.operand1_index, condStr,
                    pc->scmp_inst.operand2_index, (void*)pc->scmp_inst.target);
                break;
            }
            case TENSOR_CALL:
            {
                printf("TENSOR_CALL op=%d result->mem[%d] args=[", pc->tensor_call_inst.op,
                    pc->tensor_call_inst.result_slot);
                for (int i = 0;i < pc->tensor_call_inst.num_args; i++)
                    printf("%s%d", i ? "," : "", pc->tensor_call_inst.arg_slots[i]);
                printf("]\n");
                break;
            }
            case TENSOR_RELEASE:
            {
                printf("TENSOR_RELEASE mem[%d]\n", pc->tensor_release_inst.slot);
                break;
            }
            case ASSIGN_D:
            {
                printf("ASSIGN_D  dmem[%d] <- dmem[%d] op=%d dmem[%d]\n",   
                    pc->assign_d_inst.left_hand_side_index,
                    pc->assign_d_inst.operand1_index,
                    (int)pc->assign_d_inst.op,
                    pc->assign_d_inst.operand2_index);
                break;
            }
            case CAST:
            {
                printf("CAST        %d(type%d) -> %d(type%d)\n",
                    pc->cast_inst.src_index, (int)pc->cast_inst.src_type,
                    pc->cast_inst.dst_index, (int)pc->cast_inst.dst_type);
                break;
            }
            default: printf("UNKNOWN     type=%d\n", pc->type); break;
        }
        pc = pc->next;
    }

    printf("===================================================\n");
    printf("  Total nodes: %d\n", nodeIndex);
    printf("===================================================\n\n");
}
