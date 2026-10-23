// Runtime for class instances. An object is a handle (an int in mem[]) into object_heap; its fields are kept in
// declaration order (a parent's fields first), each as a double: a number as itself, a bool / string (strMem
// index) / Tensor / container / array base as its int. The compiler knows every field's type and index, so the
// runtime only needs to know which slot space (mem / fmem / dmem) a value comes from or goes to, and which
// fields hold a Tensor (the object owns a reference to it).
//
// Objects are reference-counted. Every reference belongs to a running call: a variable or parameter slot of that
// call, or a temporary (a new object, or one a call returned) that a store will take or OBJ_RELEASE will drop.
// A call's references are dropped when it returns, and an object is freed when its count reaches 0. A store
// releases what its slot referred to before by looking up the slot's reference, not the value in the slot, so a
// stale value (a slot restored after a call, a recursive call's caller's object) is never released by mistake.
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
    int refs = 0;
    bool freed = false;
};

struct ObjectRef
{
    int slot;    // the variable / parameter slot holding it, or TEMP
    int handle;
};
static const int TEMP = -1;

static vector<Object> object_heap;
static vector<int> free_handles;              // freed objects' handles, reused by the next OBJ_NEW
static vector<vector<ObjectRef>> call_refs(1);  // each running call's references, innermost last ([0]: top level)

static bool live(int h) { return h > 0 && h < (int)object_heap.size() && !object_heap[h].freed; }

static void free_object(int h)
{
    Object& o = object_heap[h];
    for (size_t i = 0; i < o.fields.size(); i++)
        if (o.ownsTensor[i]) tensor_release((int)o.fields[i]);
    o.fields.clear();
    o.ownsTensor.clear();
    o.freed = true;
    free_handles.push_back(h);
}

static void add_ref(vector<ObjectRef>& refs, int slot, int h)
{
    if (!live(h)) return;
    object_heap[h].refs++;
    refs.push_back({slot, h});
}

static void drop(int h)
{
    if (!live(h)) return;
    if (--object_heap[h].refs <= 0) free_object(h);
}

// Removes this call's reference held by slot (or the temporary reference to h, for slot TEMP) and returns its
// handle, 0 if there is none. The count is not changed: the caller drops it, or hands it to another slot.
static int take_ref(int slot, int h)
{
    vector<ObjectRef>& refs = call_refs.back();
    for (size_t i = refs.size(); i-- > 0;)
        if (refs[i].slot == slot && (slot != TEMP || refs[i].handle == h))
        {
            int taken = refs[i].handle;
            refs.erase(refs.begin() + i);
            return taken;
        }
    return 0;
}

void objects_enter_call() { call_refs.emplace_back(); }

void objects_bind_param(int slot, int h) { add_ref(call_refs.back(), slot, h); }

void objects_exit_call(int returned)
{
    if (call_refs.size() <= 1) return;
    // An object returned by the call is the caller's temporary: count it before the call's references go.
    if (returned) add_ref(call_refs[call_refs.size() - 2], TEMP, returned);
    vector<ObjectRef> refs = call_refs.back();
    call_refs.pop_back();
    for (const ObjectRef& r : refs) drop(r.handle);
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
        case OBJ_NEW:  // a[0] holds the number of fields; the new object is a temporary
        {
            if (object_heap.empty()) object_heap.push_back(Object());  // handle 0: "no object"
            Object o;
            o.fields.assign(mem[a[0]], 0.0);
            o.ownsTensor.assign(mem[a[0]], false);
            int h;
            if (!free_handles.empty()) { h = free_handles.back(); free_handles.pop_back(); object_heap[h] = o; }
            else { h = (int)object_heap.size(); object_heap.push_back(o); }
            mem[tc.result_slot] = h;
            add_ref(call_refs.back(), TEMP, h);
            break;
        }
        case OBJ_STORE:  // a[0] = a[1] (a variable: one more reference); a[0]'s previous object loses one
        case OBJ_MOVE:   // the same for a temporary a[1]: its reference becomes a[0]'s
        {
            int dst = a[0], h = mem[a[1]];
            int old = take_ref(dst, 0);
            if (tc.op == OBJ_MOVE && take_ref(TEMP, h)) call_refs.back().push_back({dst, h});
            else add_ref(call_refs.back(), dst, h);  // (also a moved value with no temporary, e.g. an inlined call)
            mem[dst] = h;
            drop(old);  // after counting h: storing a variable's own object back is safe
            break;
        }
        case OBJ_RELEASE:  // a temporary in a[0] that nothing took (an argument, a discarded result)
            drop(take_ref(TEMP, mem[a[0]]));
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
