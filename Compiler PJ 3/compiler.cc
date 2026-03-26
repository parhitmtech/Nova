/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include "lexer.h"
#include "compiler.h"

using namespace std;

#define DEBUG 1

extern LexicalAnalyzer lexer;
extern vector<string> errorList;
struct InstructionNode* parse_repl_input(const string& input);

bool suppress_output = false;

// ── Call stack ────────────────────────────────────────────────────────────────
struct CallFrame {
    struct InstructionNode* returnAddress;
    int dest_index;
    int slot_base;
    vector<int> savedSlots;
};

// Global memory vectors

vector<int> mem;
int next_available = 0;

vector<std::string> strMem;
int next_str_available = 0;

vector<int> freeList;

// Slot allocator
int alloc_slot()
{
    if (!freeList.empty())
    {
        int idx = freeList.back();
        freeList.pop_back();
        mem[idx] = 0;
        return idx; 
    }
    mem.push_back(0);
    return next_available++;
}

void free_slot(int idx)
{
    freeList.push_back(idx);
}

// ── Input replay buffer ───────────────────────────────────────────────────────
// Used only during benchmark mode — captures inputs from first run
// and replays them for subsequent timed runs
static vector<int> input_replay_buffer;
static int         input_replay_index = -1;  // -1 = live stdin mode

stack<CallFrame> callStack;

void debug(const char* format, ...)
{
    va_list args;
    if (DEBUG)
    {
        va_start(args, format);
        vfprintf(stdout, format, args);
        va_end(args);
    }
}

void execute_program(struct InstructionNode* program)
{
    struct InstructionNode* pc = program;
    int op1, op2, result;

    while (pc != NULL)
    {
        switch (pc->type)
        {
            case NOOP:
                pc = pc->next;
                break;

            case IN:
            {
                int val = 0;
                if (input_replay_index >= 0)
                {
                    // ── replay mode (benchmark) ───────────────────────────────
                    if (input_replay_index < (int)input_replay_buffer.size())
                        val = input_replay_buffer[input_replay_index++];
                }
                else
                {
                    // ── live stdin mode ───────────────────────────────────────
                    scanf("%d", &val);

                    // capture for potential benchmark replay
                    input_replay_buffer.push_back(val);
                }
                mem[pc->input_inst.var_index] = val;
                pc = pc->next;
                break;
            }

            case OUT:
                if (!suppress_output)
                {
                    if (pc->output_inst.is_string)
                    {
                        // is_string_var: var_index is a mem slot holding a strMem index
                        // !is_string_var: var_index is a direct strMem index (literal)
                        int strIdx = pc->output_inst.is_string_var ? mem[pc->output_inst.var_index] : pc->output_inst.var_index;
                        printf("%s", strMem[strIdx].c_str());
                    }
                    else
                        printf("%d", mem[pc->output_inst.var_index]);
                    if (pc->output_inst.newline)
                        printf("\n");
                    else
                        printf(" ");
                }
                pc = pc->next;
                break;

            case ASSIGN:
                switch (pc->assign_inst.op)
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
                switch (pc->cjmp_inst.condition_op)
                {
                    case CONDITION_GREATER:
                        pc = (op1 > op2) ? pc->next : pc->cjmp_inst.target;
                        break;
                    case CONDITION_LESS:
                        pc = (op1 < op2) ? pc->next : pc->cjmp_inst.target;
                        break;
                    case CONDITION_NOTEQUAL:
                        pc = (op1 != op2) ? pc->next : pc->cjmp_inst.target;
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

            case CALL:
            {
                if (pc->call_inst.function_head == NULL)
                {
                    debug("Error: CALL target function_head is null.\n");
                    exit(1);
                }
                CallFrame frame;
                frame.returnAddress = pc->next;
                frame.dest_index    = pc->call_inst.ret_val_index;
                frame.slot_base     = pc->call_inst.func_slot_base;

                int base  = pc->call_inst.func_slot_base;
                int count = pc->call_inst.func_slot_count;
                for (int i = 0; i < count; i++)
                    frame.savedSlots.push_back(mem[base + i]);

                for (int i = 0; i < pc->call_inst.num_params; i++)
                    mem[pc->call_inst.param_slots[i]] = mem[pc->call_inst.arg_val_slots[i]];

                callStack.push(frame);
                pc = pc->call_inst.function_head;
                break;
            }

            case RET:
            {
                if (callStack.empty())
                {
                    debug("Error: RET with empty call stack.\n");
                    exit(1);
                }
                CallFrame frame = callStack.top();
                callStack.pop();
                
                // capture return value before restoring slots
                int ret_val = mem[pc->ret_inst.ret_val_index];

                // restore function-local slots to their pre-call  values
                for (int i = 0; i < (int)frame.savedSlots.size(); i++)
                    mem[frame.slot_base + i] = frame.savedSlots[i];

                // write return value - caller reads this in the very next ASSIGN
                mem[frame.dest_index] = ret_val;

                // free all function-local slots EXCEPT dest_index
                // dest_index must survive until the callers ASSIGN copies to it
                // that ASSIGN emits no alloc_slot calls, so dest_index is dafe
                int count = (int)frame.savedSlots.size();
                for (int i = 0;i < count;i++)
                {
                    int slot = frame.slot_base + i;
                    if (slot != frame.dest_index)
                    {
                        free_slot(slot);
                    }
                }
                pc = frame.returnAddress;
                break;
            }

            case ARRAY_READ:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n", pc->array_inst.line_no, idx, size);
                    exit(1);
                }
                mem[pc->array_inst.target_index] = mem[base + idx];
                pc = pc->next;
                break;
            }

            case ARRAY_WRITE:
            {
                int base = pc->array_inst.dynamic_base
                           ? mem[pc->array_inst.base_index]
                           : pc->array_inst.base_index;
                int size = (pc->array_inst.size_slot >= 0)
                           ? mem[pc->array_inst.size_slot]
                           : pc->array_inst.array_size;
                int idx  = mem[pc->array_inst.index_slot];
                if (idx < 0 || idx >= size)
                {
                    printf("Runtime error at line %d: array index %d out of bounds (size %d)\n",
                           pc->array_inst.line_no, idx, pc->array_inst.array_size);
                    exit(1);
                }
                mem[base + idx] = mem[pc->array_inst.target_index];
                pc = pc->next;
                break;
            }

            case ALLOC:
            {
                int size = mem[pc->alloc_inst.size_slot];
                if (size <= 0)
                {
                    printf("Runtime error: array size must > 0, got %d\n", size);
                    exit(1);
                }
                int base = next_available;
                for (int i = 0;i < size;i++)
                {
                    alloc_slot();
                }
                mem[pc->alloc_inst.base_slot] = base;
                pc = pc->next;
                break;
            }

            case STRCAT:
            {
                int leftIdx = mem[pc->strcat_inst.left_slot];
                int rightIdx = mem[pc->strcat_inst.right_slot];
                strMem.push_back(strMem[leftIdx] + strMem[rightIdx]);
                int dest = next_str_available++;
                mem[pc->strcat_inst.dest_slot] = dest;
                pc = pc->next;
                break;
            }

            case SCMP:
            {
                int leftIdx = mem[pc->scmp_inst.operand1_index];
                int rightIdx = mem[pc->scmp_inst.operand2_index];
                string& left = strMem[leftIdx];
                string& right = strMem[rightIdx];
                bool pass = false;
                switch (pc->scmp_inst.condition_op)
                {
                    case CONDITION_GREATER: pass = (left > right); break;
                    case CONDITION_LESS: pass = (left < right); break;
                    case CONDITION_NOTEQUAL: pass = (left != right); break;
                }
                pc = pass ? pc->next : pc->scmp_inst.target;
                break;
            }

            default:
                debug("Error: invalid value for pc->type (%d).\n", pc->type);
                exit(1);
                break;
        }
    }
}

// Constant folding 

static vector<bool> constant_slots;

void mark_constants(struct InstructionNode* program)
{
    int n = next_available;
    vector<int> write_count(n, 0);
    constant_slots.assign(n, true);
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN)
            write_count[pc->assign_inst.left_hand_side_index]++;
        if (pc->type == IN)
            write_count[pc->input_inst.var_index]++;
        if (pc->type == ARRAY_READ)
            write_count[pc->array_inst.target_index]++;
        pc = pc->next;
    }

    for (int i = 0;i < n;i++)
    {
        if (write_count[i] > 1) constant_slots[i] = false;
    }

    pc = program;
    while (pc != nullptr)
    {
        if (pc->type == IN)
            constant_slots[pc->input_inst.var_index] = false;
        if (pc->type == ARRAY_READ)
            constant_slots[pc->array_inst.target_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op == OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
            if (!constant_slots[pc->assign_inst.operand1_index] ||
                !constant_slots[pc->assign_inst.operand2_index])
                constant_slots[pc->assign_inst.left_hand_side_index] = false;
        pc = pc->next;
    }
}

int fold_pass(struct InstructionNode* program)
{
    int folds = 0;
    struct InstructionNode* pc = program;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN && pc->assign_inst.op != OPERATOR_NONE)
        {
            int op1_idx = pc->assign_inst.operand1_index;
            int op2_idx = pc->assign_inst.operand2_index;
            int dst_idx = pc->assign_inst.left_hand_side_index;

            if (constant_slots[op1_idx] && constant_slots[op2_idx])
            {
                int op1 = mem[op1_idx], op2 = mem[op2_idx], result = 0;
                switch (pc->assign_inst.op)
                {
                    case OPERATOR_PLUS:  result = op1 + op2; break;
                    case OPERATOR_MINUS: result = op1 - op2; break;
                    case OPERATOR_MULT:  result = op1 * op2; break;
                    case OPERATOR_DIV:
                        if (op2 == 0) { pc = pc->next; continue; }
                        result = op1 / op2; break;
                    default: pc = pc->next; continue;
                }
                mem[dst_idx]                   = result;
                pc->assign_inst.op             = OPERATOR_NONE;
                pc->assign_inst.operand1_index = dst_idx;
                constant_slots[dst_idx]        = true;
                folds++;
            }
        }
        pc = pc->next;
    }
    return folds;
}

int constant_fold(struct InstructionNode* program)
{
    mark_constants(program);
    int total_folds = 0, folds = 0;
    do {
        folds = fold_pass(program);
        total_folds += folds;
    } while (folds > 0);
    return total_folds;
}

int remove_self_copies(struct InstructionNode* head)
{
    int removed = 0;
    struct InstructionNode* pc = head;
    while (pc != nullptr)
    {
        if (pc->type == ASSIGN &&
            pc->assign_inst.op == OPERATOR_NONE &&
            pc->assign_inst.left_hand_side_index == pc->assign_inst.operand1_index)
        {
            pc->type = NOOP;
            removed++;
        }
        pc = pc->next;
    }
    return removed;
}

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
                const char* condStr =
                    pc->cjmp_inst.condition_op == CONDITION_GREATER ? ">"  :
                    pc->cjmp_inst.condition_op == CONDITION_LESS    ? "<"  : "<>";
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
                const char* condStr =
                    pc->scmp_inst.condition_op == CONDITION_GREATER ? ">"  :
                    pc->scmp_inst.condition_op == CONDITION_LESS    ? "<"  : "<>";
                printf("SCMP        if strMem[mem[%d]] %s strMem[mem[%d]]  pass->next  fail->node@%p\n",
                    pc->scmp_inst.operand1_index, condStr,
                    pc->scmp_inst.operand2_index, (void*)pc->scmp_inst.target);
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

void run_repl()
{
    printf("\n");
    printf("  NovaComp v2.0 REPL\n");
    printf("  Type statements one at a time.\n");
    printf("  Type 'exit' or 'quit' to quit.\n");
    printf("\n");
    fflush(stdout);

    string accumulated = "";  // full input collected so far
    int brace_depth = 0;  // tracks open {} to detect multi-line input
    bool in_def = false;  // true when inside a def block

    while (true)
    {
        // show prompt
        if (brace_depth == 0)
            printf(">>> ");
        else
            printf("... ");
        fflush(stdout);

        // read a line
        string line;
        if (!getline(cin, line))
            break;  // EOF

        // check for exit
        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);
        // trim trailing whitespace
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != string::npos) trimmed = trimmed.substr(0, end + 1);


        if (trimmed == "exit" || trimmed == "quit")
        {
            printf("\n  Bye!\n\n");
            break;
        }

        // accumulated line
        accumulated += line + "\n";

        // count brace depth
        for (char c : line)
        {
            if (c == '{') brace_depth++;
            if (c == '}') brace_depth--;
        }

        // brace_depth should never go below 0
        if (brace_depth < 0) brace_depth = 0;

        // if braces are still open - keep reading
        if (brace_depth > 0)
        {
            continue;
        }

        // we have a complete input - parse and execute
        if (accumulated.find_first_not_of(" \t\r\n") == string::npos)
        {
            // blank input - reset and continue
            accumulated = "";
            continue;
        }

        // clear errors from previous iterations
        errorList.clear();

        // parse
        struct InstructionNode* program = parse_repl_input(accumulated);

        if (!errorList.empty())
        {
            for (const string& err : errorList)
                fprintf(stderr, "%s\n", err.c_str());
            errorList.clear();
        }
        else if (program != nullptr)
        {
            // execute
            input_replay_index = -1;  // live stdin mode
            execute_program(program);
        }

        // reset for next input
        accumulated = "";
        brace_depth = 0;
        in_def = false;
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    string inputFile    = "";
    bool flag_dump_ir   = false;
    bool flag_optimize  = false;
    bool flag_benchmark = false;
    bool flag_repl = false;
    int  bench_iters    = 10000;

    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--dump-ir")   { flag_dump_ir   = true; continue; }
        if (arg == "--optimize")  { flag_optimize  = true; continue; }
        if (arg == "--benchmark") { flag_benchmark = true; continue; }
        if (arg == "--repl") { flag_repl = true; continue; }
        if (arg[0] != '-')       { inputFile = arg; continue; }

        if (arg == "--iters" && i + 1 < argc)
        {
            bench_iters = atoi(argv[++i]);
            continue;
        }

        fprintf(stderr, "Unknown flag: %s\n", arg.c_str());
        fprintf(stderr, "Usage: %s [file.csl] [--dump-ir] [--optimize] [--benchmark] [--iters N]\n",
                argv[0]);
        return 1;
    }

    // ── REPL mode ───────────────────────────────────────────
    if (flag_repl)
    {
        run_repl();
        return 0;
    }

    // ── file mode ─────────────────────────────────────────────────────────────
    if (!inputFile.empty())
    {
        if (inputFile.size() < 5 ||
            inputFile.substr(inputFile.size() - 4) != ".csl")
        {
            fprintf(stderr, "Error: file must have a .csl extension\n");
            return 1;
        }
        FILE* test = fopen(inputFile.c_str(), "r");
        if (!test)
        {
            fprintf(stderr, "Error: file '%s' not found\n", inputFile.c_str());
            return 1;
        }
        fclose(test);

        lexer.InitializeFromFile(inputFile);
    }

    // ── parse ─────────────────────────────────────────────────────────────────
    struct InstructionNode* program = parse_generate_intermediate_representation();

    // ── optimize ──────────────────────────────────────────────────────────────
    if (flag_optimize)
    {
        int folds   = constant_fold(program);
        int removed = remove_self_copies(program);
        fprintf(stderr, "FOLDS:%d\n",   folds);
        fprintf(stderr, "REMOVED:%d\n", removed);
    }

    // ── IR dump ───────────────────────────────────────────────────────────────
    if (flag_dump_ir)
        dump_ir(program);

    // ── benchmark mode ────────────────────────────────────────────────────────
    if (flag_benchmark)
    {
        int nodeCount = count_ir_nodes(program);

        // warm-up run — live stdin, captures inputs into input_replay_buffer
        suppress_output = true;
        input_replay_index = -1;        // live stdin mode — fills replay buffer
        input_replay_buffer.clear();
        execute_program(program);

        // switch to replay mode — reuses captured inputs for all timed runs
        input_replay_index = 0;

        auto start = chrono::high_resolution_clock::now();
        for (int i = 0; i < bench_iters; i++)
        {
            input_replay_index = 0;     // rewind replay buffer each iteration
            execute_program(program);
        }
        auto end = chrono::high_resolution_clock::now();
        suppress_output = false;

        double total_us = chrono::duration<double, micro>(end - start).count();
        double avg_us   = total_us / bench_iters;

        printf("BENCH_US:%.3f\n",  avg_us);
        printf("BENCH_NODES:%d\n", nodeCount);
        printf("BENCH_ITERS:%d\n", bench_iters);
    }
    else
    {
        // ── normal single execution ───────────────────────────────────────────
        input_replay_index = -1;  // live stdin mode
        execute_program(program);
    }

    return 0;
}