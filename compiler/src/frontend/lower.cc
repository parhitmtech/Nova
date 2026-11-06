// Lowering (lower.h): one walk over the checked tree, writing each function's instructions and then the ignite
// block's. A value lives in a slot of mem[] (an int, a bool, or a string as its index in strMem), fmem[] (a float)
// or dmem[] (a double), laid out as the old parser lays them out: a function's int slots are one range, which a call
// saves and restores so a recursive call does not overwrite its caller's locals. A tensor is an int slot holding its
// handle; g_tensorSlots lists those slots, so the tensor-free pass knows which values to release. A struct is one
// slot per field; an object is an int slot holding its handle into the runtime's object heap (interp_objects.cc),
// and a vector or a map one holding its handle into the container heap (interp_containers.cc).
//
// Functions are lowered in file order, and a function a call needs before its turn is lowered right then. A call
// into a function whose body is still being lowered (a recursive call) gets its target when that body is done.
//
// A generic function or class is lowered once per copy the check pass made, with that copy's own types (check.h's
// Instance): scale<int, int> is a function of its own, and Box<int> a class of its own, with its own methods.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "lower.h"

using namespace std;

extern map<string, int> symbolTable;        // the ignite block's variables, which the debugger shows
extern map<string, int> floatSymbolTable;   // (parser_state.cc)
extern map<string, int> doubleSymbolTable;
extern map<string, vector<int>> functionReturnSlots;  // read by the tensor-free pass
extern const map<string, TensorOp> TENSOR_OP_MAP;     // the tensor built-ins, and the kinds of their results
extern const set<TensorOp> TENSOR_VOID_OPS;           // (parser_state.cc)
extern const set<TensorOp> TENSOR_DOUBLE_OPS;
extern const set<TensorOp> TENSOR_INT_OPS;

namespace
{
    // ─────────────────────────────────────────────────────────────────────────────
    // What lowering covers: numbers, bools, strings, arrays, control flow and functions (2c-1), tensors (2c-2a),
    // hf / sol (2c-2b), structs and classes (2c-3), vectors and maps (2c-4), and generics (2c-5)
    // ─────────────────────────────────────────────────────────────────────────────

    // A node's type, a variable's type and a function's signature; in a generic function's or class's copy (copy:
    // its index in instances, else NoInstance), the copy's own
    TypeId node_type(const CheckResult& checked, uint32_t n, uint32_t copy)
    {
        if (copy == NoInstance) return checked.nodeTypes[n];
        const unordered_map<uint32_t, TypeId>& types = checked.instances[copy].nodeTypes;
        auto it = types.find(n);
        return it == types.end() ? TypeTable::Unknown : it->second;
    }

    TypeId variable_type(const CheckResult& checked, uint32_t entity, uint32_t copy)
    {
        if (copy != NoInstance)
        {
            const unordered_map<uint32_t, TypeId>& types = checked.instances[copy].varTypes;
            auto it = types.find(entity);
            if (it != types.end()) return it->second;
        }
        return checked.entities[entity].type;
    }

    const Signature* signature_of(const CheckResult& checked, uint32_t fn, uint32_t copy)
    {
        const unordered_map<uint32_t, Signature>& table =
            copy == NoInstance ? checked.signatures : checked.instances[copy].signatures;
        auto it = table.find(fn);
        return it == table.end() ? nullptr : &it->second;
    }

    bool generic_class(const ParseTree& tree, const CheckResult& checked, uint32_t cls)
    {
        return tree.kind(tree.children(checked.entities[cls].decl)[0]) == NodeKind::TypeParams;
    }

    // A top-level definition with copies: a generic function or class
    bool generic(const ParseTree& tree, const CheckResult& checked, uint32_t decl)
    {
        if (tree.kind(decl) == NodeKind::ClassDecl) return generic_class(tree, checked, checked.refs[decl]);
        const Signature* sig = signature_of(checked, decl, NoInstance);
        return tree.kind(decl) == NodeKind::FunctionDecl && sig && sig->generic;
    }

    // What a top-level definition is lowered as: itself, or each copy the check pass made of a generic one (a copy
    // never made is never lowered)
    vector<uint32_t> copies_of(const ParseTree& tree, const CheckResult& checked, uint32_t decl)
    {
        if (!generic(tree, checked, decl)) return {NoInstance};
        vector<uint32_t> copies;
        for (uint32_t i = 0; i < checked.instances.size(); i++)
        {
            if (checked.instances[i].decl == decl) copies.push_back(i);
        }
        return copies;
    }

    bool covered_type(const CheckResult& checked, TypeId t)
    {
        switch (checked.types[t].kind)
        {
            case TypeKind::Unknown:     // a call that returns nothing
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Double:
            case TypeKind::Bool:
            case TypeKind::String:
            case TypeKind::Tensor:
            case TypeKind::Array:
            case TypeKind::None:
            case TypeKind::Struct:
            case TypeKind::Class:       return true;
            case TypeKind::Vector:      return covered_type(checked, checked.types[t].a);  // of what it holds
            case TypeKind::Map:         return covered_type(checked, checked.types[t].b);
            default:                    return false;
        }
    }

    // Whether lowering covers node n (each node is asked on its own), in a copy with the copy's types
    bool covered(const ParseTree& tree, const CheckResult& checked, uint32_t n, uint32_t copy)
    {
        auto entity = [&](uint32_t m) { return checked.refs[m] == NoEntity ? nullptr : &checked.entities[checked.refs[m]]; };
        auto kind = [&](uint32_t m) { return checked.types[node_type(checked, m, copy)].kind; };
        auto tensor = [&](uint32_t m) { return kind(m) == TypeKind::Tensor; };
        auto whole = [&](uint32_t m) { return kind(m) == TypeKind::Struct; };  // a struct as one value
        auto container = [&](uint32_t m) { return kind(m) == TypeKind::Vector || kind(m) == TypeKind::Map; };
        vector<uint32_t> kids = tree.children(n);
        switch (tree.kind(n))
        {
            case NodeKind::GenericType:     // vector<T>, map<K, V>, and a generic class's copy, Box<int>
            {
                string_view name = tree.spelling(n);
                const Entity* e = entity(n);
                return name == "vector" || name == "map" || (e && e->kind == EntityKind::Class);
            }
            case NodeKind::StructDecl:      // fields of numbers, bools, strings, Tensors and structs
            case NodeKind::ClassDecl:       // ... and of objects, containers and int arrays (int data[256]); not one
                                            // whose parent is generic (class C extends Box)
            {
                bool isClass = tree.kind(n) == NodeKind::ClassDecl;
                for (uint32_t f : kids)
                {
                    if (tree.kind(f) == NodeKind::Extends)
                    {
                        uint32_t up = checked.refs[tree.children(f)[0]];
                        if (up != NoEntity && generic_class(tree, checked, up)) return false;
                    }
                    if (tree.kind(f) != NodeKind::FieldDecl) continue;
                    vector<uint32_t> parts = tree.children(f);  // its type, then an array field's size
                    const Entity* e = entity(parts[0]);
                    if (parts.size() > 1 && (!isClass || tree.token(parts[0]).kind != INT_TYPE ||
                                             tree.kind(parts[1]) != NodeKind::IntLiteral)) return false;
                    if (!isClass && ((e && e->kind == EntityKind::Class) || tree.kind(parts[0]) == NodeKind::GenericType))
                    {
                        return false;
                    }
                }
                return true;
            }
            case NodeKind::Member:          // a field; t.T, t.rows, t.cols, t.rank; hf.infer, sol.wait; obj.m(...),
            {                               // v.push(...)
                if (whole(kids[0]))  // l.a.x: a field of a struct variable
                {
                    if (tree.kind(kids[0]) != NodeKind::Name && tree.kind(kids[0]) != NodeKind::Member) return false;
                    break;
                }
                if (kind(kids[0]) == TypeKind::Class || container(kids[0])) break;
                const Entity* e = tree.kind(kids[0]) == NodeKind::Name ? entity(kids[0]) : nullptr;
                return tensor(kids[0]) || (e && e->kind == EntityKind::Module);
            }
            case NodeKind::FunctionDecl:    // not one that takes or returns a struct; a generic function in each of
            {                               // its copies, as a generic class's methods are
                const Signature* sig = signature_of(checked, n, copy);
                if (!sig || sig->generic) return false;
                for (TypeId t : sig->params)
                {
                    if (checked.types[t].kind == TypeKind::Struct) return false;
                }
                for (TypeId t : sig->returns)
                {
                    if (checked.types[t].kind == TypeKind::Struct) return false;
                }
                return true;
            }
            case NodeKind::TypeName:        // a built-in type, a struct, a class or a class's type parameter
            {
                TokenType k = tree.token(n).kind;
                if (k == ID)
                {
                    const Entity* e = entity(n);
                    return e && (e->kind == EntityKind::Struct || e->kind == EntityKind::Class ||
                                 e->kind == EntityKind::TypeParam);
                }
                return k == INT_TYPE || k == FLOAT_TYPE || k == DOUBLE_TYPE || k == BOOL_TYPE || k == STRING_TYPE ||
                       k == TENSOR_TYPE;
            }
            case NodeKind::MultiDecl:       // int a, double b = f(): a call to a function, giving no containers
            {
                for (size_t i = 0; i + 1 < kids.size(); i++)
                {
                    TypeKind k = checked.types[variable_type(checked, checked.refs[kids[i]], copy)].kind;
                    if (k == TypeKind::Vector || k == TypeKind::Map) return false;
                }
                uint32_t value = kids.back();
                if (tree.kind(value) != NodeKind::Call) return false;
                const Entity* e = entity(tree.children(value)[0]);
                return e && e->kind == EntityKind::Function;
            }
            case NodeKind::Case:            return kind(kids[0]) == TypeKind::Int;
            case NodeKind::Index:           // t[i, j, ...] on any Tensor; a[i]: one index into an array variable or an
            {                               // int array field (self.data[i]); v[i] and m[k] on any container
                bool array = tree.kind(kids[0]) == NodeKind::Name ||
                             (tree.kind(kids[0]) == NodeKind::Member && kind(kids[0]) == TypeKind::Array);
                if (!tensor(kids[0]) && (kids.size() != 2 || !(array || container(kids[0])))) return false;
                break;
            }
            case NodeKind::Call:            // a struct is an argument only to its own constructor, Point(3, 4)
            {
                if (tree.kind(kids[0]) == NodeKind::TypeName)  // a conversion: int(x), float(x), double(x)
                {
                    TokenType k = tree.token(kids[0]).kind;
                    if (k != INT_TYPE && k != FLOAT_TYPE && k != DOUBLE_TYPE) return false;
                }
                const Entity* e = tree.kind(kids[0]) == NodeKind::Name ? entity(kids[0]) : nullptr;
                bool structValue = e && e->kind == EntityKind::Struct;
                for (size_t i = 1; i < kids.size(); i++)
                {
                    if (whole(kids[i]) && !structValue) return false;
                }
                break;
            }
            case NodeKind::Return:          // a struct is not returned, nor compared; nor is a container compared
            case NodeKind::Binary:
            {
                for (uint32_t k : kids)
                {
                    if (whole(k) || (tree.kind(n) == NodeKind::Binary && container(k))) return false;
                }
                break;
            }
            case NodeKind::Name:
            {
                const Entity* e = entity(n);
                if (!e) return false;
                if (e->kind == EntityKind::Builtin)
                {
                    string_view f = names.text(e->name);
                    return f == "print" || f == "input" || f == "array" || TENSOR_OP_MAP.count(string(f));
                }
                switch (e->kind)
                {
                    case EntityKind::Variable:
                    case EntityKind::Function:
                    case EntityKind::Module:
                    case EntityKind::Struct:    // Point(3, 4)
                    case EntityKind::Class:     // Counter()
                    case EntityKind::None:      break;
                    default:                    return false;
                }
                break;
            }
            default:                        break;
        }
        bool expression = tree.kind(n) >= NodeKind::IntLiteral;
        return !expression || covered_type(checked, node_type(checked, n, copy));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Lowerer
    // ─────────────────────────────────────────────────────────────────────────────

    VarType var_type(TypeKind k)
    {
        switch (k)
        {
            case TypeKind::Int:         return TYPE_INT;
            case TypeKind::Float:       return TYPE_FLOAT;
            case TypeKind::Double:      return TYPE_DOUBLE;
            case TypeKind::Bool:        return TYPE_BOOL;
            case TypeKind::String:      return TYPE_STRING;
            case TypeKind::Tensor:      return TYPE_TENSOR;
            case TypeKind::Vector:      return TYPE_VECTOR;
            case TypeKind::Map:         return TYPE_MAP;
            case TypeKind::None:
            case TypeKind::Class:       return TYPE_CLASS;
            default:                    return TYPE_UNKNOWN;
        }
    }

    // An object's or a container's handle: counted references, which a store takes and a release drops
    bool reference(VarType t)
    {
        return t == TYPE_CLASS || t == TYPE_VECTOR || t == TYPE_MAP;
    }

    // A vector's or a map's methods, and the op each runs (pop, min, max and sum of floats or doubles run the *_D op)
    const map<string, TensorOp> CONTAINER_METHODS = {
        {"size", CON_SIZE},         {"push", CON_PUSH},         {"pop", CON_POP},           {"clear", CON_CLEAR},
        {"contains", CON_CONTAINS}, {"index_of", CON_INDEX_OF}, {"binary_search", CON_BSEARCH},
        {"min", CON_MIN},           {"max", CON_MAX},           {"sum", CON_SUM},           {"sort", CON_SORT},
        {"reverse", CON_REVERSE},   {"has", CON_HAS},           {"remove", CON_REMOVE},
    };

    template <class T> T* array_of(const vector<T>& v)  // a call's slots and types, as the CALL instruction keeps them
    {
        T* a = new T[v.size()];
        copy(v.begin(), v.end(), a);
        return a;
    }

    class Lowerer
    {
        public:
            Lowerer(const ParseTree& tree, const CheckResult& in) : tree(tree), in(in) {}
            InstructionNode* run();

        private:
            const ParseTree& tree;
            const CheckResult& in;

            // The instructions being written: a function's body, or the ignite block's
            InstructionNode* head = nullptr;
            InstructionNode* tail = nullptr;

            // A variable's slot. An array's slot is its first element and size its length; a dynamic one, array(n),
            // is placed when it runs, and slot and size are the int slots that hold its place and length. A struct's
            // fields have a slot each, a nested struct's spread out in its place (struct_layout's order).
            struct Variable
            {
                int slot = -1;
                int size = 0;
                bool dynamic = false;
                vector<int> fields;
            };
            unordered_map<uint32_t, Variable> variables;  // by entity

            // A function or method, or a copy of one (key: function_key)
            struct Function
            {
                enum State : uint8_t { NotLowered, Lowering, Done } state = NotLowered;
                InstructionNode* head = nullptr;
                int slotBase = 0, slotCount = 0;  // its int slots
                vector<int> params, returns;      // their slots
                vector<VarType> paramTypes, returnTypes;
                vector<InstructionNode*> pending;  // calls made before its body was done
                int self = -1;                     // a method's: the slot of its object, its hidden parameter 0
            };
            unordered_map<uint64_t, Function> functions;  // by copy and FunctionDecl (function_key)
            unordered_map<uint32_t, uint32_t> methods;    // a method's FunctionDecl -> its ClassDecl

            // A class, or a generic class's copy (key: class_key): its id, which its objects record (a virtual call
            // runs the method of the object's class), and its fields, its parent's first, in the order an object
            // keeps them, each with the copy its type is read in
            struct Field
            {
                uint32_t decl = NoNode;  // FieldDecl
                uint32_t copy = NoInstance;
            };
            struct Class
            {
                int id = 0;
                vector<Field> fields;
                bool registered = false;  // its methods are in virtualTable
            };
            unordered_map<uint64_t, Class> classes;
            unordered_map<string, int> methodIds;    // a virtual call names its method by this id

            struct Loop
            {
                InstructionNode* breakTo;
                InstructionNode* continueTo;
            };
            vector<Loop> loops;  // the loops around the statement being lowered, innermost last
            uint32_t function = NoNode;  // the FunctionDecl being lowered; NoNode in the ignite block
            uint32_t instance = NoInstance;  // the generic function's or class's copy it belongs to

            // Types as the check pass gave them; in a copy, the copy's own
            TypeId type_id(uint32_t n) const { return node_type(in, n, instance); }
            VarType type_of(uint32_t n) const { return var_type(in.types[type_id(n)].kind); }
            TypeKind kind_of(uint32_t n) const { return in.types[type_id(n)].kind; }
            uint32_t entity_of(uint32_t n) const { return in.types[type_id(n)].a; }  // a struct's or class's
            bool container(uint32_t n) const { return kind_of(n) == TypeKind::Vector || kind_of(n) == TypeKind::Map; }
            TypeId var_type_id(uint32_t entity) const { return variable_type(in, entity, instance); }
            VarType var_type_of(uint32_t entity) const { return var_type(in.types[var_type_id(entity)].kind); }
            int line(uint32_t n) const { return tree.token(n).line; }
            string text(uint32_t n) const { return string(tree.spelling(n)); }

            // Instructions and slots
            InstructionNode* add(InstructionType type, int line);
            InstructionNode* label(int line);
            void append(InstructionNode* node);
            void jump(InstructionNode* target, int line);
            int new_slot(VarType t);
            int int_constant(int v);
            int zero(VarType t);
            InstructionNode* assign(VarType t, int dst, int a, ArithmeticOperatorType op, int b, int line);
            void copy(int dst, VarType dt, int src, VarType st, int line);
            int convert(int slot, VarType from, VarType to, int line);
            int negate(int v, VarType t, int line);
            int start(VarType t);
            int declare(uint32_t entity);

            // Definitions
            static uint64_t function_key(uint32_t fnCopy, uint32_t fn) { return (uint64_t)fnCopy << 32 | fn; }
            Function& current() { return functions[function_key(instance, function)]; }
            string copy_name(uint32_t decl, uint32_t c) const;
            Function& lower_function(uint32_t fn, uint32_t fnCopy = NoInstance);
            uint32_t call_copy(uint32_t n) const;
            void call_main();

            // Statements
            void block(uint32_t n);
            void statement(uint32_t n);
            void var_decl(uint32_t n);
            void multi_decl(uint32_t n);
            void assign_statement(uint32_t target, uint32_t value, int line);
            void store(uint32_t target, int value, VarType type, int line, bool temp = false);
            void compound_assign(uint32_t n);
            void declare_array(uint32_t entity, uint32_t call, int line);
            void if_statement(uint32_t n);
            void while_statement(uint32_t n);
            void do_while_statement(uint32_t n);
            void for_statement(uint32_t n);
            void switch_statement(uint32_t n);
            void return_statement(uint32_t n);

            // Expressions
            int expr(uint32_t n);
            int string_constant(uint32_t n);
            int unary(uint32_t n);
            int binary(uint32_t n);
            int arith(TokenType op, int left, VarType lt, int right, VarType rt, int line, VarType& t);
            int logical(uint32_t n);
            void condition(uint32_t n, InstructionNode* fail);
            void truth_test(int v, VarType t, bool wantTrue, InstructionNode* fail, int line);
            void compare(int left, VarType lt, ConditionalOperatorType op, int right, VarType rt, InstructionNode* fail,
                         int line);
            int call(uint32_t n);
            InstructionNode* call_instruction(uint32_t n, uint32_t fn, const vector<uint32_t>& args, vector<int>& slots,
                                              int object, uint32_t fnCopy);
            int call_function(uint32_t n, uint32_t fn, const vector<uint32_t>& args, int object = -1,
                              uint32_t fnCopy = NoInstance);
            void release_temporaries(const vector<uint32_t>& args, const vector<int>& slots, int line);
            void print(uint32_t n, uint32_t arg);
            void array_access(InstructionType type, uint32_t array, int index, int value, int line);

            // Tensors
            InstructionNode* tensor_call(TensorOp op, const vector<int>& args, const vector<VarType>& types, int result,
                                         int line);
            int tensor_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, int line);
            int tensor_binary(TokenType op, int left, VarType lt, int right, VarType rt, int line);
            void element(uint32_t index, vector<int>& args, vector<VarType>& types);
            int member(uint32_t n);

            // hf and sol
            int hf_call(uint32_t n, const string& method, const vector<uint32_t>& args);
            int sol_call(uint32_t n, const string& method, const vector<uint32_t>& args);

            // Structs
            VarType written_type(uint32_t type, uint32_t typeCopy) const;
            VarType field_type(uint32_t field, uint32_t fieldCopy = NoInstance) const;
            void struct_layout(uint32_t entity, vector<VarType>& types) const;
            vector<int> struct_slots(uint32_t n);
            vector<int> struct_value(uint32_t n, vector<VarType>& types);
            void struct_copy(const vector<int>& dst, uint32_t entity, uint32_t value, int line);

            // Classes: a class, or a generic class's copy, is named by its entity and its copy (class_key)
            static constexpr uint64_t NoClass = UINT64_MAX;
            static uint64_t class_key(uint32_t cls, uint32_t clsCopy) { return (uint64_t)clsCopy << 32 | cls; }
            uint64_t class_key(TypeId type) const;
            static uint32_t class_entity(uint64_t key) { return (uint32_t)key; }
            static uint32_t class_copy(uint64_t key) { return (uint32_t)(key >> 32); }
            uint64_t parent_of(uint64_t cls) const;
            uint32_t find_method(uint64_t cls, const string& method, uint32_t& methodCopy) const;
            const Class& class_of(uint64_t cls);
            int field_index(uint64_t cls, const string& name, Field& field);
            int method_id(const string& method);
            void register_class(uint64_t cls);
            bool temporary(uint32_t n) const;
            void release(int value, VarType type, int line);
            void object_store(int slot, int value, VarType type, bool temp, int line);
            int new_object(uint64_t cls, int line);
            void default_object(uint32_t n, uint32_t entity);
            int construct(uint32_t n, const vector<uint32_t>& args);
            int call_method(uint32_t n, uint32_t callee, const vector<uint32_t>& args);
            int field_read(uint32_t n);
            void field_write(uint32_t target, int value, VarType type, bool temp, int line);
            void field_element(InstructionType type, uint32_t field, int index, int value, int line);

            // Containers
            int container_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, VarType result, int line);
            int new_container(uint32_t type, uint32_t typeCopy, int line);
            int container_call(uint32_t n, uint32_t callee, const vector<uint32_t>& args);
            int container_get(uint32_t n);
            void container_set(uint32_t target, int value, VarType type, bool temp, int line);
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Instructions and slots
    // ─────────────────────────────────────────────────────────────────────────────

    InstructionNode* Lowerer::add(InstructionType type, int line)
    {
        InstructionNode* node = label(line);
        node->type = type;
        append(node);
        return node;
    }

    // A NOOP to jump to, appended where it belongs
    InstructionNode* Lowerer::label(int line)
    {
        InstructionNode* node = new InstructionNode();
        node->type = NOOP;
        node->line_no = line;
        return node;
    }

    void Lowerer::append(InstructionNode* node)
    {
        if (head == nullptr) head = node;
        else tail->next = node;
        tail = node;
    }

    void Lowerer::jump(InstructionNode* target, int line)
    {
        add(JMP, line)->jmp_inst.target = target;
    }

    // A slot for a value of type t; a tensor's is listed in g_tensorSlots
    int Lowerer::new_slot(VarType t)
    {
        if (t == TYPE_FLOAT) return alloc_float_slot();
        if (t == TYPE_DOUBLE) return alloc_double_slot();
        int s = alloc_slot();
        if (t == TYPE_TENSOR) g_tensorSlots.insert(s);
        return s;
    }

    int Lowerer::int_constant(int v)
    {
        int s = alloc_slot();
        mem[s] = v;
        return s;
    }

    int Lowerer::zero(VarType t)
    {
        int s = new_slot(t);
        if (t == TYPE_FLOAT) fmem[s] = 0.0f;
        else if (t == TYPE_DOUBLE) dmem[s] = 0.0;
        else mem[s] = 0;
        return s;
    }

    // dst = a op b in t's slots (op OPERATOR_NONE: dst = a)
    InstructionNode* Lowerer::assign(VarType t, int dst, int a, ArithmeticOperatorType op, int b, int line)
    {
        InstructionNode* node = add(t == TYPE_FLOAT ? ASSIGN_F : t == TYPE_DOUBLE ? ASSIGN_D : ASSIGN, line);
        if (t == TYPE_FLOAT) node->assign_f_inst = {dst, a, b, op};
        else if (t == TYPE_DOUBLE) node->assign_d_inst = {dst, a, b, op};
        else node->assign_inst = {dst, a, b, op, 0};
        return node;
    }

    // dst = src, where a float value may go into a double
    void Lowerer::copy(int dst, VarType dt, int src, VarType st, int line)
    {
        if (dt == TYPE_DOUBLE && st == TYPE_FLOAT)
        {
            add(CAST, line)->cast_inst = {src, dst, st, dt};
            return;
        }
        assign(dt, dst, src, OPERATOR_NONE, 0, line);
    }

    // A number in another number type's slots (int < float < double)
    int Lowerer::convert(int slot, VarType from, VarType to, int line)
    {
        if (from == to) return slot;
        int dst = new_slot(to);
        add(CAST, line)->cast_inst = {slot, dst, from, to};
        return dst;
    }

    // -v: 0 - v in v's number type; a tensor is scaled by -1
    int Lowerer::negate(int v, VarType t, int line)
    {
        if (t == TYPE_TENSOR)
        {
            int minusOne = alloc_double_slot();
            dmem[minusOne] = -1.0;
            return tensor_op(TEN_SCALE, {v, minusOne}, {TYPE_TENSOR, TYPE_DOUBLE}, line);
        }
        VarType nt = t == TYPE_FLOAT || t == TYPE_DOUBLE ? t : TYPE_INT;
        int dst = new_slot(nt);
        assign(nt, dst, zero(nt), OPERATOR_MINUS, v, line);
        return dst;
    }

    // What a variable starts as: 0, or -1 for a Tensor, which no tensor is, so using it before it is set is an error
    int Lowerer::start(VarType t)
    {
        return t == TYPE_TENSOR ? int_constant(-1) : zero(t);
    }

    // A variable's slot, made where it is declared; a struct's slots, one per field
    int Lowerer::declare(uint32_t entity)
    {
        const Type& type = in.types[var_type_id(entity)];
        if (type.kind == TypeKind::Struct)
        {
            vector<VarType> types;
            struct_layout(type.a, types);
            for (VarType t : types) variables[entity].fields.push_back(new_slot(t));
            return -1;
        }
        VarType t = var_type_of(entity);
        int slot = new_slot(t);
        variables[entity].slot = slot;
        if (function == NoNode)
        {
            string name(names.text(in.entities[entity].name));
            (t == TYPE_FLOAT ? floatSymbolTable : t == TYPE_DOUBLE ? doubleSymbolTable : symbolTable)[name] = slot;
        }
        return slot;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Definitions
    // ─────────────────────────────────────────────────────────────────────────────

    // Every function and method, in file order, then the ignite block (or a call to main() when there is none). A
    // generic class's methods are lowered once per copy, and a generic function's copies when a call needs them. The
    // classes' methods go into virtualTable once all are lowered.
    InstructionNode* Lowerer::run()
    {
        for (uint32_t n : tree.children(tree.root()))
        {
            if (tree.kind(n) != NodeKind::ClassDecl) continue;
            for (uint32_t m : tree.children(n))
            {
                if (tree.kind(m) == NodeKind::FunctionDecl) methods[m] = n;
            }
        }
        vector<uint64_t> classKeys;
        uint32_t ignite = NoNode;
        for (uint32_t n : tree.children(tree.root()))
        {
            if (tree.kind(n) == NodeKind::FunctionDecl && !generic(tree, in, n)) lower_function(n);
            else if (tree.kind(n) == NodeKind::Ignite) ignite = n;
            else if (tree.kind(n) == NodeKind::ClassDecl)
            {
                for (uint32_t c : copies_of(tree, in, n))
                {
                    classKeys.push_back(class_key(in.refs[n], c));
                    for (uint32_t m : tree.children(n))
                    {
                        if (tree.kind(m) == NodeKind::FunctionDecl) lower_function(m, c);
                    }
                }
            }
        }
        if (ignite != NoNode) block(tree.children(ignite)[0]);
        else call_main();
        for (uint64_t c : classKeys) register_class(c);
        if (head == nullptr) add(NOOP, ignite != NoNode ? line(ignite) : 0);
        return head;
    }

    // A copy's name, as the old parser names it: scale<int, double>, Box<int>
    string Lowerer::copy_name(uint32_t decl, uint32_t c) const
    {
        string s = text(decl);
        const vector<TypeId>& args = in.instances[c].args;
        for (size_t i = 0; i < args.size(); i++) s += (i == 0 ? "<" : ", ") + type_name(in, args[i]);
        return s + ">";
    }

    // A function's body (or a copy's), once. Its parameters, return slots and locals are its int slot range; it ends
    // with a return of whatever is in its return slot, for a body that ends without one. A method's object is in a
    // slot of the range too (self); a method is in classMethodTable, by its class's name (a copy's: Box<int>), where
    // the tensor-free pass finds it.
    Lowerer::Function& Lowerer::lower_function(uint32_t fn, uint32_t fnCopy)
    {
        Function& f = functions[function_key(fnCopy, fn)];
        if (f.state != Function::NotLowered) return f;
        f.state = Function::Lowering;
        InstructionNode* outerHead = head;
        InstructionNode* outerTail = tail;
        vector<Loop> outerLoops = loops;
        uint32_t outerFunction = function, outerInstance = instance;
        head = tail = nullptr;
        loops.clear();
        function = fn;
        instance = fnCopy;

        const Signature& sig = *signature_of(in, fn, instance);
        vector<uint32_t> kids = tree.children(fn);  // ParamList, ReturnTypes if written, Block
        f.slotBase = next_available;
        for (uint32_t p : tree.children(kids[0]))
        {
            if (tree.token(p).kind == SELF) continue;
            f.params.push_back(declare(in.refs[p]));
            f.paramTypes.push_back(var_type_of(in.refs[p]));
        }
        for (TypeId t : sig.returns) f.returnTypes.push_back(var_type(in.types[t].kind));
        if (f.returnTypes.empty()) f.returnTypes.push_back(TYPE_UNKNOWN);  // returns nothing
        for (VarType t : f.returnTypes) f.returns.push_back(new_slot(t));
        bool method = methods.count(fn);
        bool functionCopy = instance != NoInstance && !method;
        string name = functionCopy ? copy_name(fn, instance) : text(fn);
        if (method) f.self = alloc_slot();
        else
        {
            functionParamTypes[name] = f.paramTypes;
            functionReturnType[name] = f.returnTypes[0];
            functionReturnSlots[name] = f.returns;
        }

        block(kids.back());
        add(RET, line(fn))->ret_inst = {f.returns[0], f.returnTypes[0]};
        f.head = head;
        f.slotCount = next_available - f.slotBase;
        if (method)
        {
            string cls = instance == NoInstance ? text(methods[fn]) : copy_name(methods[fn], instance);
            classMethodTable[cls][name] = head;
        }
        else functionTable[name] = head;
        for (InstructionNode* call : f.pending)
        {
            call->call_inst.function_head = f.head;
            call->call_inst.func_slot_base = f.slotBase;
            call->call_inst.func_slot_count = f.slotCount;
        }
        f.pending.clear();
        f.state = Function::Done;

        head = outerHead;
        tail = outerTail;
        loops = outerLoops;
        function = outerFunction;
        instance = outerInstance;
        return f;
    }

    // The copy of a generic function that call n runs (the check pass made it); NoInstance for any other call
    uint32_t Lowerer::call_copy(uint32_t n) const
    {
        const unordered_map<uint32_t, uint32_t>& calls =
            instance == NoInstance ? in.calls : in.instances[instance].calls;
        auto it = calls.find(n);
        return it == calls.end() ? NoInstance : it->second;
    }

    // A file without an ignite block runs def main()
    void Lowerer::call_main()
    {
        for (uint32_t n : tree.children(tree.root()))
        {
            if (tree.kind(n) == NodeKind::FunctionDecl && text(n) == "main") call_function(n, n, {});
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Statements
    // ─────────────────────────────────────────────────────────────────────────────

    void Lowerer::block(uint32_t n)
    {
        for (uint32_t s : tree.children(n)) statement(s);
    }

    void Lowerer::statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        switch (tree.kind(n))
        {
            case NodeKind::VarDecl:         var_decl(n); return;
            case NodeKind::MultiDecl:       multi_decl(n); return;
            case NodeKind::Assign:          assign_statement(kids[0], kids[1], line(n)); return;
            case NodeKind::CompoundAssign:  compound_assign(n); return;
            case NodeKind::ExprStatement:   // a temporary object nothing takes (a call's result) is dropped
            {
                int v = expr(kids[0]);
                if (temporary(kids[0])) release(v, type_of(kids[0]), line(n));
                return;
            }
            case NodeKind::If:              if_statement(n); return;
            case NodeKind::While:           while_statement(n); return;
            case NodeKind::DoWhile:         do_while_statement(n); return;
            case NodeKind::For:             for_statement(n); return;
            case NodeKind::Switch:          switch_statement(n); return;
            case NodeKind::Return:          return_statement(n); return;
            case NodeKind::Break:           jump(loops.back().breakTo, line(n)); return;
            case NodeKind::Continue:        jump(loops.back().continueTo, line(n)); return;
            default:                        return;  // Pass, or an Empty part of a for header
        }
    }

    // int x = e, or int x: a variable starts as start() says each time the declaration runs (a loop body runs it
    // again); a struct's fields each do. Point p alone is a new object, its init run if it takes no arguments, and
    // vector<int> v a new, empty vector.
    void Lowerer::var_decl(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        uint32_t e = in.refs[n];
        const Type& type = in.types[var_type_id(e)];
        VarType t = var_type_of(e);
        if (type.kind == TypeKind::Struct)
        {
            declare(e);
            const vector<int>& fields = variables[e].fields;
            if (kids.size() > 1)
            {
                struct_copy(fields, type.a, kids[1], line(n));
                return;
            }
            vector<VarType> types;
            struct_layout(type.a, types);
            for (size_t i = 0; i < fields.size(); i++) assign(types[i], fields[i], start(types[i]), OPERATOR_NONE, 0, line(n));
            return;
        }
        if (kids.size() == 1)
        {
            if (t == TYPE_CLASS) default_object(n, e);
            else if (reference(t))
            {
                int h = new_container(kids[0], instance, line(n));
                object_store(declare(e), h, t, true, line(n));
            }
            else assign(t, declare(e), start(t), OPERATOR_NONE, 0, line(n));
            return;
        }
        int value = expr(kids[1]);
        if (reference(t)) object_store(declare(e), value, t, temporary(kids[1]), line(n));
        else copy(declare(e), t, value, type_of(kids[1]), line(n));
    }

    // int q, int r = divmod(a, b): one variable per value the function returns, each copied from its return slot
    void Lowerer::multi_decl(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        vector<uint32_t> call = tree.children(kids.back());
        uint32_t fn = in.entities[in.refs[call[0]]].decl;
        vector<uint32_t> args(call.begin() + 1, call.end());
        vector<int> slots;
        InstructionNode* node = call_instruction(kids.back(), fn, args, slots, -1, NoInstance);
        const Function& f = functions[function_key(NoInstance, fn)];
        node->call_inst.all_ret_slots = array_of(f.returns);
        node->call_inst.ret_slot_types = array_of(f.returnTypes);
        node->call_inst.num_ret_slots = (int)f.returns.size();
        for (size_t i = 0; i + 1 < kids.size(); i++)
        {
            uint32_t e = in.refs[kids[i]];
            copy(declare(e), var_type_of(e), f.returns[i], f.returnTypes[i], line(n));
        }
        release_temporaries(args, slots, line(n));
    }

    // x = e (declaring x if this is its first assignment), a[i] = e, obj.f = e; a struct is copied field by field
    void Lowerer::assign_statement(uint32_t target, uint32_t value, int line)
    {
        bool declares = false;
        if (tree.kind(target) == NodeKind::Name)
        {
            uint32_t e = in.refs[target];
            declares = in.entities[e].decl == target;
            if (declares && in.types[var_type_id(e)].kind == TypeKind::Array)
            {
                declare_array(e, value, line);
                return;
            }
        }
        if (kind_of(value) == TypeKind::Struct)  // q = p, l.a = Point(1, 2)
        {
            if (declares) declare(in.refs[target]);
            struct_copy(struct_slots(target), entity_of(value), value, line);
            return;
        }
        int v = expr(value);
        if (declares) declare(in.refs[target]);
        store(target, v, type_of(value), line, temporary(value));
    }

    // The value in slot value, of the given type, into a variable, an array's, a tensor's or a container's element,
    // or a field. An object or container variable, field or element takes a temporary's reference (temp), or one of
    // its own.
    void Lowerer::store(uint32_t target, int value, VarType type, int line, bool temp)
    {
        if (tree.kind(target) == NodeKind::Member)
        {
            if (kind_of(tree.children(target)[0]) == TypeKind::Class) field_write(target, value, type, temp, line);
            else copy(struct_slots(target)[0], type_of(target), value, type, line);  // a struct variable's field
            return;
        }
        if (tree.kind(target) == NodeKind::Index)
        {
            vector<uint32_t> kids = tree.children(target);
            if (type_of(kids[0]) == TYPE_TENSOR)  // t[i, j] = v
            {
                vector<int> args;
                vector<VarType> types;
                element(target, args, types);
                args.push_back(value);
                types.push_back(type);
                tensor_op(TEN_SET, args, types, line);
                return;
            }
            if (container(kids[0]))  // v[i] = x, m[k] = x, self.ids[w] = x
            {
                container_set(target, value, type, temp, line);
                return;
            }
            if (tree.kind(kids[0]) == NodeKind::Member)  // self.data[i] = v
            {
                field_element(ARRAY_WRITE, kids[0], expr(kids[1]), value, line);
                return;
            }
            int index = expr(kids[1]);
            array_access(ARRAY_WRITE, kids[0], index, value, this->line(kids[0]));
            return;
        }
        uint32_t e = in.refs[target];
        if (reference(var_type_of(e))) object_store(variables[e].slot, value, var_type_of(e), temp, line);
        else copy(variables[e].slot, var_type_of(e), value, type, line);
    }

    // x += e is x = x + e
    void Lowerer::compound_assign(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        int left = expr(kids[0]);
        int right = expr(kids[1]);
        TokenType op;
        switch (tree.token(n).kind)
        {
            case PLUSEQ:    op = PLUS; break;
            case MINUSEQ:   op = MINUS; break;
            case MULTEQ:    op = MULT; break;
            default:        op = DIV; break;
        }
        VarType t;
        int v = arith(op, left, type_of(kids[0]), right, type_of(kids[1]), line(n), t);
        store(kids[0], v, t, line(n));
    }

    // a = array(5) takes five slots here; a = array(n) is placed by ALLOC when it runs
    void Lowerer::declare_array(uint32_t entity, uint32_t call, int line)
    {
        uint32_t size = tree.children(call)[1];
        Variable& a = variables[entity];
        if (tree.kind(size) == NodeKind::IntLiteral)
        {
            a.size = stoi(text(size));
            a.slot = next_available;
            for (int i = 0; i < a.size; i++) alloc_slot();
            return;
        }
        int n = expr(size);
        a.dynamic = true;
        a.slot = alloc_slot();
        a.size = alloc_slot();
        assign(TYPE_INT, a.size, n, OPERATOR_NONE, 0, line);
        add(ALLOC, line)->alloc_inst = {a.slot, a.size};
    }

    // Each condition jumps past its block when it is false; a block that runs jumps to the end
    void Lowerer::if_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);  // the condition, Block, then Elif... and Else
        InstructionNode* end = label(line(n));
        InstructionNode* next = label(line(n));
        condition(kids[0], next);
        block(kids[1]);
        jump(end, line(n));
        append(next);
        for (size_t i = 2; i < kids.size(); i++)
        {
            vector<uint32_t> part = tree.children(kids[i]);
            if (tree.kind(kids[i]) == NodeKind::Else)
            {
                block(part[0]);
                break;
            }
            next = label(line(kids[i]));
            condition(part[0], next);
            block(part[1]);
            jump(end, line(kids[i]));
            append(next);
        }
        append(end);
    }

    void Lowerer::while_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        InstructionNode* start = label(line(n));
        InstructionNode* end = label(line(n));
        append(start);
        condition(kids[0], end);
        loops.push_back({end, start});
        block(kids[1]);
        loops.pop_back();
        jump(start, line(n));
        append(end);
    }

    // 'continue' goes to the condition, after the body
    void Lowerer::do_while_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);  // Block, the condition
        InstructionNode* start = label(line(n));
        InstructionNode* test = label(line(n));
        InstructionNode* end = label(line(n));
        append(start);
        loops.push_back({end, test});
        block(kids[0]);
        loops.pop_back();
        append(test);
        condition(kids[1], end);
        jump(start, line(n));
        append(end);
    }

    // for (start; condition; step): 'continue' runs the step, then the condition
    void Lowerer::for_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);  // the start, the condition, the step (each may be Empty), Block
        statement(kids[0]);
        InstructionNode* start = label(line(n));
        InstructionNode* step = label(line(n));
        InstructionNode* end = label(line(n));
        append(start);
        if (tree.kind(kids[1]) != NodeKind::Empty) condition(kids[1], end);
        loops.push_back({end, step});
        block(kids[3]);
        loops.pop_back();
        append(step);
        statement(kids[2]);
        jump(start, line(n));
        append(end);
    }

    // The value is compared with each case in turn; a case that matches jumps to its block, and each block ends by
    // jumping past the others (cases never fall through). A 'break' inside leaves the enclosing loop.
    void Lowerer::switch_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);  // the value, then Case... and Default
        int value = alloc_slot();
        assign(TYPE_INT, value, expr(kids[0]), OPERATOR_NONE, 0, line(n));
        InstructionNode* end = label(line(n));
        InstructionNode* otherwise = end;
        vector<pair<InstructionNode*, uint32_t>> bodies;  // each block and the label before it
        for (size_t i = 1; i < kids.size(); i++)
        {
            vector<uint32_t> part = tree.children(kids[i]);  // Case: the value, Block; Default: Block
            InstructionNode* start = label(line(kids[i]));
            bodies.push_back({start, part.back()});
            if (tree.kind(kids[i]) == NodeKind::Default)
            {
                otherwise = start;
                continue;
            }
            InstructionNode* test = add(CJMP, line(kids[i]));  // falls through when the value differs
            test->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
            test->cjmp_inst.operand1_index = value;
            test->cjmp_inst.operand2_index = expr(part[0]);
            test->cjmp_inst.target = start;
            test->cjmp_inst.operand_type = TYPE_INT;
        }
        jump(otherwise, line(n));
        for (const auto& [start, body] : bodies)
        {
            append(start);
            block(body);
            jump(end, line(n));
        }
        append(end);
    }

    // The values go into the function's return slots, then RET
    void Lowerer::return_statement(uint32_t n)
    {
        const Function& f = current();
        vector<uint32_t> values = tree.children(n);
        vector<int> slots;
        for (uint32_t v : values) slots.push_back(expr(v));
        for (size_t i = 0; i < values.size(); i++) copy(f.returns[i], f.returnTypes[i], slots[i], type_of(values[i]), line(n));
        add(RET, line(n))->ret_inst = {f.returns[0], f.returnTypes[0]};
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Expressions
    // ─────────────────────────────────────────────────────────────────────────────

    // The slot that holds an expression's value
    int Lowerer::expr(uint32_t n)
    {
        switch (tree.kind(n))
        {
            case NodeKind::IntLiteral:      return int_constant(stoi(text(n)));
            case NodeKind::FloatLiteral:
            {
                int s = alloc_float_slot();
                fmem[s] = stof(text(n));
                return s;
            }
            case NodeKind::StringLiteral:   return string_constant(n);
            case NodeKind::BoolLiteral:     return int_constant(tree.token(n).kind == TRUE);
            case NodeKind::Name:            // none is the handle 0, no object
            {
                if (in.entities[in.refs[n]].kind == EntityKind::None) return int_constant(0);
                return variables[in.refs[n]].slot;
            }
            case NodeKind::Self:            return current().self;
            case NodeKind::Paren:           return expr(tree.children(n)[0]);
            case NodeKind::Unary:           return unary(n);
            case NodeKind::Binary:          return binary(n);
            case NodeKind::Call:            return call(n);
            case NodeKind::Member:          return member(n);
            case NodeKind::Index:           // a[i], t[i, j], self.data[i], v[i], m[k]
            {
                if (type_of(tree.children(n)[0]) == TYPE_TENSOR)
                {
                    vector<int> args;
                    vector<VarType> types;
                    element(n, args, types);
                    return tensor_op(TEN_GET, args, types, line(n));
                }
                if (container(tree.children(n)[0])) return container_get(n);
                vector<uint32_t> kids = tree.children(n);
                int index = expr(kids[1]);
                int value = alloc_slot();
                if (tree.kind(kids[0]) == NodeKind::Member) field_element(ARRAY_READ, kids[0], index, value, line(n));
                else array_access(ARRAY_READ, kids[0], index, value, line(kids[1]));
                return value;
            }
            default:                        return 0;
        }
    }

    // A string is an int slot holding its index in strMem
    int Lowerer::string_constant(uint32_t n)
    {
        int index = next_str_available++;
        strMem.push_back(string(names.text(tree.token(n).symbol)));
        return int_constant(index);
    }

    // not x; -x, where a literal folds into its constant (-2, -0.5)
    int Lowerer::unary(uint32_t n)
    {
        uint32_t x = tree.children(n)[0];
        if (tree.token(n).kind == NOT)
        {
            int v = expr(x);
            int result = alloc_slot();
            InstructionNode* done = label(line(n));
            assign(TYPE_INT, result, int_constant(0), OPERATOR_NONE, 0, line(n));
            truth_test(v, type_of(x), false, done, line(n));
            assign(TYPE_INT, result, int_constant(1), OPERATOR_NONE, 0, line(n));
            append(done);
            return result;
        }
        if (tree.kind(x) == NodeKind::IntLiteral) return int_constant((int)-stoll(text(x)));
        if (tree.kind(x) == NodeKind::FloatLiteral)
        {
            int s = alloc_float_slot();
            fmem[s] = -stof(text(x));
            return s;
        }
        return negate(expr(x), type_of(x), line(n));
    }

    bool comparison(TokenType t, ConditionalOperatorType& op)
    {
        switch (t)
        {
            case LESS:      op = CONDITION_LESS;          return true;
            case GREATER:   op = CONDITION_GREATER;       return true;
            case LESSEQ:    op = CONDITION_LESS_EQUAL;    return true;
            case GREATEREQ: op = CONDITION_GREATER_EQUAL; return true;
            case EQEQ:      op = CONDITION_EQUAL;         return true;
            case NOTEQUAL:  op = CONDITION_NOTEQUAL;      return true;
            default:        return false;
        }
    }

    // Arithmetic; a comparison as a value: result = 0, then 1 if it holds; and / or
    int Lowerer::binary(uint32_t n)
    {
        TokenType op = tree.token(n).kind;
        if (op == AND || op == OR) return logical(n);
        vector<uint32_t> kids = tree.children(n);
        int left = expr(kids[0]);
        int right = expr(kids[1]);
        ConditionalOperatorType cond;
        if (comparison(op, cond))
        {
            int result = alloc_slot();
            InstructionNode* done = label(line(n));
            assign(TYPE_INT, result, int_constant(0), OPERATOR_NONE, 0, line(n));
            compare(left, type_of(kids[0]), cond, right, type_of(kids[1]), done, line(n));
            assign(TYPE_INT, result, int_constant(1), OPERATOR_NONE, 0, line(n));
            append(done);
            if (temporary(kids[0])) release(left, type_of(kids[0]), line(n));  // make() == none: a temporary object,
            if (temporary(kids[1])) release(right, type_of(kids[1]), line(n));  // once compared
            return result;
        }
        VarType t;
        return arith(op, left, type_of(kids[0]), right, type_of(kids[1]), line(n), t);
    }

    // left op right: string + string concatenates; numbers are computed in the wider of their types (an int with an
    // int stays an int). a % b on ints is a - (a / b) * b, as in C. A tensor on either side, or @, is a tensor
    // operation. The result's type goes in t.
    int Lowerer::arith(TokenType op, int left, VarType lt, int right, VarType rt, int line, VarType& t)
    {
        if (op == AT || lt == TYPE_TENSOR || rt == TYPE_TENSOR)
        {
            t = TYPE_TENSOR;
            return tensor_binary(op, left, lt, right, rt, line);
        }
        if (op == PLUS && lt == TYPE_STRING)
        {
            t = TYPE_STRING;
            int dest = alloc_slot();
            add(STRCAT, line)->strcat_inst = {dest, left, right};
            return dest;
        }
        if (op == MOD)
        {
            t = TYPE_INT;
            int quotient = alloc_slot(), product = alloc_slot(), rem = alloc_slot();
            assign(TYPE_INT, quotient, left, OPERATOR_DIV, right, line);
            assign(TYPE_INT, product, quotient, OPERATOR_MULT, right, line);
            assign(TYPE_INT, rem, left, OPERATOR_MINUS, product, line);
            return rem;
        }
        t = (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) ? TYPE_DOUBLE : (lt == TYPE_FLOAT || rt == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
        if (t != TYPE_INT)
        {
            left = convert(left, lt, t, line);
            right = convert(right, rt, t, line);
        }
        ArithmeticOperatorType o = op == PLUS ? OPERATOR_PLUS : op == MINUS ? OPERATOR_MINUS : op == MULT ? OPERATOR_MULT
                                                                                                         : OPERATOR_DIV;
        int dest = new_slot(t);
        assign(t, dest, left, o, right, line);
        return dest;
    }

    // The operands of a chain of one operator: a and b and c
    void chain(const ParseTree& tree, uint32_t n, TokenType op, vector<uint32_t>& operands)
    {
        if (tree.kind(n) == NodeKind::Binary && tree.token(n).kind == op)
        {
            vector<uint32_t> kids = tree.children(n);
            chain(tree, kids[0], op, operands);
            operands.push_back(kids[1]);
            return;
        }
        operands.push_back(n);
    }

    // a and b and ...: result = 0, each operand must be true to go on, result = 1 if all were. a or b or ...: result
    // = 1, any true operand jumps to the end, result = 0 if none was. Neither evaluates past the operand that decides.
    int Lowerer::logical(uint32_t n)
    {
        bool isAnd = tree.token(n).kind == AND;
        vector<uint32_t> operands;
        chain(tree, n, tree.token(n).kind, operands);
        int result = alloc_slot();
        InstructionNode* done = label(line(n));
        assign(TYPE_INT, result, int_constant(isAnd ? 0 : 1), OPERATOR_NONE, 0, line(n));
        for (uint32_t o : operands)
        {
            int v = expr(o);
            truth_test(v, type_of(o), isAnd, done, line(n));
        }
        assign(TYPE_INT, result, int_constant(isAnd ? 1 : 0), OPERATOR_NONE, 0, line(n));
        append(done);
        return result;
    }

    // Falls through when condition n holds and jumps to fail when it does not. A comparison jumps on itself (unless
    // it has a temporary object to drop after, both ways); any other value is true when it is not zero.
    void Lowerer::condition(uint32_t n, InstructionNode* fail)
    {
        while (tree.kind(n) == NodeKind::Paren) n = tree.children(n)[0];
        ConditionalOperatorType op;
        vector<uint32_t> kids = tree.children(n);
        if (tree.kind(n) == NodeKind::Binary && comparison(tree.token(n).kind, op) && !temporary(kids[0]) &&
            !temporary(kids[1]))
        {
            int left = expr(kids[0]);
            int right = expr(kids[1]);
            compare(left, type_of(kids[0]), op, right, type_of(kids[1]), fail, line(n));
            return;
        }
        int v = expr(n);
        truth_test(v, type_of(n), true, fail, line(n));
    }

    // Falls through when v's truth (not zero) is wantTrue, and jumps to fail when it is not
    void Lowerer::truth_test(int v, VarType t, bool wantTrue, InstructionNode* fail, int line)
    {
        VarType zt = t == TYPE_FLOAT || t == TYPE_DOUBLE ? t : TYPE_INT;
        compare(v, zt, wantTrue ? CONDITION_NOTEQUAL : CONDITION_EQUAL, zero(zt), zt, fail, line);
    }

    // Falls through when (left op right) holds and jumps to fail when it does not. Strings compare as strings;
    // numbers are first converted to the wider of their types.
    void Lowerer::compare(int left, VarType lt, ConditionalOperatorType op, int right, VarType rt, InstructionNode* fail,
                          int line)
    {
        if (lt == TYPE_STRING || rt == TYPE_STRING)
        {
            InstructionNode* node = add(SCMP, line);
            node->scmp_inst = {op, left, right, fail};
            return;
        }
        VarType t = (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) ? TYPE_DOUBLE : (lt == TYPE_FLOAT || rt == TYPE_FLOAT) ? TYPE_FLOAT
                                                                                                                  : TYPE_INT;
        if (t != TYPE_INT)
        {
            left = convert(left, lt, t, line);
            right = convert(right, rt, t, line);
        }
        InstructionNode* node = add(CJMP, line);
        node->cjmp_inst.condition_op = op;
        node->cjmp_inst.operand1_index = left;
        node->cjmp_inst.operand2_index = right;
        node->cjmp_inst.target = fail;
        node->cjmp_inst.operand_type = t;
    }

    // f(args), obj.m(args), Counter(args), vector<int>(), v.push(x), print(x), input(), int(x) / float(x) / double(x),
    // zeros(2, 3) and the other tensor built-ins, and hf.infer(...), sol.wait(...). array(n) is lowered by the
    // assignment it sets, and a struct's Point(3, 4) by the declaration or assignment it is the value of.
    int Lowerer::call(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        uint32_t callee = kids[0];
        vector<uint32_t> args(kids.begin() + 1, kids.end());
        if (tree.kind(callee) == NodeKind::TypeName)
        {
            TokenType k = tree.token(callee).kind;
            VarType to = k == FLOAT_TYPE ? TYPE_FLOAT : k == DOUBLE_TYPE ? TYPE_DOUBLE : TYPE_INT;
            int src = expr(args[0]);
            int dst = new_slot(to);
            add(CAST, line(n))->cast_inst = {src, dst, type_of(args[0]), to};
            return dst;
        }
        if (tree.kind(callee) == NodeKind::GenericType)  // vector<int>(), Box<int>(5)
        {
            if (kind_of(n) == TypeKind::Class) return construct(n, args);
            return new_container(callee, instance, line(n));
        }
        if (tree.kind(callee) == NodeKind::Member)  // obj.m(...), v.push(...), hf.infer(...), sol.wait(...)
        {
            if (kind_of(tree.children(callee)[0]) == TypeKind::Class) return call_method(n, callee, args);
            if (container(tree.children(callee)[0])) return container_call(n, callee, args);
            string module = text(tree.children(callee)[0]), method = text(callee);
            return module == "hf" ? hf_call(n, method, args) : sol_call(n, method, args);
        }
        const Entity& e = in.entities[in.refs[callee]];
        if (e.kind == EntityKind::Function) return call_function(n, e.decl, args, -1, call_copy(n));
        if (e.kind == EntityKind::Class) return construct(n, args);
        string_view f = names.text(e.name);
        auto op = TENSOR_OP_MAP.find(string(f));
        if (op != TENSOR_OP_MAP.end())
        {
            vector<int> slots;
            vector<VarType> types;
            for (uint32_t a : args)
            {
                slots.push_back(expr(a));
                types.push_back(type_of(a));
            }
            return tensor_op(op->second, slots, types, line(n));
        }
        if (f == "print") print(n, args[0]);
        if (f != "input") return 0;
        int slot = alloc_slot();
        add(IN, line(n))->input_inst.var_index = slot;
        return slot;
    }

    // The arguments (their slots go in slots), then the CALL: it copies them into the function's parameter slots and
    // saves its int slots. A method's object (object: its slot, or -1 for a function) is its hidden parameter 0, and
    // a method's call is virtual (init's excepted): it runs the method of the object's own class. fnCopy is the copy
    // of a generic function, or the class copy a method is in.
    InstructionNode* Lowerer::call_instruction(uint32_t n, uint32_t fn, const vector<uint32_t>& args, vector<int>& slots,
                                               int object, uint32_t fnCopy)
    {
        vector<VarType> types;
        for (uint32_t a : args)
        {
            slots.push_back(expr(a));
            types.push_back(type_of(a));
        }
        Function& f = lower_function(fn, fnCopy);
        vector<int> params = f.params, values = slots;
        vector<VarType> paramTypes = f.paramTypes;
        if (object >= 0)
        {
            params.insert(params.begin(), f.self);
            paramTypes.insert(paramTypes.begin(), TYPE_CLASS);
            values.insert(values.begin(), object);
            types.insert(types.begin(), TYPE_CLASS);
        }
        InstructionNode* node = add(CALL, line(n));
        node->call_inst.ret_val_index = f.returns[0];
        node->call_inst.ret_type = f.returnTypes[0];
        node->call_inst.num_params = (int)params.size();
        if (object >= 0 && text(fn) != "init") node->call_inst.virtual_method = method_id(text(fn));
        if (!params.empty())
        {
            node->call_inst.param_slots = array_of(params);
            node->call_inst.param_types = array_of(paramTypes);
            node->call_inst.arg_val_slots = array_of(values);
            node->call_inst.arg_types = array_of(types);
        }
        if (f.state == Function::Lowering)
        {
            f.pending.push_back(node);
            return node;
        }
        node->call_inst.function_head = f.head;
        node->call_inst.func_slot_base = f.slotBase;
        node->call_inst.func_slot_count = f.slotCount;
        return node;
    }

    // f(args) as a value: its first result, copied out of the function's return slot before another call reuses it.
    // The temporary objects passed to it are then dropped (its parameters had their own references).
    int Lowerer::call_function(uint32_t n, uint32_t fn, const vector<uint32_t>& args, int object, uint32_t fnCopy)
    {
        vector<int> slots;
        InstructionNode* node = call_instruction(n, fn, args, slots, object, fnCopy);
        VarType t = node->call_inst.ret_type;
        int result = new_slot(t);
        assign(t, result, node->call_inst.ret_val_index, OPERATOR_NONE, 0, line(n));
        release_temporaries(args, slots, line(n));
        return result;
    }

    void Lowerer::release_temporaries(const vector<uint32_t>& args, const vector<int>& slots, int line)
    {
        for (size_t i = 0; i < args.size(); i++)
        {
            if (temporary(args[i])) release(slots[i], type_of(args[i]), line);
        }
    }

    // print(x): a string literal is printed from strMem directly
    void Lowerer::print(uint32_t n, uint32_t arg)
    {
        InstructionNode* node;
        if (tree.kind(arg) == NodeKind::StringLiteral)
        {
            node = add(OUT, line(n));
            node->output_inst.var_index = next_str_available++;
            strMem.push_back(string(names.text(tree.token(arg).symbol)));
            node->output_inst.is_string = true;
            node->output_inst.value_type = TYPE_STRING;
        }
        else
        {
            int v = expr(arg);
            VarType t = type_of(arg);
            node = add(OUT, line(n));
            node->output_inst.var_index = v;
            node->output_inst.is_string = node->output_inst.is_string_var = t == TYPE_STRING;
            node->output_inst.value_type = t;
        }
        node->output_inst.newline = true;
    }

    // a[i]: ARRAY_READ copies the element into value, ARRAY_WRITE copies value into it, checking i against the size
    void Lowerer::array_access(InstructionType type, uint32_t array, int index, int value, int line)
    {
        const Variable& a = variables[in.refs[array]];
        InstructionNode* node = add(type, line);
        node->array_inst.base_index = a.slot;
        node->array_inst.dynamic_base = a.dynamic;
        node->array_inst.index_slot = index;
        node->array_inst.target_index = value;
        node->array_inst.array_size = a.dynamic ? -1 : a.size;
        node->array_inst.size_slot = a.dynamic ? a.size : -1;
        node->array_inst.line_no = line;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Tensors
    // ─────────────────────────────────────────────────────────────────────────────

    // A TENSOR_CALL: a tensor built-in, or an object operation (OBJ_NEW, OBJ_GET, ...), which the runtime runs as
    // one. result is its result's slot, or -1.
    InstructionNode* Lowerer::tensor_call(TensorOp op, const vector<int>& args, const vector<VarType>& types, int result,
                                          int line)
    {
        InstructionNode* node = add(TENSOR_CALL, line);
        node->tensor_call_inst.op = op;
        node->tensor_call_inst.result_slot = result;
        node->tensor_call_inst.num_args = (int)args.size();
        for (size_t i = 0; i < args.size(); i++)
        {
            node->tensor_call_inst.arg_slots[i] = args[i];
            node->tensor_call_inst.arg_types[i] = types[i];
        }
        return node;
    }

    // A tensor built-in. Its result is a tensor's handle, an int or a double, or nothing (tensor_print); a void op's
    // value is a slot of its own that nothing reads.
    int Lowerer::tensor_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, int line)
    {
        bool isVoid = TENSOR_VOID_OPS.count(op), isDouble = TENSOR_DOUBLE_OPS.count(op), isInt = TENSOR_INT_OPS.count(op);
        int result = isVoid ? -1 : new_slot(isDouble ? TYPE_DOUBLE : isInt ? TYPE_INT : TYPE_TENSOR);
        tensor_call(op, args, types, result, line);
        return isVoid ? alloc_slot() : result;
    }

    // Two tensors: + - * / element by element, @ is matmul. A tensor and a number: * scales, / scales by 1 / the
    // number, and + and - add the number to every element: t - k is t + (-k), and k - t is (-t) + k.
    int Lowerer::tensor_binary(TokenType op, int left, VarType lt, int right, VarType rt, int line)
    {
        if (op == AT) return tensor_op(TEN_MATMUL, {left, right}, {lt, rt}, line);
        bool lT = lt == TYPE_TENSOR, rT = rt == TYPE_TENSOR;
        if (lT && rT)
        {
            TensorOp t = op == PLUS ? TEN_ADD : op == MINUS ? TEN_SUB : op == MULT ? TEN_MUL : TEN_DIV;
            return tensor_op(t, {left, right}, {lt, rt}, line);
        }
        int t = lT ? left : right, k = lT ? right : left;
        VarType kt = lT ? rt : lt;
        if (op == MULT) return tensor_op(TEN_SCALE, {t, k}, {TYPE_TENSOR, kt}, line);
        if (op == DIV)
        {
            int one = alloc_double_slot();
            dmem[one] = 1.0;
            int recip = alloc_double_slot();
            assign(TYPE_DOUBLE, recip, one, OPERATOR_DIV, convert(k, kt, TYPE_DOUBLE, line), line);
            return tensor_op(TEN_SCALE, {t, recip}, {TYPE_TENSOR, TYPE_DOUBLE}, line);
        }
        if (op == MINUS && lT) k = negate(k, kt, line);
        if (op == MINUS && rT) t = negate(t, TYPE_TENSOR, line);
        return tensor_op(TEN_ADD_SCALAR, {t, k}, {TYPE_TENSOR, kt}, line);
    }

    // The tensor and the indexes of t[i, j, ...], one per dimension, as TEN_GET and TEN_SET take them
    void Lowerer::element(uint32_t index, vector<int>& args, vector<VarType>& types)
    {
        for (uint32_t k : tree.children(index))
        {
            args.push_back(expr(k));
            types.push_back(type_of(k));
        }
    }

    // A field of an object or of a struct variable; t.T is the transpose; t.rows, t.cols and t.rank are ints
    int Lowerer::member(uint32_t n)
    {
        uint32_t base = tree.children(n)[0];
        if (kind_of(base) == TypeKind::Class) return field_read(n);
        if (kind_of(base) == TypeKind::Struct) return struct_slots(n)[0];
        int t = expr(base);
        string m = text(n);
        TensorOp op = m == "T" ? TEN_T : m == "rows" ? TEN_ROWS : m == "cols" ? TEN_COLS : TEN_RANK;
        return tensor_op(op, {t}, {TYPE_TENSOR}, line(n));
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // hf and sol
    // ─────────────────────────────────────────────────────────────────────────────

    // hf.method(...): one HF_INFER, given its first two arguments (the model, a dataset's name or the token, then the
    // input or the split) and max_tokens, 100 unless given. A void call's value is a slot of its own that nothing reads.
    int Lowerer::hf_call(uint32_t n, const string& method, const vector<uint32_t>& args)
    {
        HFOp op = method == "infer" ? HF_OP_INFER : method == "generate" ? HF_OP_GENERATE : method == "classify" ? HF_OP_CLASSIFY
                : method == "dataset" ? HF_OP_DATASET : HF_OP_SET_TOKEN;
        int slots[2] = {-1, -1};
        int maxTokens = 100;
        size_t count = 0;
        for (uint32_t a : args)
        {
            if (tree.kind(a) == NodeKind::KeywordArg) maxTokens = stoi(text(tree.children(a)[0]));  // max_tokens=50
            else if (count == 2) maxTokens = stoi(text(a));
            else slots[count++] = expr(a);
        }
        bool isVoid = op == HF_OP_SET_TOKEN;
        int result = isVoid ? -1 : alloc_slot();
        InstructionNode* node = add(HF_INFER, line(n));
        node->hf_inst.op = op;
        node->hf_inst.model_slot = slots[0];
        node->hf_inst.input_slot = slots[1];
        node->hf_inst.result_slot = result;
        node->hf_inst.max_tokens = maxTokens;
        return isVoid ? alloc_slot() : result;
    }

    // sol.method(...): one SOL_CALL. finetune takes name=value arguments (epochs 3, lr 2e-5 and batch_size 16 unless
    // given) and writes the job's handle, which wait, predict and save take first; predict's value is its answer.
    int Lowerer::sol_call(uint32_t n, const string& method, const vector<uint32_t>& args)
    {
        SolOp op = method == "finetune" ? SOL_OP_FINETUNE : method == "wait" ? SOL_OP_WAIT : method == "predict" ? SOL_OP_PREDICT
                 : method == "save" ? SOL_OP_SAVE : SOL_OP_SET_KEY;
        int model = -1, task = -1, dataset = -1, epochs = 3, batchSize = 16;
        float lr = 2e-5f;
        vector<int> slots;
        for (uint32_t a : args)
        {
            if (tree.kind(a) != NodeKind::KeywordArg)
            {
                slots.push_back(expr(a));
                continue;
            }
            string k = text(a);
            uint32_t v = tree.children(a)[0];
            if (k == "epochs") epochs = stoi(text(v));
            else if (k == "batch_size") batchSize = stoi(text(v));
            else if (k == "lr") lr = stof(text(v));
            else (k == "model" ? model : k == "task" ? task : dataset) = expr(v);
        }
        int job = op == SOL_OP_FINETUNE ? alloc_slot() : op == SOL_OP_SET_KEY ? -1 : slots[0];
        int result = op == SOL_OP_PREDICT ? alloc_slot() : -1;
        InstructionNode* node = add(SOL_CALL, line(n));
        node->sol_inst.op = op;
        node->sol_inst.job_slot = job;
        node->sol_inst.model_slot = model;
        node->sol_inst.task_slot = task;
        node->sol_inst.dataset_slot = dataset;
        node->sol_inst.epochs = epochs;
        node->sol_inst.batch_size = batchSize;
        node->sol_inst.lr = lr;
        node->sol_inst.input_slot = op == SOL_OP_PREDICT ? slots[1] : -1;
        node->sol_inst.result_slot = result;
        node->sol_inst.path_slot = op == SOL_OP_SET_KEY ? slots[0] : op == SOL_OP_SAVE ? slots[1] : -1;
        if (op == SOL_OP_FINETUNE) return job;
        return op == SOL_OP_PREDICT ? result : alloc_slot();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Structs
    // ─────────────────────────────────────────────────────────────────────────────

    // A written type (int, Point, vector<int>, Box<int>) as a slot holds it; in a class copy (typeCopy), a type
    // parameter is its type argument. A struct's is TYPE_UNKNOWN: it takes its fields' slots (struct_layout).
    VarType Lowerer::written_type(uint32_t type, uint32_t typeCopy) const
    {
        uint32_t e = in.refs[type];
        if (tree.kind(type) == NodeKind::GenericType && in.entities[e].kind != EntityKind::Class)
        {
            return text(type) == "map" ? TYPE_MAP : TYPE_VECTOR;
        }
        if (e != NoEntity && in.entities[e].kind == EntityKind::TypeParam)  // T in Box<int>: int
        {
            const Instance& c = in.instances[typeCopy];
            vector<uint32_t> params = tree.children(tree.children(c.decl)[0]);
            for (size_t i = 0; i < params.size(); i++)
            {
                if (params[i] == in.entities[e].decl) return var_type(in.types[c.args[i]].kind);
            }
        }
        switch (tree.token(type).kind)
        {
            case INT_TYPE:      return TYPE_INT;
            case FLOAT_TYPE:    return TYPE_FLOAT;
            case DOUBLE_TYPE:   return TYPE_DOUBLE;
            case BOOL_TYPE:     return TYPE_BOOL;
            case STRING_TYPE:   return TYPE_STRING;
            case TENSOR_TYPE:   return TYPE_TENSOR;
            default:            return in.entities[e].kind == EntityKind::Class ? TYPE_CLASS : TYPE_UNKNOWN;
        }
    }

    // A field's type as a slot holds it, in a class copy (fieldCopy) with its type arguments: an int array field's
    // slot holds where its elements start
    VarType Lowerer::field_type(uint32_t field, uint32_t fieldCopy) const
    {
        vector<uint32_t> kids = tree.children(field);  // its type, then an array field's size
        return kids.size() > 1 ? TYPE_INT : written_type(kids[0], fieldCopy);
    }

    // The types of a struct's slots: one per field, in order, a nested struct's fields in its place
    void Lowerer::struct_layout(uint32_t entity, vector<VarType>& types) const
    {
        for (uint32_t f : tree.children(in.entities[entity].decl))
        {
            VarType t = field_type(f);
            if (t == TYPE_UNKNOWN) struct_layout(in.refs[tree.children(f)[0]], types);
            else types.push_back(t);
        }
    }

    // The slots of a struct variable (p), or of a field of one (l.a, l.a.x): its part of the variable's
    vector<int> Lowerer::struct_slots(uint32_t n)
    {
        if (tree.kind(n) == NodeKind::Name) return variables[in.refs[n]].fields;
        uint32_t base = tree.children(n)[0];
        vector<int> all = struct_slots(base);
        size_t at = 0;
        for (uint32_t f : tree.children(in.entities[entity_of(base)].decl))
        {
            vector<VarType> types;
            VarType t = field_type(f);
            if (t == TYPE_UNKNOWN) struct_layout(in.refs[tree.children(f)[0]], types);
            else types.push_back(t);
            if (text(f) == text(n)) return vector<int>(all.begin() + at, all.begin() + at + types.size());
            at += types.size();
        }
        return {};
    }

    // A struct value's field values in struct_layout's order, their types appended to types: a variable's or a
    // field's slots, or a new struct's values, Point(3, 4)
    vector<int> Lowerer::struct_value(uint32_t n, vector<VarType>& types)
    {
        while (tree.kind(n) == NodeKind::Paren) n = tree.children(n)[0];
        if (tree.kind(n) != NodeKind::Call)
        {
            struct_layout(entity_of(n), types);
            return struct_slots(n);
        }
        vector<int> slots;
        vector<uint32_t> kids = tree.children(n);
        for (size_t i = 1; i < kids.size(); i++)
        {
            if (kind_of(kids[i]) == TypeKind::Struct)
            {
                vector<int> inner = struct_value(kids[i], types);
                slots.insert(slots.end(), inner.begin(), inner.end());
                continue;
            }
            slots.push_back(expr(kids[i]));
            types.push_back(type_of(kids[i]));
        }
        return slots;
    }

    // A struct value into the slots dst of a struct variable or field (of struct entity), field by field
    void Lowerer::struct_copy(const vector<int>& dst, uint32_t entity, uint32_t value, int line)
    {
        vector<VarType> to, from;
        struct_layout(entity, to);
        vector<int> src = struct_value(value, from);
        for (size_t i = 0; i < dst.size() && i < src.size(); i++) copy(dst[i], to[i], src[i], from[i], line);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Classes
    // ─────────────────────────────────────────────────────────────────────────────

    // An object type's class: Point, or Box<int> (a copy)
    uint64_t Lowerer::class_key(TypeId type) const
    {
        const Type& t = in.types[type];
        return class_key(t.a, t.b == 0 ? NoInstance : t.b - 1);
    }

    // A class's parent class (never generic: lowering does not cover that), or NoClass
    uint64_t Lowerer::parent_of(uint64_t cls) const
    {
        for (uint32_t m : tree.children(in.entities[class_entity(cls)].decl))
        {
            if (tree.kind(m) == NodeKind::Extends) return class_key(in.refs[tree.children(m)[0]], NoInstance);
        }
        return NoClass;
    }

    // The FunctionDecl a class runs for a method: its own, or the nearest parent's, and in methodCopy the copy of the
    // class it is in; NoNode if none has it
    uint32_t Lowerer::find_method(uint64_t cls, const string& method, uint32_t& methodCopy) const
    {
        for (uint64_t c = cls; c != NoClass; c = parent_of(c))
        {
            for (uint32_t m : tree.children(in.entities[class_entity(c)].decl))
            {
                if (tree.kind(m) != NodeKind::FunctionDecl || text(m) != method) continue;
                methodCopy = class_copy(c);
                return m;
            }
        }
        methodCopy = NoInstance;
        return NoNode;
    }

    // A class's id and fields, the first time it is needed
    const Lowerer::Class& Lowerer::class_of(uint64_t cls)
    {
        auto it = classes.find(cls);
        if (it != classes.end()) return it->second;
        uint64_t up = parent_of(cls);
        vector<Field> fields = up == NoClass ? vector<Field>() : class_of(up).fields;
        for (uint32_t m : tree.children(in.entities[class_entity(cls)].decl))
        {
            if (tree.kind(m) == NodeKind::FieldDecl) fields.push_back({m, class_copy(cls)});
        }
        Class& c = classes[cls];
        c.id = (int)classes.size();
        c.fields = fields;
        return c;
    }

    // A field's index in its class's objects (the first field of that name, a parent's before the class's own), and
    // its FieldDecl and copy in field
    int Lowerer::field_index(uint64_t cls, const string& name, Field& field)
    {
        const vector<Field>& fields = class_of(cls).fields;
        for (size_t i = 0; i < fields.size(); i++)
        {
            if (text(fields[i].decl) != name) continue;
            field = fields[i];
            return (int)i;
        }
        field = Field();
        return 0;
    }

    int Lowerer::method_id(const string& method)
    {
        auto it = methodIds.find(method);
        if (it != methodIds.end()) return it->second;
        int id = (int)methodIds.size() + 1;
        methodIds[method] = id;
        return id;
    }

    // The method a virtual call of each name runs on this class's objects, its own or its parent's (virtualTable)
    void Lowerer::register_class(uint64_t cls)
    {
        int id = class_of(cls).id;
        Class& c = classes[cls];
        if (c.registered) return;
        c.registered = true;
        uint64_t up = parent_of(cls);
        if (up != NoClass)
        {
            register_class(up);
            virtualTable[id] = virtualTable[classes[up].id];
        }
        for (uint32_t m : tree.children(in.entities[class_entity(cls)].decl))
        {
            if (tree.kind(m) != NodeKind::FunctionDecl) continue;
            const Function& f = lower_function(m, class_copy(cls));
            vector<int> params = f.params;
            vector<VarType> types = f.paramTypes;
            params.insert(params.begin(), f.self);
            types.insert(types.begin(), TYPE_CLASS);
            MethodTarget t;
            t.head = f.head;
            t.num_params = (int)params.size();
            t.param_slots = array_of(params);
            t.param_types = array_of(types);
            t.slot_base = f.slotBase;
            t.slot_count = f.slotCount;
            virtualTable[id][method_id(text(m))] = t;
        }
    }

    // Whether n is a temporary object or container: one reference that a store takes, or that release drops once it
    // is used. A new one, a call's result, and one read from a field or a container's element are; a variable, self
    // and none keep their own.
    bool Lowerer::temporary(uint32_t n) const
    {
        while (tree.kind(n) == NodeKind::Paren) n = tree.children(n)[0];
        NodeKind k = tree.kind(n);
        return reference(type_of(n)) && (k == NodeKind::Call || k == NodeKind::Member || k == NodeKind::Index);
    }

    // Drops a temporary object or container (type says which)
    void Lowerer::release(int value, VarType type, int line)
    {
        tensor_call(OBJ_RELEASE, {value}, {type}, -1, line);
    }

    // slot = value: the variable refers to value's object or container (type), taking a temporary's reference
    // (OBJ_MOVE) or counting one of its own (OBJ_STORE); what it referred to before loses that reference
    void Lowerer::object_store(int slot, int value, VarType type, bool temp, int line)
    {
        tensor_call(temp ? OBJ_MOVE : OBJ_STORE, {slot, value}, {TYPE_INT, type}, slot, line);
    }

    // A new object, a temporary, with all fields 0 (an object field none), but an int array field, which gets a
    // block of slots of its own, a Tensor field, -1 (unset: an error if used), and a container field, a new, empty
    // container
    int Lowerer::new_object(uint64_t cls, int line)
    {
        const Class& c = class_of(cls);
        int h = alloc_slot();
        tensor_call(OBJ_NEW, {int_constant((int)c.fields.size()), int_constant(c.id)}, {TYPE_INT, TYPE_INT}, h, line);
        for (size_t i = 0; i < c.fields.size(); i++)
        {
            const Field& f = c.fields[i];
            vector<uint32_t> kids = tree.children(f.decl);  // its type, then an array field's size
            VarType t = field_type(f.decl, f.copy);
            int value;
            if (kids.size() > 1)
            {
                value = alloc_slot();
                add(ALLOC, line)->alloc_inst = {value, int_constant(stoi(text(kids[1])))};
            }
            else if (t == TYPE_TENSOR)
            {
                value = int_constant(-1);
                t = TYPE_INT;
            }
            else if (t == TYPE_VECTOR || t == TYPE_MAP) value = new_container(kids[0], f.copy, line);
            else continue;
            tensor_call(OBJ_SET, {h, int_constant((int)i), value}, {TYPE_INT, TYPE_INT, t}, -1, line);
            if (t == TYPE_VECTOR || t == TYPE_MAP) release(value, t, line);  // the field has its own reference
        }
        return h;
    }

    // Counter c: a new object, its init run if it takes no arguments
    void Lowerer::default_object(uint32_t n, uint32_t entity)
    {
        uint64_t cls = class_key(var_type_id(entity));
        int h = new_object(cls, line(n));
        uint32_t initCopy;
        uint32_t init = find_method(cls, "init", initCopy);
        if (init != NoNode && signature_of(in, init, initCopy)->params.empty()) call_function(n, init, {}, h, initCopy);
        object_store(declare(entity), h, TYPE_CLASS, true, line(n));
    }

    // Counter(args), Box<int>(5): a new object, its init (its own, or the nearest parent's) run with the arguments;
    // a temporary
    int Lowerer::construct(uint32_t n, const vector<uint32_t>& args)
    {
        uint64_t cls = class_key(type_id(n));
        int h = new_object(cls, line(n));
        uint32_t initCopy;
        uint32_t init = find_method(cls, "init", initCopy);
        if (init != NoNode) call_function(n, init, args, h, initCopy);
        return h;
    }

    // obj.m(args): the CALL names the method obj's class (as the check pass typed it) runs, and the runtime runs the
    // object's own class's instead, an override, when it has one. A temporary object is dropped after.
    int Lowerer::call_method(uint32_t n, uint32_t callee, const vector<uint32_t>& args)
    {
        uint32_t object = tree.children(callee)[0];
        int h = expr(object);
        uint32_t methodCopy;
        uint32_t fn = find_method(class_key(type_id(object)), text(callee), methodCopy);
        int result = call_function(n, fn, args, h, methodCopy);
        if (temporary(object)) release(h, TYPE_CLASS, line(n));
        return result;
    }

    // obj.f: OBJ_GET, or OBJ_GET_D for a float or double, which comes back as a double. A Tensor read is one more
    // owner of it, an object or container read a temporary, and an int array field reads as where its elements
    // start.
    int Lowerer::field_read(uint32_t n)
    {
        uint32_t object = tree.children(n)[0];
        int h = expr(object);
        Field field;
        int index = field_index(class_key(type_id(object)), text(n), field);
        VarType t = field_type(field.decl, field.copy);
        bool inDmem = t == TYPE_FLOAT || t == TYPE_DOUBLE;
        int result = inDmem ? alloc_double_slot() : new_slot(t);
        tensor_call(inDmem ? OBJ_GET_D : OBJ_GET, {h, int_constant(index)}, {TYPE_INT, TYPE_INT}, result, line(n));
        if (t == TYPE_FLOAT) result = convert(result, TYPE_DOUBLE, TYPE_FLOAT, line(n));
        if (temporary(object)) release(h, TYPE_CLASS, line(n));
        return result;
    }

    // obj.f = value: OBJ_SET, which counts the field's own reference to a Tensor, an object or a container; a
    // temporary value is then dropped
    void Lowerer::field_write(uint32_t target, int value, VarType type, bool temp, int line)
    {
        uint32_t object = tree.children(target)[0];
        int h = expr(object);
        Field field;
        int index = field_index(class_key(type_id(object)), text(target), field);
        tensor_call(OBJ_SET, {h, int_constant(index), value}, {TYPE_INT, TYPE_INT, type}, -1, line);
        if (temp) release(value, type, line);
        if (temporary(object)) release(h, TYPE_CLASS, line);
    }

    // self.data[i]: ARRAY_READ copies the element into value, ARRAY_WRITE copies value into it. The field holds where
    // the object's elements start; i is checked against the field's size.
    void Lowerer::field_element(InstructionType type, uint32_t field, int index, int value, int line)
    {
        int base = expr(field);
        Field f;
        field_index(class_key(type_id(tree.children(field)[0])), text(field), f);
        InstructionNode* node = add(type, line);
        node->array_inst.base_index = base;
        node->array_inst.dynamic_base = true;
        node->array_inst.index_slot = index;
        node->array_inst.target_index = value;
        node->array_inst.array_size = stoi(text(tree.children(f.decl)[1]));
        node->array_inst.size_slot = -1;
        node->array_inst.line_no = line;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Containers
    // ─────────────────────────────────────────────────────────────────────────────

    // A container op (a TENSOR_CALL) whose result is of type result, or TYPE_UNKNOWN for none. A float or double
    // comes back as a double (a *_D op), and a float is then narrowed; a Tensor is one more owner of it.
    int Lowerer::container_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, VarType result, int line)
    {
        bool inDmem = result == TYPE_FLOAT || result == TYPE_DOUBLE;
        int slot = result == TYPE_UNKNOWN ? -1 : inDmem ? alloc_double_slot() : new_slot(result);
        tensor_call(op, args, types, slot, line);
        if (result == TYPE_FLOAT) return convert(slot, TYPE_DOUBLE, TYPE_FLOAT, line);
        return slot >= 0 ? slot : alloc_slot();
    }

    // vector<T>() or map<K, V>() from its written type (in a class copy, typeCopy, T its type argument): a new,
    // empty container, a temporary. The runtime is told the key's type and the elements'.
    int Lowerer::new_container(uint32_t type, uint32_t typeCopy, int line)
    {
        vector<int> args;
        for (uint32_t t : tree.children(type)) args.push_back(int_constant(written_type(t, typeCopy)));
        TensorOp op = args.size() == 2 ? CON_MAP_NEW : CON_VEC_NEW;
        return container_op(op, args, vector<VarType>(args.size(), TYPE_INT), TYPE_INT, line);
    }

    // v.push(x), v.pop(), m.has(k), ...: the check pass has checked the method and its argument. The container
    // takes its own reference to a temporary object pushed, which is then dropped, and so is the object v.pop()
    // gives when nothing takes it (a statement on its own, which the check pass types as giving nothing).
    int Lowerer::container_call(uint32_t n, uint32_t callee, const vector<uint32_t>& args)
    {
        uint32_t base = tree.children(callee)[0];
        const Type& type = in.types[type_id(base)];
        VarType elem = var_type(in.types[type.kind == TypeKind::Map ? type.b : type.a].kind);
        string m = text(callee);
        bool element = m == "pop" || m == "min" || m == "max" || m == "sum";  // gives an element
        VarType result = element ? elem : type_of(n);
        TensorOp op = CONTAINER_METHODS.at(m);
        if (result == TYPE_FLOAT || result == TYPE_DOUBLE)
        {
            op = op == CON_POP ? CON_POP_D : op == CON_MIN ? CON_MIN_D : op == CON_MAX ? CON_MAX_D : CON_SUM_D;
        }
        vector<int> slots = {expr(base)};
        vector<VarType> types = {TYPE_INT};
        for (uint32_t a : args)
        {
            slots.push_back(expr(a));
            types.push_back(type_of(a));
        }
        int r = container_op(op, slots, types, result, line(n));
        if (!args.empty() && temporary(args[0])) release(slots[1], types[1], line(n));
        if (m == "pop" && elem == TYPE_CLASS && type_of(n) == TYPE_UNKNOWN) release(r, TYPE_CLASS, line(n));
        if (temporary(base)) release(slots[0], type_of(base), line(n));
        return r;
    }

    // v[i], m[k]: an object element is a temporary, and a Tensor element one more owner of it
    int Lowerer::container_get(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        int h = expr(kids[0]);
        int key = expr(kids[1]);
        VarType t = type_of(n);
        TensorOp op = t == TYPE_FLOAT || t == TYPE_DOUBLE ? CON_GET_D : CON_GET;
        int r = container_op(op, {h, key}, {TYPE_INT, type_of(kids[1])}, t, line(n));
        if (temporary(kids[0])) release(h, type_of(kids[0]), line(n));
        return r;
    }

    // v[i] = x (i must be there), m[k] = x (k is added if it is not): the container counts its own reference to an
    // object, so a temporary value is then dropped
    void Lowerer::container_set(uint32_t target, int value, VarType type, bool temp, int line)
    {
        vector<uint32_t> kids = tree.children(target);
        int h = expr(kids[0]);
        int key = expr(kids[1]);
        container_op(CON_SET, {h, key, value}, {TYPE_INT, type_of(kids[1]), type}, TYPE_UNKNOWN, line);
        if (temp) release(value, type, line);
        if (temporary(kids[0])) release(h, type_of(kids[0]), line);
    }
}  // namespace

// Each definition's nodes; a generic function's or class's in each of its copies, with the copy's types
bool lowers(const ParseTree& tree, const CheckResult& checked)
{
    for (uint32_t d : tree.children(tree.root()))
    {
        for (uint32_t c : copies_of(tree, checked, d))
        {
            for (uint32_t n = d + 1 - tree.nodes[d].size; n <= d; n++)
            {
                if (!covered(tree, checked, n, c)) return false;
            }
        }
    }
    return covered(tree, checked, tree.root(), NoInstance);
}

InstructionNode* lower_tree(const ParseTree& tree, const CheckResult& checked)
{
    return Lowerer(tree, checked).run();
}
