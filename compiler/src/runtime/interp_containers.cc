// Runtime for the built-in containers vector<T> and map<K, V>. A container is a handle (an int in mem[]) into
// container_heap. Every element is kept as a double: a number as itself, a bool / string (strMem index) /
// Tensor handle as its int. The compiler checks element types, so the runtime only needs to know which slot
// space (mem / fmem / dmem) a value comes from or goes to.
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "compiler.h"
#include "tensor_ops.h"
#include "interp_internal.h"

using namespace std;

struct Container
{
    bool isMap = false;
    VarType key = TYPE_UNKNOWN;    // map: TYPE_INT or TYPE_STRING
    VarType elem = TYPE_UNKNOWN;
    vector<double> items;          // vector
    map<long long, double> intKeys;  // map<int, V>
    map<string, double> stringKeys;  // map<string, V>
};

static vector<Container> container_heap;

// The value in a slot, by the slot's type: float / double live in fmem / dmem, everything else in mem.
static double read_value(int slot, VarType t)
{
    if (t == TYPE_FLOAT) return fmem[slot];
    if (t == TYPE_DOUBLE) return dmem[slot];
    return mem[slot];
}

// A Tensor stored in a container is one more owner of its handle.
static void retain_element(const Container& c, double v)
{
    if (c.elem == TYPE_TENSOR) tensor_retain((int)v);
}

static void release_element(const Container& c, double v)
{
    if (c.elem == TYPE_TENSOR) tensor_release((int)v);
}

static Container& container_at(int h, int line)
{
    if (h < 0 || h >= (int)container_heap.size())
        tensor_shape_error(line, "container used before it was created");
    return container_heap[h];
}

// A map key from a slot: an int, or the string a strMem index points at.
static string key_text(const Container& c, int slot)
{
    return (c.key == TYPE_STRING) ? "\"" + strMem[mem[slot]] + "\"" : to_string(mem[slot]);
}

// The element at index i of a vector, or the value for a key of a map; a runtime error if it is not there.
static double& element(Container& c, int slot, int line)
{
    if (!c.isMap)
    {
        int i = mem[slot];
        if (i < 0 || i >= (int)c.items.size())
            tensor_shape_error(line, "vector index " + to_string(i) + " out of range (size " + to_string(c.items.size()) + ")");
        return c.items[i];
    }
    if (c.key == TYPE_STRING)
    {
        auto it = c.stringKeys.find(strMem[mem[slot]]);
        if (it == c.stringKeys.end()) tensor_shape_error(line, "map has no key " + key_text(c, slot));
        return it->second;
    }
    auto it = c.intKeys.find(mem[slot]);
    if (it == c.intKeys.end()) tensor_shape_error(line, "map has no key " + key_text(c, slot));
    return it->second;
}

InstructionNode* exec_container_call(InstructionNode* pc)
{
    auto& tc = pc->tensor_call_inst;
    int* a = tc.arg_slots;
    int line = pc->line_no;
    switch (tc.op)
    {
        case CON_VEC_NEW:  // vector<T>(): a[0] holds T
        case CON_MAP_NEW:  // map<K, V>(): a[0] holds K, a[1] holds V
        {
            Container c;
            c.isMap = (tc.op == CON_MAP_NEW);
            c.key = c.isMap ? (VarType)mem[a[0]] : TYPE_UNKNOWN;
            c.elem = (VarType)mem[a[c.isMap ? 1 : 0]];
            container_heap.push_back(c);
            mem[tc.result_slot] = (int)container_heap.size() - 1;
            break;
        }
        case CON_PUSH:  // v.push(x)
        {
            Container& c = container_at(mem[a[0]], line);
            double v = read_value(a[1], tc.arg_types[1]);
            retain_element(c, v);
            c.items.push_back(v);
            break;
        }
        case CON_POP: case CON_POP_D:  // v.pop(): the last element; its Tensor reference moves to the result
        {
            Container& c = container_at(mem[a[0]], line);
            if (c.items.empty()) tensor_shape_error(line, "pop() on an empty vector");
            double v = c.items.back();
            c.items.pop_back();
            if (tc.op == CON_POP_D) dmem[tc.result_slot] = v;
            else mem[tc.result_slot] = (int)v;
            break;
        }
        case CON_GET: case CON_GET_D:  // v[i], m[k]
        {
            Container& c = container_at(mem[a[0]], line);
            double v = element(c, a[1], line);
            retain_element(c, v);  // the result slot is one more owner of a Tensor element
            if (tc.op == CON_GET_D) dmem[tc.result_slot] = v;
            else mem[tc.result_slot] = (int)v;
            break;
        }
        case CON_SET:  // v[i] = x (i must exist), m[k] = x (adds k)
        {
            Container& c = container_at(mem[a[0]], line);
            double v = read_value(a[2], tc.arg_types[2]);
            retain_element(c, v);
            if (c.isMap && c.key == TYPE_STRING)
            {
                auto it = c.stringKeys.find(strMem[mem[a[1]]]);
                if (it != c.stringKeys.end()) release_element(c, it->second);
                c.stringKeys[strMem[mem[a[1]]]] = v;
            }
            else if (c.isMap)
            {
                auto it = c.intKeys.find(mem[a[1]]);
                if (it != c.intKeys.end()) release_element(c, it->second);
                c.intKeys[mem[a[1]]] = v;
            }
            else
            {
                double& slot = element(c, a[1], line);
                release_element(c, slot);
                slot = v;
            }
            break;
        }
        case CON_SIZE:
        {
            Container& c = container_at(mem[a[0]], line);
            size_t n = !c.isMap ? c.items.size() : (c.key == TYPE_STRING) ? c.stringKeys.size() : c.intKeys.size();
            mem[tc.result_slot] = (int)n;
            break;
        }
        case CON_HAS:  // m.has(k)
        {
            Container& c = container_at(mem[a[0]], line);
            bool has = (c.key == TYPE_STRING) ? c.stringKeys.count(strMem[mem[a[1]]]) > 0 : c.intKeys.count(mem[a[1]]) > 0;
            mem[tc.result_slot] = has ? 1 : 0;
            break;
        }
        case CON_REMOVE:  // m.remove(k): nothing happens if k is not there
        {
            Container& c = container_at(mem[a[0]], line);
            if (c.key == TYPE_STRING)
            {
                auto it = c.stringKeys.find(strMem[mem[a[1]]]);
                if (it != c.stringKeys.end()) { release_element(c, it->second); c.stringKeys.erase(it); }
            }
            else
            {
                auto it = c.intKeys.find(mem[a[1]]);
                if (it != c.intKeys.end()) { release_element(c, it->second); c.intKeys.erase(it); }
            }
            break;
        }
        case CON_CLEAR:
        {
            Container& c = container_at(mem[a[0]], line);
            for (double v : c.items) release_element(c, v);
            for (auto& kv : c.intKeys) release_element(c, kv.second);
            for (auto& kv : c.stringKeys) release_element(c, kv.second);
            c.items.clear();
            c.intKeys.clear();
            c.stringKeys.clear();
            break;
        }
        default: break;
    }
    return pc->next;
}
