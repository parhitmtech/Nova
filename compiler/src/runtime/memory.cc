// Slot allocators for mem[] / fmem[] / dmem[]. Split out of compiler.cc.
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

int alloc_float_slot()
{
    fmem.push_back(0.0f);
    return next_float_available++;
}

int alloc_double_slot()
{
    dmem.push_back(0.0);
    return next_double_available++;
}
