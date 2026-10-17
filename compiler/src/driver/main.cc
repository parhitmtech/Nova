/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Command-line entry point: parse flags, compile, then run or emit. Split out of compiler.cc.
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
#include "bytecode.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"

using namespace std;

// main

#ifndef NOVA_NOVM_BUILD
int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);  // disable stdout buffering
    setvbuf(stderr, nullptr, _IONBF, 0); // disable stderr buffering

    string inputFile       = "";
    bool flag_dump_ir      = false;
    bool flag_optimize     = false;
    bool flag_benchmark    = false;
    bool flag_repl         = false;
    int  bench_iters       = 10000;
    bool flag_emit_asm     = false;
    bool flag_build        = false;
    bool flag_emit_cuda    = false;
    bool flag_emit_bytecode= false;
    string asmFile         = "";
    string cudaOutputFile  = "";
    string bytecodeOutFile = "";

    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--dump-ir")        { flag_dump_ir        = true; continue; }
        if (arg == "--optimize")       { flag_optimize       = true; continue; }
        if (arg == "--benchmark")      { flag_benchmark      = true; continue; }
        if (arg == "--emit-asm")       { flag_emit_asm       = true; continue; }
        if (arg == "--build")          { flag_build          = true; continue; }
        if (arg == "--repl")           { flag_repl           = true; continue; }
        if (arg == "--debug")          { debug_mode = true; debug_step = true; continue; }
        if (arg == "--emit-cuda")      { flag_emit_cuda      = true; continue; }
        if (arg == "--emit-bytecode")  { flag_emit_bytecode  = true; continue; }
        if (arg[0] != '-')             { inputFile = arg; continue; }

        if (arg == "--iters" && i + 1 < argc)
        {
            bench_iters = atoi(argv[++i]);
            continue;
        }
        if (arg == "-o" && i + 1 < argc)
        {
            bytecodeOutFile = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown flag: %s\n", arg.c_str());
        fprintf(stderr, "Usage: %s [file.csl] [--dump-ir] [--optimize] [--benchmark] [--iters N] [--emit-cuda] [--emit-bytecode] [-o out.nbc]\n",
                argv[0]);
        return 1;
    }

    // REPL mode 
    if (flag_repl)
    {
        run_repl();
        return 0;
    }

    string processed = "";

    // file mode 
    if (!inputFile.empty())
    {
        // Accept both .csl and .nova extensions
        bool validExt = false;
        if (inputFile.size() >= 5 && inputFile.substr(inputFile.size() - 4) == ".csl") validExt = true;
        if (inputFile.size() >= 6 && inputFile.substr(inputFile.size() - 5) == ".nova") validExt = true;
        if (!validExt)
        {
            fprintf(stderr, "Error: file must have a .csl or .nova extension\n");
            return 1;
        }
        FILE* test = fopen(inputFile.c_str(), "r");
        if (!test)
        {
            fprintf(stderr, "Error: file '%s' not found\n", inputFile.c_str());
            return 1;
        }
        fclose(test);

        string raw_source = read_file(inputFile);
        string base_dir = inputFile;
        size_t last_slash = base_dir.find_last_of("/\\");
        base_dir = (last_slash != string::npos) ? base_dir.substr(0, last_slash) : ".";
        set<string> already_imported;
        processed = preprocess_import(raw_source, base_dir, already_imported);
        lexer.ReinitializeFromString(processed);
    }
    else
    {
        // stdin mode (./compiler < program.csl)
        ostringstream raw_stream;
        raw_stream << cin.rdbuf();
        string raw_source = raw_stream.str();
        set<string> already_imported;
        processed = preprocess_import(raw_source, ".", already_imported);
        lexer.ReinitializeFromString(processed);
    }

    //Calculate how many lines were added by imports
    if (debug_mode)
    {
        int processed_lines = (int)std::count(processed.begin(), processed.end(), '\n');
        int raw_lines = 0;
        if (!inputFile.empty())
        {
            string raw = read_file(inputFile);
            raw_lines = (int)std::count(raw.begin(), raw.end(), '\n');
        }
        import_line_offset = std::max(0, processed_lines - raw_lines);
    }

    // parse 
    struct InstructionNode* program = parse_generate_intermediate_representation();

    if (flag_dump_ir)
    {
        LivenessResult lr = compute_liveness(program);
        dump_liveness(program, lr);
        InterferenceGraph ig = build_interference_graph(program, lr);
        dump_interference_graph(ig);
        ColoringResult cr = color_graph(ig);
        dump_coloring(cr);

        // Also test on fib function body if it exists
        // Liveness + interference for FIB function
        if (functionTable.count("fib"))
        {
            LivenessResult lr2 = compute_liveness(functionTable["fib"]);
            dump_liveness(functionTable["fib"], lr2);
            InterferenceGraph ig2 = build_interference_graph(functionTable["fib"], lr2);
            dump_interference_graph(ig2);
            ColoringResult cr2 = color_graph(ig2);
            dump_coloring(cr2);

            // Dump fib IR
            printf("\n-- fib IR \n");
            dump_ir(functionTable["fib"]);
        }
    }

    // // Temporary debug — check if symbolTable has anything
    // fprintf(stderr, "DEBUG symbolTable size: %d\n", (int)symbolTable.size());
    // for (auto& kv : symbolTable)
    //     fprintf(stderr, "  %s -> slot %d\n", kv.first.c_str(), kv.second);

    if (debug_mode)
    {
        debug_symbol_snapshot.clear();
        for (auto& kv : symbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_INT};
        }
        for (auto& kv : floatSymbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_FLOAT};
        }
        for (auto& kv : doubleSymbolTable)
        {
            debug_symbol_snapshot[kv.first] = {kv.second, TYPE_DOUBLE};
        }
    }

    // optimize 
    if (flag_optimize)
    {
        int folds   = constant_fold(program);
        int removed = remove_self_copies(program);
        int simplified = algebraic_simplify(program);
        removed += remove_self_copies(program);
        int propogated = copy_propagate(program);
        removed += remove_self_copies(program);
        int eliminated = dead_code_eliminate(program);
        removed += remove_self_copies(program);
        int hoisted = loop_invariant_code_motion(program);
        int inlined = inline_functions(program);
        // removed += remove_self_copies(program);
        (void)folds; (void)removed; (void)simplified; (void)propogated;
        (void)eliminated; (void)hoisted; (void)inlined;
    }

    // Identify pure + recursive functions for automatic memoization
    detect_memoizable_functions();

    // Compiler-inserted tensor frees (liveness-based, refcounted).
    // Runs after detect_memoizable_functions() so it can strip any tensor-tainted-parameter
    // function back out of pure_functions, and after the --optimize block so it sees the IR that will
    // actually run.
    insert_tensor_releases(program);

    // CUDA codegen — output .cu file and exit (does not execute the program)
    if (flag_emit_cuda)
    {
        if (inputFile.empty())
        {
            fprintf(stderr, "Error: --emit-cuda requires an input file\n");
            return 1;
        }
        cudaOutputFile = inputFile;
        size_t dot = cudaOutputFile.rfind('.');
        if (dot != string::npos) cudaOutputFile = cudaOutputFile.substr(0, dot);
        cudaOutputFile += ".cu";
        generate_cuda(program, cudaOutputFile);
        return 0;
    }

    // Bytecode emit — serialize IR to .nbc and exit
    if (flag_emit_bytecode)
    {
        if (bytecodeOutFile.empty())
        {
            if (inputFile.empty())
            {
                fprintf(stderr, "Error: --emit-bytecode requires an input file or -o <out.nbc>\n");
                return 1;
            }
            bytecodeOutFile = inputFile;
            size_t dot = bytecodeOutFile.rfind('.');
            if (dot != string::npos) bytecodeOutFile = bytecodeOutFile.substr(0, dot);
            bytecodeOutFile += ".nbc";
        }
        emit_bytecode(program, bytecodeOutFile);
        return 0;
    }

    if (flag_emit_asm)
    {
        // derive output Filename from input (program.csl -> program.asm)
        asmFile = inputFile;
        size_t dot = asmFile.rfind('.');
        if (dot != string::npos) asmFile = asmFile.substr(0, dot);
        asmFile += ".asm";

        // Running register allocation for main and all functions
        LivenessResult lr_main = compute_liveness(program);
        InterferenceGraph ig_main = build_interference_graph(program, lr_main);
        ColoringResult cr_main = color_graph(ig_main);

        // use only main coloring
        generate_x86(program, asmFile, &cr_main);

        if (flag_build)
        {
            // nasm -> object file
            string objFile = asmFile.substr(0, asmFile.rfind('.')) + ".o";
            string binFile = asmFile.substr(0, asmFile.rfind('.'));

            string nasmCmd = "nasm -f elf64 " + asmFile + " -o " + objFile;
            string linkCmd = "gcc -o " + binFile + " " + objFile + " -no-pie";

            fprintf(stderr, "Assembling: %s\n", nasmCmd.c_str());
            int r1 = system(nasmCmd.c_str());
            if (r1 != 0) 
            {
                fprintf(stderr, "nasm failed\n");
                return 1;
            }

            fprintf(stderr, "Linking:    %s\n", linkCmd.c_str());
            int r2 = system(linkCmd.c_str());
            if (r2 != 0)
            {
                fprintf(stderr, "gcc link failed\n");
                return 1;
            }

            fprintf(stderr, "Binary:     %s\n", binFile.c_str());   
        }
    }

    // IR dump 
    if (flag_dump_ir)
    {
        dump_ir(program);
        for (auto& kv : functionTable)
        {
            printf("\n-- function IR: %s --\n", kv.first.c_str());
            dump_ir(kv.second);
        }
    }

    // benchmark mode 
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
    // normal single execution
    input_replay_index = -1;  // live stdin mode

#if defined(_WIN32) && !defined(NOVA_NO_JIT)
    // Pre-compute colorings for all functions so JIT can reuse them
    for (auto& kv : functionTable)
    {
        struct InstructionNode* fhead = kv.second;
        LivenessResult flr = compute_liveness(fhead);
        InterferenceGraph fig = build_interference_graph(fhead, flr);
        unordered_set<int> lacSlots = find_live_across_calls(fhead, flr);
        unordered_set<int> constSlots = find_constant_slots(fhead);
        g_funcColorings[fhead] = color_graph(fig, lacSlots, constSlots);
    }

    // Build param count map by scanning all call sites in main + every function
    unordered_map<InstructionNode*, int> funcParamCount;
    auto scanForCalls = [&](struct InstructionNode* ir) {
        for (struct InstructionNode* p = ir; p != nullptr; p = p->next)
            if (p->type == CALL)
                funcParamCount[p->call_inst.function_head] = p->call_inst.num_params;
    };
    scanForCalls(program);
    for (auto& kv2 : functionTable) scanForCalls(kv2.second);

    // Pre-JIT compile all functions before execution
    for (auto& kv : functionTable)
    {
        struct InstructionNode* fhead = kv.second;
        // JIT only handles single-parameter functions for now
        if (funcParamCount.count(fhead) && funcParamCount[fhead] > 1) continue;
        void* native = jit_compile_function(fhead);
        if (native) jit_compiled[fhead] = native;
    }
#endif

    execute_program(program);
    fflush(stdout);

    if (getenv("NOVA_TENSOR_STATS"))
    {
        int live = 0, freed = 0;
        for (auto& t : tensor_heap) (t.freed ? freed : live)++;
        fprintf(stderr, "[tensor-stats] heap_size=%zu live=%d freed=%d\n", 
                tensor_heap.size(), live, freed);
    }

    return 0;
}
#endif  // NOVA_NOVM_BUILD
