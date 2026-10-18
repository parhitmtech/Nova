/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Interactive REPL (--repl). Split out of compiler.cc.
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
        string wrapped = accumulated;
        bool hasIgnite = accumulated.find("ignite") != string::npos;
        bool hasTopLevel = (hasIgnite ||
                           accumulated.find("def ") != string::npos ||
                           accumulated.find("class ") != string::npos ||
                           accumulated.find("struct ") != string::npos ||
                           accumulated.find("import ") != string::npos);
        if (!hasTopLevel)
        {
            // Pure statement - wrap in an ignite block
            wrapped = "ignite\n{\n" + accumulated + "\n}\n";
        }
        else if (!hasIgnite)
        {
            // Top-level def/class/import without an ignite block - append an empty one
            wrapped = accumulated + "\nignite\n{\n}\n";
        }
        set<string> already_imported;
        string processed = preprocess_import(wrapped, ".", already_imported);
        lexer.ReinitializeFromString(processed);
        struct InstructionNode* program = parse_generate_intermediate_representation();

        if (!errorList.empty())
        {
            for (const string& err : errorList)
                fprintf(stderr, "%s\n", err.c_str());
            errorList.clear();
            accumulated = "";
            brace_depth = 0;
            continue; // recover insterad of crashing
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
    }
}
