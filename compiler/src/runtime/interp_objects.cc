// Runtime for class instances. An object is a handle (an int in mem[]) into object_heap; its fields are kept in
// declaration order (a parent's fields first), each as a double: a number as itself, a bool / string (strMem
// index) / Tensor / container / array base as its int. The compiler knows every field's type and index, so the
// runtime only needs to know which slot space (mem / fmem / dmem) a value comes from or goes to, and which
// fields hold a Tensor (the object owns a reference to it).
//
// Objects and containers are reference-counted (a container's count is kept in interp_containers.cc). A reference
// belongs to a running call (a variable or parameter slot of that call, or a temporary: a new object or
// container, one a call returned, one read from a field or element, which a store will take or OBJ_RELEASE will
// drop), or to an object field or a container element. A call's references are dropped when it returns, and an
// object or container is freed when its count reaches 0. A store
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
    vector<bool> ownsObject;  // ... or an object
    vector<bool> ownsContainer;  // ... or a container
    int cls = 0;                 // its class's id: a method call runs this class's method(virtualTable)
    int refs = 0;
    bool freed = false;
};

// What a reference is to: an object (object_heap) or a container (container_heap, interp_containers.cc). A
// value's kind follows from its type.
enum RefKind { REF_OBJECT, REF_CONTAINER };
static RefKind kind_of(VarType t) { return (t == TYPE_VECTOR || t == TYPE_MAP) ? REF_CONTAINER : REF_OBJECT; }

struct ObjectRef
{
    int slot;    // the variable / parameter slot holding it, or TEMP
    int handle;
    RefKind kind;
};
static const int TEMP = -1;

static vector<Object> object_heap;
static vector<int> free_handles;              // freed objects' handles, reused by the next OBJ_NEW
static vector<vector<ObjectRef>> call_refs(1);  // each running call's references, innermost last ([0]: top level)

static bool live(int h) { return h > 0 && h < (int)object_heap.size() && !object_heap[h].freed; }

// Frees h, and every object that then has no references left because only h's fields referred to it (a list's
// nodes: one at a time, not recursively, so a long list cannot overflow the stack).
static void free_object(int h)
{
    vector<int> pending = {h};
    while (!pending.empty())
    {
        int x = pending.back();
        pending.pop_back();
        Object& o = object_heap[x];
        for (size_t i = 0;i < o.fields.size(); i++)
        {
            if (o.ownsTensor[i]) tensor_release((int)o.fields[i]);
            int f = (int)o.fields[i];
            if (o.ownsObject[i] && live(f) && --object_heap[f].refs <= 0) pending.push_back(f);
            if (o.ownsContainer[i]) containers_release(f);
        }
        o.fields.clear();
        o.ownsTensor.clear();
        o.ownsObject.clear();
        o.ownsContainer.clear();
        o.freed = true;
        free_handles.push_back(x);
    }
}

static bool live_ref(RefKind k, int h) { return (k == REF_CONTAINER) ? containers_live(h) : live(h); }

static void add_ref(vector<ObjectRef>& refs, int slot, int h, RefKind k = REF_OBJECT)
{
    if (!live_ref(k, h)) return;
    if (k == REF_CONTAINER) containers_retain(h);
    else object_heap[h].refs++;
    refs.push_back({slot, h, k});
}

static void drop(int h)
{
    if (!live(h)) return;
    if (--object_heap[h].refs <= 0) free_object(h);
}

static void drop_ref(const ObjectRef& r)
{
    if (r.kind == REF_CONTAINER) containers_release(r.handle);
    else drop(r.handle);
}

// Removes this call's reference held by slot (or the temporary reference of kind k to h, for slot TEMP) and 
// returns it, with handle 0 if there is none. The count is not changed: the caller drops it, or hands it to 
// another slot.
static ObjectRef take_ref(int slot, int h, RefKind k)
{
    vector<ObjectRef>& refs = call_refs.back();
    for (size_t i = refs.size(); i-- > 0;)
        if (refs[i].slot == slot && (slot != TEMP || (refs[i].handle == h && refs[i].kind == k)))
        {
            ObjectRef taken = refs[i];
            refs.erase(refs.begin() + i);
            return taken;
        }
    return {slot, 0, k};
}

void objects_enter_call() { call_refs.emplace_back(); }

void objects_bind_param(int slot, int h, VarType t) { add_ref(call_refs.back(), slot, h, kind_of(t)); }

// A new container: a temporary of the running call
void objects_add_container_temp(int h) { add_ref(call_refs.back(), TEMP, h, REF_CONTAINER); }

// A reference that belongs to no call: an object field's, or a container element's 
void objects_retain(int h)
{
    if (live(h)) object_heap[h].refs++;
}

void objects_release(int h) { drop(h); }

// A value read from a field or an element: a temporary of the running call
void objects_add_temp(int h) { add_ref(call_refs.back(), TEMP, h); }

// v.pop(): the element's reference becomes the running call's temporary
void objects_take_temp(int h)
{
    if (live(h)) call_refs.back().push_back({TEMP, h, REF_OBJECT});
}

void objects_exit_call(int returned, VarType type)
{
    if (call_refs.size() <= 1) return;
    // An object or container returned by the call is the caller's temporary: count it before the call's
    // references go.
    if (returned) add_ref(call_refs[call_refs.size() - 2], TEMP, returned, kind_of(type));
    vector<ObjectRef> refs = call_refs.back();
    call_refs.pop_back();
    for (const ObjectRef& r : refs) drop_ref(r);
}

static double read_value(int slot, VarType t)
{
    if (t == TYPE_FLOAT) return fmem[slot];
    if (t == TYPE_DOUBLE) return dmem[slot];
    return mem[slot];
}

static Object& object_at(int h, int line)
{
    if (h == 0) tensor_shape_error(line, "object is none (never set, or set to none)");
    if (h < 0 || h >= (int)object_heap.size())
        tensor_shape_error(line, "object used before it was created");
    if (object_heap[h].freed) tensor_shape_error(line, "object used after it was freed");
    return object_heap[h];
}

int objects_class_of(int h, int line) { return object_at(h, line).cls; }

InstructionNode* exec_object_call(InstructionNode* pc)
{
    auto& tc = pc->tensor_call_inst;
    int* a = tc.arg_slots;
    int line = pc->line_no;
    switch (tc.op)
    {
        case OBJ_NEW:  // a[0] holds the number of fields, a[1] the class's id; the new object is a temporary
        {
            if (object_heap.empty()) object_heap.push_back(Object());  // handle 0: "no object"
            Object o;
            o.fields.assign(mem[a[0]], 0.0);
            o.ownsTensor.assign(mem[a[0]], false);
            o.ownsObject.assign(mem[a[0]], false);  // an object field starts unset (0)
            o.ownsContainer.assign(mem[a[0]], false);
            o.cls = mem[a[1]];  // a[1] holds the class's id
            int h;
            if (!free_handles.empty()) { h = free_handles.back(); free_handles.pop_back(); object_heap[h] = o; }
            else { h = (int)object_heap.size(); object_heap.push_back(o); }
            mem[tc.result_slot] = h;
            add_ref(call_refs.back(), TEMP, h);
            break;
        }
        case OBJ_STORE:  // a[0] = a[1] (a variable: one more reference); a[0]'s previous object loses one
        case OBJ_MOVE:   // the same for a temporary a[1]: its reference becomes a[0]'s
        {                // (a[1]'s type says whether it is an object or a container)
            int dst = a[0], h = mem[a[1]];
            RefKind k = kind_of(tc.arg_types[1]);
            ObjectRef old = take_ref(dst, 0, k);
            if (tc.op == OBJ_MOVE && take_ref(TEMP, h, k).handle) call_refs.back().push_back({dst, h, k});
            else add_ref(call_refs.back(), dst, h, k);  // (also a moved value with no temporary, e.g. an inlined call)
            mem[dst] = h;
            drop_ref(old);  // after counting h: storing a variable's own object back is safe
            break;
        }
        case OBJ_RELEASE:  // a temporary in a[0] that nothing took (an argument, a discarded result)
            drop_ref(take_ref(TEMP, mem[a[0]], kind_of(tc.arg_types[0])));
            break;
        case OBJ_GET: case OBJ_GET_D:  // a[1] holds the field index
        {
            Object& o = object_at(mem[a[0]], line);
            int i = mem[a[1]];
            double v = o.fields[i];
            if (o.ownsTensor[i]) tensor_retain((int)v);  // the result slot is one more owner
            if (o.ownsObject[i]) objects_add_temp((int)v);  // ...and of an object, a temporary 
            if (o.ownsContainer[i]) objects_add_container_temp((int)v);  // ... as of a container
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
            bool object = (tc.arg_types[2] == TYPE_CLASS);
            bool container = (tc.arg_types[2] == TYPE_VECTOR || tc.arg_types[2] == TYPE_MAP);
            if (tensor) tensor_retain((int)v);  // retain before releasing: storing the same tensor back is safe
            if (object) objects_retain((int)v);
            if (container) containers_retain((int)v);
            if (o.ownsTensor[i]) tensor_release((int)o.fields[i]);
            int old = o.ownsObject[i] ? (int)o.fields[i] : 0;
            int oldContainer = o.ownsContainer[i] ? (int)o.fields[i] : 0;
            o.fields[i] = v;
            o.ownsTensor[i] = tensor;
            o.ownsObject[i] = object;
            o.ownsContainer[i] = container;
            drop(old);  // last: it can free objects (o too, if only that object referred to it)
            containers_release(oldContainer);
            break; 
        }
        default: break;
    }
    return pc->next;
}
