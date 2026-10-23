// Runtime for class instances. An object is a handle (an int in mem[]) into object_heap; its fields are kept in
// declaration order (a parent's fields first), each as a double: a number as itself, a bool / string (strMem
// index) / Tensor / container / array base as its int. The compiler knows every field's type and index, so the
// runtime only needs to know which slot space (mem / fmem / dmem) a value comes from or goes to, and which
// fields hold a Tensor (the object owns a reference to it).
#include <cstdio>
#include <string>
#include <vector>
#include "compiler.h"
#include "tensor_ops.h"
#include "interp_internal.h"

using namespace std;

struct Object
{
    vector<double> fields;
    vector<bool> ownsTensor;  // field i holds a Tensor this object has a reference to
    bool freed = false;
};

static vector<Object> object_heap;
static vector<int> free_handles;  // freed objects' handles, reused by the next OBJ_NEW
// The objects each running call created, innermost last ([0]: the top level, never freed). A local's slot is
// restored when its function returns, so the function's objects are freed then; a declaration that runs again
// in the same call frees its previous object itself (OBJ_FREE).
static vector<vector<int>> call_objects(1);

static void free_object(int h)
{
    if (h <= 0 || h >= (int)object_heap.size() || object_heap[h].freed) return;  // nothing held, or freed already
    Object& o = object_heap[h];
    for (size_t i = 0; i < o.fields.size(); i++)
        if (o.ownsTensor[i]) tensor_release((int)o.fields[i]);
    o.fields.clear();
    o.ownsTensor.clear();
    o.freed = true;
    free_handles.push_back(h);
}

void objects_enter_call() { call_objects.emplace_back(); }

void objects_exit_call()
{
    if (call_objects.size() <= 1) return;
    for (int h : call_objects.back()) free_object(h);
    call_objects.pop_back();
}

static double read_value(int slot, VarType t)
{
    if (t == TYPE_FLOAT) return fmem[slot];
    if (t == TYPE_DOUBLE) return dmem[slot];
    return mem[slot];
}

static Object& object_at(int h, int line)
{
    if (h <= 0 || h >= (int)object_heap.size())
        tensor_shape_error(line, "object used before it was created");
    if (object_heap[h].freed) tensor_shape_error(line, "object used after it was freed");
    return object_heap[h];
}

InstructionNode* exec_object_call(InstructionNode* pc)
{
    auto& tc = pc->tensor_call_inst;
    int* a = tc.arg_slots;
    int line = pc->line_no;
    switch (tc.op)
    {
        case OBJ_NEW:  // a[0] holds the number of fields
        {
            if (object_heap.empty()) object_heap.push_back(Object());  // handle 0: "no object"
            Object o;
            o.fields.assign(mem[a[0]], 0.0);
            o.ownsTensor.assign(mem[a[0]], false);
            int h;
            if (!free_handles.empty()) { h = free_handles.back(); free_handles.pop_back(); object_heap[h] = o; }
            else { h = (int)object_heap.size(); object_heap.push_back(o); }
            mem[tc.result_slot] = h;
            call_objects.back().push_back(h);
            break;
        }
        case OBJ_FREE:  // the object a variable held before its declaration runs again
            free_object(mem[a[0]]);
            break;
        case OBJ_GET: case OBJ_GET_D:  // a[1] holds the field index
        {
            Object& o = object_at(mem[a[0]], line);
            int i = mem[a[1]];
            double v = o.fields[i];
            if (o.ownsTensor[i]) tensor_retain((int)v);  // the result slot is one more owner
            if (tc.op == OBJ_GET_D) dmem[tc.result_slot] = v;
            else mem[tc.result_slot] = (int)v;
            break;
        }
        case OBJ_SET:  // a[1] holds the field index, a[2] the value
        {
            Object& o = object_at(mem[a[0]], line);
            int i = mem[a[1]];
            double v = read_value(a[2], tc.arg_types[2]);
            bool tensor = (tc.arg_types[2] == TYPE_TENSOR);
            if (tensor) tensor_retain((int)v);  // retain before releasing: storing the same tensor back is safe
            if (o.ownsTensor[i]) tensor_release((int)o.fields[i]);
            o.fields[i] = v;
            o.ownsTensor[i] = tensor;
            break;
        }
        default: break;
    }
    return pc->next;
}
