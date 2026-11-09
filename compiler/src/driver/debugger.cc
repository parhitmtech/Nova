// Step debugger (--debug) and benchmark input replay. Split out of compiler.cc.
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

using namespace std;

#define DEBUG 1

// ── Input replay buffer ───────────────────────────────────────────────────────
// Used only during benchmark mode — captures inputs from first run
// and replays them for subsequent timed runs
vector<int> input_replay_buffer;
int         input_replay_index = -1;  // -1 = live stdin mode


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

void debug_print_state(int line_no)
{
    // Build JSON with current line + all variable values
    printf("__DEBUG__{\"type\":\"paused\",\"line\":%d,\"vars\":{", line_no);

    bool first = true;
    // Int variables
    for (auto& kv : debug_symbol_snapshot)
    {
        int slot = kv.second.first;
        VarType type = kv.second.second;
        if (!first) printf(",");
        if (type == TYPE_FLOAT && slot < (int)fmem.size())
        {
            printf("\"%s\":{\"type\":\"float\",\"value\":%.4f}", kv.first.c_str(), fmem[slot]);
        }
        else if (type == TYPE_DOUBLE && slot < (int)dmem.size())
        {
            printf("\"%s\":{\"type\":\"double\",\"value\":%.4f}", kv.first.c_str(), dmem[slot]);
        }
        else if (slot < (int)mem.size())
        {
            printf("\"%s\":{\"type\":\"int\",\"value\":%d}", kv.first.c_str(), mem[slot]);
        }
        first = false;
    }
    printf("}}\n");
    fflush(stdout);
}

string debug_wait_command()
{
    char buf[256];
    while (fgets(buf, sizeof(buf), stdin))
    {
        string cmd(buf);
        // trim whitespace
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
        {
            cmd.pop_back();
        }
        if (cmd == "step" || cmd == "continue" || cmd == "stop")
        {
            return cmd;
        }

        // Handle break points: "breakpoints:5,12,20"
        if (cmd.size() >= 12 && cmd.substr(0, 12) == "breakpoints:")
        {
            string nums = cmd.substr(12);
            debug_breakpoints.clear();
            if (!nums.empty())
            {
                stringstream ss(nums);
                string token;
                while (getline(ss, token, ','))
                {
                    try { debug_breakpoints.insert(stoi(token) + import_line_offset); }
                    catch(...) {}
                }
            }
            continue;
        }
    }
    return "stop";
}
