// The IR interpreter (execute_program). Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "lexer.h"
#include "compiler.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"
#include "interp_internal.h"
#include "debugger.h"

using namespace std;


void execute_program(struct InstructionNode* program)
{
    struct InstructionNode* pc = program;

    // Infinite loop detection:
    // Simple (non-tensor) instructions execute millions/sec in tight loops.
    // ML training with tensor ops executes only ~100K nodes for 500 epochs.
    // Limit: 500M simple instructions before aborting.
    // Each TENSOR_CALL grants +10M extra budget (rewards real ML work).
    static const long long SIMPLE_LIMIT = 500'000'000LL;
    static const long long TENSOR_BONUS =  10'000'000LL;
    long long simple_budget = SIMPLE_LIMIT;

    while (pc != NULL)
    {
        if (debug_mode) debugger_at(pc);  // the step debugger: stops here when stepping or at a breakpoint
        // Decrement budget for every instruction.
        // TENSOR_CALL grants a large bonus so ML programs are never falsely killed.
        if (pc->type == TENSOR_CALL)
            simple_budget += TENSOR_BONUS;
        else
            simple_budget--;

        if (simple_budget <= 0)
        {
            runtime_failure(DiagCode::ExecutionLimit, pc->line_no, "execution limit reached (about 500 million "
                          "instructions): check for an infinite loop (tensor work raises the limit, so training is "
                          "not stopped)");
        }
        runtime_pc = pc;  // where a runtime error is reported

        switch (pc->type)  // main instruction dispatch switch
        {
            case NOOP:      pc = pc->next; break;
            case IN:            pc = exec_in(pc); break;
            case OUT:           pc = exec_out(pc); break;
            case ASSIGN:        pc = exec_assign(pc); break;
            case CJMP:          pc = exec_cjmp(pc); break;
            case JMP:           pc = exec_jmp(pc); break;
            case CALL:          pc = exec_call(pc); break;
            case RET:           pc = exec_ret(pc); break;
            case ARRAY_READ:    pc = exec_array_read(pc); break;
            case ARRAY_WRITE:   pc = exec_array_write(pc); break;
            case ALLOC:         pc = exec_alloc(pc); break;
            case STRCAT:        pc = exec_strcat(pc); break;
            case SCMP:          pc = exec_scmp(pc); break;
            case ASSIGN_F:      pc = exec_assign_f(pc); break;
            case ASSIGN_D:      pc = exec_assign_d(pc); break;
            case CAST:          pc = exec_cast(pc); break;
            case TENSOR_CALL:   pc = exec_tensor_call(pc); break;
            case TENSOR_RELEASE: pc = exec_tensor_release(pc); break;
            case HF_INFER:      pc = exec_hf_infer(pc); break;
            case SOL_CALL:      pc = exec_sol_call(pc); break;
            default:
                debug("Error: invalid value for pc->type (%d).\n", pc->type);
                exit(1);
                break;
        }
    }
}
