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
    bool in_block = false;    // a line ended with ':': keep reading until a blank line (like Python)

    while (true)
    {
        // show prompt
        if (!in_block)
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

        // A line ending in ':' opens a block: keep reading until an empty line ends it.
        string code = trimmed.substr(0, trimmed.find('#'));
        size_t codeEnd = code.find_last_not_of(" \t");
        if (codeEnd != string::npos && code[codeEnd] == ':') in_block = true;
        if (in_block)
        {
            if (!trimmed.empty()) continue;
            in_block = false;
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
                           accumulated.find("orbit ") != string::npos);
        if (!hasTopLevel)
        {
            // Pure statements - indent them into an ignite block
            wrapped = "ignite:\n";
            istringstream lines(accumulated);
            string l;
            while (getline(lines, l)) wrapped += "    " + l + "\n";
        }
        else if (!hasIgnite)
        {
            // Top-level def/class/import without an ignite block - append an empty one
            wrapped = accumulated + "\nignite:\n    pass\n";
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
            in_block = false;
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
        in_block = false;
    }
}
