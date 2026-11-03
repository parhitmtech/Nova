// The check pass, second step (check.h): the type of every expression, with the rules for numbers, strings,
// tensors, arrays, structs, objects, containers and calls. Names are already resolved (check.cc), so this step reads
// entities through refs and needs no scopes: it can check a function's body whenever a caller first needs its return
// type.
//
// A generic function or class is checked once per copy: scale(2, 3) checks scale's body with x and k ints, and
// Box<int> checks Box's fields and methods with T an int. A value this step does not type (a method's untyped
// parameter, the result of a method without '-> type') is Unknown, and Unknown fits everywhere, so it never causes
// an error.
#include <cstring>
#include <map>
#include <set>
#include "check.h"
#include "compiler.h"
#include "tensor_ops.h"

using namespace std;

extern const map<string, TensorOp> TENSOR_OP_MAP;  // the tensor built-ins, and the kinds of their results
extern const set<TensorOp> TENSOR_VOID_OPS;        // (parser_state.cc)
extern const set<TensorOp> TENSOR_DOUBLE_OPS;
extern const set<TensorOp> TENSOR_INT_OPS;

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

TypeTable::TypeTable()
{
    for (TypeKind k : {TypeKind::Unknown, TypeKind::Int, TypeKind::Float, TypeKind::Double, TypeKind::Bool,
         TypeKind::String, TypeKind::Tensor, TypeKind::Array, TypeKind::None})
    {
        intern({k});
    }
}

// The key packs the kind and both operands (ids below 2^28) into one number.
TypeId TypeTable::intern(Type t)
{
    uint64_t key = (uint64_t)t.kind << 56 | (uint64_t)t.a << 28 | t.b;
    auto it = ids.find(key);
    if (it != ids.end()) return it->second;
    types.push_back(t);
    return ids[key] = (TypeId)types.size() - 1;
}

string type_name(const CheckResult& checked, TypeId id)
{
    const Type& t = checked.types[id];
    switch (t.kind)
    {
        case TypeKind::Int:         return "int";
        case TypeKind::Float:       return "float";
        case TypeKind::Double:      return "double";
        case TypeKind::Bool:        return "bool";
        case TypeKind::String:      return "string";
        case TypeKind::Tensor:      return "Tensor";
        case TypeKind::Array:       return "array";
        case TypeKind::None:        return "none";
        case TypeKind::Vector:      return "vector<" + type_name(checked, t.a) + ">";
        case TypeKind::Map:         return "map<" + type_name(checked, t.a) + ", " + type_name(checked, t.b) + ">";
        case TypeKind::Struct:      return string(names.text(checked.entities[t.a].name));
        case TypeKind::Class:
        {
            string s = string(names.text(checked.entities[t.a].name));
            if (t.b == 0) return s;
            const vector<TypeId>& args = checked.instances[t.b - 1].args;  // a copy: Box<int>
            for (size_t i = 0; i < args.size(); i++) s += (i == 0 ? "<" : ", ") + type_name(checked, args[i]);
            return s + ">";
        }
        default:                    return "unknown";
    }
}

namespace {
    constexpr TypeId Unknown = TypeTable::Unknown, Int = TypeTable::Int, Float = TypeTable::Float,
                     Double = TypeTable::Double, Bool = TypeTable::Bool, String = TypeTable::String,
                     Tensor = TypeTable::Tensor, Array = TypeTable::Array, None = TypeTable::None;

    bool is_number(TypeId t) { return t == Int || t == Float || t == Double || t == Bool; }

    class Typer
    {
        public:
            Typer(const ParseTree& tree, CheckResult& out) : tree(tree), out(out) { out.nodeTypes.assign(tree.nodes.size(), Unknown); }
            void run();

        private:
            const ParseTree& tree;
            CheckResult& out;

            // The function whose body is being checked: its FunctionDecl (NoNode in the ignite block) and its name in
            // messages (f, Linear.forward, scale<int, int>, Box<int>.get), and the copy it belongs to.
            struct Context
            {
                uint32_t function = NoNode;
                string label;
                bool selfCalled = false;  // it called itself before its first return, so its return type was not known yet
                bool method = false;
                uint32_t instance = NoInstance;  // a class copy's type parameters stand for its type arguments
            };
            Context context;
            enum class State : uint8_t { NotChecked, Checking, Done };
            unordered_map<uint64_t, State> states;  // by copy and FunctionDecl (state_key)
            map<pair<uint32_t, vector<TypeId>>, uint32_t> copies;  // by decl and types: the copy's index in instances

            string text(uint32_t n) const { return string(tree.spelling(n)); }
            string name(TypeId t) const { return type_name(out, t); }
            TypeKind kind(TypeId t) const { return out.types[t].kind; }
            TypeId typed(uint32_t n, TypeId t)  // records an expression's type
            {
                out.nodeTypes[n] = t;
                return t;
            }
            void error(uint32_t n, const string& message)
            {
                out.errors.push_back("Error at line " + to_string(tree.token(n).line) + ": " + message);
            }
            bool is_reference(TypeId t) const 
            {
                TypeKind k = kind(t);
                return k == TypeKind::Array || k == TypeKind::None || k == TypeKind::Vector || k == TypeKind::Map ||
                    k == TypeKind::Struct || k == TypeKind::Class;
            }
            bool is_generic_class(uint32_t classEntity) const 
            {
                return tree.kind(tree.children(out.entities[classEntity].decl)[0]) == NodeKind::TypeParams;
            }
            TypeId class_type(uint32_t classEntity)  // self in a generic class is the copy being checked
            {
                if (!is_generic_class(classEntity)) return out.types.intern({TypeKind::Class, classEntity});
                bool inCopy = context.instance != NoInstance && out.instances[context.instance].decl == out.entities[classEntity].decl;
                return inCopy ? out.types.intern({TypeKind::Class, classEntity, context.instance + 1}) : Unknown;
            }
            uint32_t copy_of(TypeId t) const { return kind(t) == TypeKind::Class && out.types[t].b ? out.types[t].b - 1 : NoInstance; }
            static uint64_t state_key(uint32_t instance, uint32_t fn) { return (uint64_t)instance << 32 | fn; }
            bool is_object(TypeId t) const { return kind(t) == TypeKind::Class || t == None; }
            bool is_container(TypeId t) const { return kind(t) == TypeKind::Vector || kind(t) == TypeKind::Map; }
            string a_name(TypeId t) const { return (kind(t) == TypeKind::Class ? "a " : "") + name(t); }  // a P, int

            bool fits(TypeId value, TypeId declared) const;
            uint32_t parent(uint32_t classEntity) const;
            uint32_t find_field(uint32_t entity, const string& field) const;
            uint32_t find_method(uint32_t classEntity, const string& method, uint32_t* owner) const;
            TypeId field_type(uint32_t fieldDecl, uint32_t instance);
            uint32_t binding(TypeId object, uint32_t member) const;
            string path(uint32_t n) const;
            void check_overrides(uint32_t classDecl);
            TypeId written(uint32_t n, bool report);
            TypeId type_argument(uint32_t typeParam) const;
            string spell(uint32_t n) const;
            Signature& signature(uint32_t fn, bool method, uint32_t instance = NoInstance);
            void check_function(uint32_t fn, const string& label, bool method, uint32_t instance = NoInstance);
            uint32_t make_copy(uint32_t decl, const vector<TypeId>& args);
            TypeId class_copy(uint32_t classEntity, const vector<TypeId>& args);
            string copy_name(const string& name, const vector<TypeId>& args) const;
            void check_field(uint32_t n);
            void set_variable(uint32_t declNode, TypeId t);

            void check_block(uint32_t n);
            void check_statement(uint32_t n);
            void check_return(uint32_t n, const vector<uint32_t>& values);
            void check_assign(uint32_t n, TypeId value, TypeId declared);
            void condition(uint32_t n);
            void store(uint32_t target, TypeId value);
            void store_field(uint32_t target, TypeId value);

            TypeId expr(uint32_t n);
            TypeId unary(uint32_t n);
            TypeId binary(uint32_t n, TokenType op, TypeId lt, TypeId rt);
            TypeId tensor_binary(uint32_t n, TokenType op, TypeId lt, TypeId rt);
            void tensor_args(uint32_t n, const string& what, const char* signature, const vector<TypeId>& types,
                             const vector<string>& argNames);
            TypeId call(uint32_t n, bool statement);
            TypeId call_name(uint32_t n, uint32_t callee, const vector<uint32_t>& args);
            TypeId call_function(uint32_t n, uint32_t fn, const string& f, const vector<TypeId>& args);
            TypeId call_builtin(uint32_t n, const string& f, const vector<uint32_t>& args, const vector<TypeId>& types);
            void construct(uint32_t n, TypeId type, const vector<TypeId>& args);
            TypeId call_member(uint32_t n, uint32_t callee, const vector<uint32_t>& args, bool statement);
            TypeId call_module(uint32_t n, const string& module, const string& method, const vector<uint32_t>& args);
            TypeId call_method(uint32_t n, uint32_t callee, TypeId object, const vector<TypeId>& args);
            TypeId call_container(uint32_t n, uint32_t callee, TypeId container, const vector<TypeId>& args, bool statement);
            void check_key(uint32_t n, uint32_t container, TypeId type, TypeId key);
            void check_element(uint32_t n, uint32_t container, TypeId type, TypeId value);
            TypeId index(uint32_t n);
            TypeId member(uint32_t n);
    };
    
    // May a value of type 'value' go where 'declared' is expected (a variable, a field, a parameter, a return value)?
    // The same type, a float widening to a double, or either one Unknown. An object goes where its own class or a
    // parent of it is expected (it has the parent's fields first, and its methods override the parent's), and none
    // goes wherever an object does.
    bool Typer::fits(TypeId value, TypeId declared) const
    {
        if (value == declared || value == Unknown || declared == Unknown) return true;
        if (value == Float && declared == Double) return true;
        if (kind(declared) != TypeKind::Class || !is_object(value)) return false;
        if (value == None) return true;
        if (out.types[declared].b != 0) return false;  // a copy: Box<int> takes only a Box<int>
        uint32_t c = out.types[value].a;
        for (size_t steps = 0; c != NoEntity && steps < out.entities.size(); steps++)  // steps: in case of a cycle
        {
            if (c == out.types[declared].a) return true;
            c = parent(c);
        }
        return false;
    }

    // A class's parent class, or NoEntity
    uint32_t Typer::parent(uint32_t classEntity) const
    {
        for (uint32_t m : tree.children(out.entities[classEntity].decl))
        {
            if (tree.kind(m) == NodeKind::Extends) return out.refs[tree.children(m)[0]];
        }
        return NoEntity;
    }

    // The FieldDecl of a struct's field, or of a class's (its own, or the nearest parent's); NoNode if it has none
    uint32_t Typer::find_field(uint32_t entity, const string& field) const
    {
        for (size_t steps = 0; entity != NoEntity && steps < out.entities.size(); steps++)
        {
            for (uint32_t m : tree.children(out.entities[entity].decl))
            {
                if (tree.kind(m) == NodeKind::FieldDecl && text(m) == field) return m;
            }
            entity = out.entities[entity].kind == EntityKind::Class ? parent(entity) : NoEntity;
        }
        return NoNode;
    }

    // The FunctionDecl a class runs for a method (its own, or the nearest parent's), and in owner the class it is
    // in; NoNode if it has none
    uint32_t Typer::find_method(uint32_t classEntity, const string& method, uint32_t* owner) const
    {
        for (size_t steps = 0; classEntity != NoEntity && steps < out.entities.size(); steps++)
        {
            for (uint32_t m : tree.children(out.entities[classEntity].decl))
            {
                if (tree.kind(m) != NodeKind::FunctionDecl || text(m) != method) continue;
                *owner = classEntity;
                return m;
            }
            classEntity = parent(classEntity);
        }
        return NoNode;
    }

    // A field's type, in a class copy with its type parameters replaced; an array field (int data[256]) is an array
    TypeId Typer::field_type(uint32_t fieldDecl, uint32_t instance)
    {
        vector<uint32_t> kids = tree.children(fieldDecl);
        if (kids.size() > 1) return Array;
        uint32_t outer = context.instance;
        context.instance = instance;
        TypeId t = written(kids[0], false);
        context.instance = outer;
        return t;
    }

    // The copy whose type arguments a field's or method's written types take: the object's own class copy, when
    // the member is in that class (not in a parent)
    uint32_t Typer::binding(TypeId object, uint32_t member) const
    {
        uint32_t decl = out.entities[out.types[object].a].decl;
        bool own = member < decl && member + tree.nodes[decl].size > decl;
        return own ? copy_of(object) : NoInstance;
    }

    // An expression as messages name it: v, self.items, h.p, v[...]
    string Typer::path(uint32_t n) const
    {
        switch (tree.kind(n))
        {
            case NodeKind::Self:   return "self";
            case NodeKind::Member: return path(tree.children(n)[0]) + "." + text(n);
            case NodeKind::Index:  return path(tree.children(n)[0]) + "[...]";
            case NodeKind::Call:   return path(tree.children(n)[0]) + "(...)";
            case NodeKind::Paren:  return path(tree.children(n)[0]);
            default:               return text(n);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Definitions and written types
    // ─────────────────────────────────────────────────────────────────────────────

    // File order. A function may already have been checked, when an earlier caller needed its return type.
    void Typer::run()
    {
        for (uint32_t n : tree.children(tree.root()))
        {
            switch (tree.kind(n))
            {
                case NodeKind::FunctionDecl:
                {
                    check_function(n, text(n), false);
                    break;
                }
                case NodeKind::StructDecl:
                {
                    for (uint32_t f : tree.children(n))
                    {
                        if (tree.kind(f) == NodeKind::FieldDecl) check_field(f);
                    }
                    break;
                }
                case NodeKind::ClassDecl:
                {
                    vector<uint32_t> members = tree.children(n);
                    if (tree.kind(members[0]) == NodeKind::TypeParams) break;  // each copy is checked when it is made
                    check_overrides(n);
                    for (uint32_t m : members)
                    {
                        if (tree.kind(m) == NodeKind::FieldDecl) check_field(m);
                        else if (tree.kind(m) == NodeKind::FunctionDecl) check_function(m, text(n) + "." + text(m), true);
                    }
                    break;
                }
                case NodeKind::Ignite:
                {
                    Context outer = context;
                    context = Context();
                    check_block(tree.children(n)[0]);
                    context = outer;
                    break;
                }
                default: break;
            }
        }
    }

    // The type a written type names: int, Tensor<float>, vector<int>, map<string, int>, Point. With report, also the 
    // rules for writing one: how many type arguments, what an element or a map key can be, what a type argument can
    // be. Box<int> is Box's copy for int (made the first time it is written), and T in a class copy its type argument.
    TypeId Typer::written(uint32_t n, bool report)
    {
        switch (tree.token(n).kind)
        {
            case INT_TYPE:      return Int;
            case FLOAT_TYPE:    return Float;
            case DOUBLE_TYPE:   return Double;
            case BOOL_TYPE:     return Bool;
            case STRING_TYPE:   return String;
            case TENSOR_TYPE:   return Tensor;  // Tensor<float> too: the element type is checked by the lexer
            default:            break;
        }
        uint32_t e = out.refs[n];
        if (e == NoEntity) return Unknown;  // reported with the names
        const Entity& ent = out.entities[e];
        vector<uint32_t> args = tree.children(n);
        if (ent.kind == EntityKind::Struct) return out.types.intern({TypeKind::Struct, e});
        if (ent.kind == EntityKind::Class && !is_generic_class(e)) return out.types.intern({TypeKind::Class, e});
        if (ent.kind == EntityKind::Class)  // Box<int>: one type argument per type parameter, each a simple type
        {
            size_t want = tree.children(tree.children(ent.decl)[0]).size();
            if (args.empty()) return Unknown;  // reported with the names: Box needs its type arguments
            if (args.size() != want)
            {
                if (report)
                {
                    error(n, "class '" + text(n) + "' takes " + to_string(want) + " type argument(s) but got " +
                             to_string(args.size()));
                }
                return Unknown;
            }
            vector<TypeId> types;
            bool ok = true;
            for (uint32_t a : args)
            {
                bool nested = false;  // a container of containers, or of a generic class's copy
                for (uint32_t inner : tree.children(a)) nested |= tree.kind(inner) == NodeKind::GenericType;
                uint32_t ae = out.refs[a];
                bool isStruct = ae != NoEntity && out.entities[ae].kind == EntityKind::Struct;
                bool isGenericClass = ae != NoEntity && out.entities[ae].kind == EntityKind::Class && is_generic_class(ae);
                if ((nested || isStruct || isGenericClass) && report)
                {
                    error(n, "'" + spell(a) + "' cannot be a type argument: use int, float, double, bool, string, Tensor, "
                             "a class, vector<T> or map<K, V>");
                }
                types.push_back(written(a, report));
                ok &= !nested && !isStruct && !isGenericClass && types.back() != Unknown;  // Unknown: reported already
            }
            return ok ? class_copy(e, types) : Unknown;
        }
        if (ent.kind == EntityKind::TypeParam) return type_argument(e);
        if (ent.kind != EntityKind::BuiltinType) return Unknown;

        // vector<T>, map<K, V>
        bool isMap = text(n) == "map";
        if (args.size() != (isMap ? 2u : 1u))
        {
            if (report)
            {
                error(n, string(isMap ? "map takes 2 type arguments (map<K, V>)" : "vector takes 1 type argument (vector<T>)") + 
                         " but got " + to_string(args.size()));
            }
            return Unknown;
        }
        TypeId elem = written(args.back(), report);
        TypeKind ek = kind(elem);
        bool elemOk = elem == Unknown || is_number(elem) || elem == String || elem == Tensor || ek == TypeKind::Class;
        if (!elemOk || tree.kind(args.back()) == NodeKind::GenericType)
        {
            if (report)
            {
                error(n, "'" + spell(args.back()) + "' cannot be an element type: use int, float, double, bool, string, "
                         "Tensor or a class");
            }
            return Unknown;
        }
        if (!isMap) return out.types.intern({TypeKind::Vector, elem});
        TypeId key = written(args[0], report);
        if (key != Int && key != String && key != Unknown)
        {
            if (report) error(n, "a map key must be int or string, not '" + spell(args[0]) + "'");
            return Unknown;
        }
        return out.types.intern({TypeKind::Map, key, elem});
    }

    // What a type parameter stands for in the class copy being checked: T in Box<int> is int
    TypeId Typer::type_argument(uint32_t typeParam) const
    {
        if (context.instance == NoInstance) return Unknown;
        const Instance& copy = out.instances[context.instance];
        if (tree.kind(copy.decl) != NodeKind::ClassDecl) return Unknown;
        vector<uint32_t> params = tree.children(tree.children(copy.decl)[0]);
        for (size_t i = 0; i < params.size(); i++)
        {
            if (params[i] == out.entities[typeParam].decl) return copy.args[i];
        }
        return Unknown;
    }

    // A written type as the source spells it, for messages: vector<vector<int>>
    string Typer::spell(uint32_t n) const 
    {
        string s = text(n);
        vector<uint32_t> args = tree.children(n);
        for (size_t i = 0; i < args.size(); i++) s += (i == 0 ? "<" : ", ") + spell(args[i]);
        return args.empty() ? s : s + ">";
    }

    // A function's parameter and return types, as written, in a copy with its own types. Only a function (not a
    // method) with an untyped parameter is generic, and each copy gives that parameter its argument's type; a method's
    // untyped parameter is Unknown. A method without '-> types' returns Unknown: only a function's return type is
    // taken from its first return.
    Signature& Typer::signature(uint32_t fn, bool method, uint32_t instance)
    {
        unordered_map<uint32_t, Signature>& table = instance == NoInstance ? out.signatures : out.instances[instance].signatures;
        auto it = table.find(fn);
        if (it != table.end()) return it->second;
        bool functionCopy = instance != NoInstance && out.instances[instance].decl == fn;
        uint32_t outer = context.instance;
        context.instance = instance;
        Signature s;
        vector<uint32_t> kids = tree.children(fn);  // ParamList, ReturnTypes if written, Block
        for (uint32_t p : tree.children(kids[0]))
        {
            if (tree.token(p).kind == SELF) continue;
            vector<uint32_t> type = tree.children(p);
            size_t i = s.params.size();
            if (!type.empty()) s.params.push_back(written(type[0], false));
            else if (functionCopy) s.params.push_back(out.instances[instance].args[i]);
            else
            {
                s.generic |= !method;
                s.params.push_back(Unknown);
            }
        }
        s.inferred = kids.size() == 2;
        if (!s.inferred)
        {
            for (uint32_t t : tree.children(kids[1])) s.returns.push_back(written(t, false));
        }
        context.instance = outer;
        return table[fn] = s;
    }

    // A function's body, once: in file order, or earlier when a caller first needs its return type. A generic
    // function's body is checked once per copy, when a call first needs it, and a generic class's methods once per
    // class copy. A copy's locals start untyped, and the types another copy of the same function gave them (it may
    // still be being checked: a copy can call another) come back after.
    void Typer::check_function(uint32_t fn, const string& label, bool method, uint32_t instance)
    {
        uint64_t key = state_key(instance, fn);
        if (states[key] != State::NotChecked) return;
        states[key] = State::Checking;
        Signature sig = signature(fn, method, instance);  // a copy: the table may grow
        if (sig.generic)
        {
            states[key] = State::Done;
            return;
        }
        vector<pair<uint32_t, TypeId>> saved;
        if (instance != NoInstance)
        {
            uint32_t first = fn + 1 - tree.nodes[fn].size;
            for (uint32_t e = 0; e < out.entities.size(); e++)
            {
                Entity& var = out.entities[e];
                if (var.kind != EntityKind::Variable || var.decl < first || var.decl >= fn) continue;
                saved.push_back({e, var.type});
                var.type = Unknown;
            }
        }
        Context outer = context;
        context = Context();
        context.function = fn;
        context.label = label;
        context.method = method;
        context.instance = instance;
        vector<uint32_t> kids = tree.children(fn);
        size_t i = 0;
        for (uint32_t p : tree.children(kids[0]))
        {
            if (tree.token(p).kind == SELF) continue;
            vector<uint32_t> type = tree.children(p);
            if (!type.empty()) written(type[0], true);
            set_variable(p, sig.params[i++]);
        }
        if (kids.size() == 3)
        {
            for (uint32_t t : tree.children(kids[1])) written(t, true);
        }
        check_block(kids.back());
        for (const auto& [e, t] : saved) out.entities[e].type = t;
        context = outer;
        states[key] = State::Done;
    }

    // The copy of a generic function or class for these types, made the first time they are used; its index in
    // instances
    uint32_t Typer::make_copy(uint32_t decl, const vector<TypeId>& args)
    {
        auto it = copies.find({decl, args});
        if (it != copies.end()) return it->second;
        out.instances.push_back({decl, args, {}});
        return copies[{decl, args}] = (uint32_t)out.instances.size() - 1;
    }

    // Box<int>: the class's copy for these type arguments. A new copy's fields and methods are checked with its types
    // right away, as the old parser compiles each copy the first time it is written.
    TypeId Typer::class_copy(uint32_t classEntity, const vector<TypeId>& args)
    {
        uint32_t decl = out.entities[classEntity].decl;
        bool isNew = !copies.count({decl, args});
        uint32_t instance = make_copy(decl, args);
        TypeId t = out.types.intern({TypeKind::Class, classEntity, instance + 1});
        if (!isNew) return t;
        Context outer = context;
        context = Context();
        context.instance = instance;
        for (uint32_t m : tree.children(decl))
        {
            if (tree.kind(m) == NodeKind::FieldDecl) check_field(m);
            else if (tree.kind(m) == NodeKind::FunctionDecl) check_function(m, name(t) + "." + text(m), true, instance);
        }
        context = outer;
        return t;
    }

    // A copy's name in messages: scale<int, double>
    string Typer::copy_name(const string& f, const vector<TypeId>& args) const
    {
        string s = f;
        for (size_t i = 0; i < args.size(); i++) s += (i == 0 ? "<" : ", ") + name(args[i]);
        return s + ">";
    }

    // An override takes and returns what the method it overrides does, so a call through the parent's type can run
    // it (init is not called that way)
    void Typer::check_overrides(uint32_t classDecl)
    {
        uint32_t up = parent(out.refs[classDecl]);
        if (up == NoEntity || is_generic_class(up)) return;
        for (uint32_t m : tree.children(classDecl))
        {
            if (tree.kind(m) != NodeKind::FunctionDecl || text(m) == "init") continue;
            uint32_t owner = NoEntity;
            uint32_t over = find_method(up, text(m), &owner);
            if (over == NoNode) continue;
            Signature mine = signature(m, true), theirs = signature(over, true);  // copies: the table may grow
            if (mine.params != theirs.params || mine.returns != theirs.returns)
            {
                error(m, "'" + text(classDecl) + "." + text(m) + "' must take and return the same types as '" +
                         string(names.text(out.entities[owner].name)) + "." + text(m) + "', which it overrides");
            }
        }
    }

    // A field's type, and an array field's size
    void Typer::check_field(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        written(kids[0], true);
        if (kids.size() > 1) expr(kids[1]);
    }

    void Typer::set_variable(uint32_t declNode, TypeId t)
    {
        uint32_t e = out.refs[declNode];
        if (e != NoEntity) out.entities[e].type = t;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Statements
    // ─────────────────────────────────────────────────────────────────────────────

    void Typer::check_block(uint32_t n)
    {
        for (uint32_t s : tree.children(n)) check_statement(s);
    }

    void Typer::check_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        switch (tree.kind(n))
        {
            case NodeKind::VarDecl:
            {
                TypeId declared = written(kids[0], true);
                if (kids.size() > 1) check_assign(n, expr(kids[1]), declared);
                else if (declared == String)
                {
                    error(n, "string '" + text(n) + "' needs an initial value, e.g. string " + text(n) + " = \"\"");
                }
                set_variable(n, declared);
                return;
            }
            case NodeKind::MultiDecl:  // Tensor a, Tensor b = halves(t): one value for each, of its type
            {
                uint32_t value = kids.back();
                expr(value);
                vector<TypeId> vars;
                for (size_t i = 0; i + 1 < kids.size(); i++)
                {
                    vars.push_back(written(tree.children(kids[i])[0], true));
                    set_variable(kids[i], vars.back());
                }
                vector<uint32_t> call = tree.kind(value) == NodeKind::Call ? tree.children(value) : vector<uint32_t>();
                uint32_t callee = call.empty() ? NoNode : call[0];
                uint32_t e = callee != NoNode && tree.kind(callee) == NodeKind::Name ? out.refs[callee] : NoEntity;
                if (e == NoEntity || out.entities[e].kind != EntityKind::Function) return;
                const Signature& sig = out.signatures[out.entities[e].decl];
                string f = text(callee);
                if (sig.generic)
                {
                    error(callee, "'" + f + "' has untyped parameters, so it cannot return several values: give its "
                                  "parameters types");
                }  
                else if (sig.returns.size() != vars.size())
                {
                    error(callee, "function '" + f + "' returns " + to_string(sig.returns.size()) + " value(s) but " + 
                                  to_string(vars.size()) + " variables given");
                }
                else
                {
                    for (size_t i = 0; i < vars.size(); i++)
                    {
                        if (sig.returns[i] != Unknown && vars[i] != Unknown && sig.returns[i] != vars[i])
                        {
                            error(callee, "return value " + to_string(i + 1) + " of '" + f + "': expected " + 
                                          name(sig.returns[i]) + " but got " + name(vars[i]));
                        }
                    }
                }
                return;
            } 
            case NodeKind::Assign:
            {
                store(kids[0], expr(kids[1]));
                return;
            }
            case NodeKind::CompoundAssign:  // x += e is x = x + e, with x read once
            {
                TokenType op = tree.token(n).kind == PLUSEQ ? PLUS :
                               tree.token(n).kind == MINUSEQ ? MINUS :
                               tree.token(n).kind == MULTEQ ? MULT : DIV;
                TypeId target = expr(kids[0]);
                TypeId result = binary(n, op, target, expr(kids[1]));
                if (tree.kind(kids[0]) == NodeKind::Name) store(kids[0], result);
                return;
            }
            case NodeKind::ExprStatement:  // a call as a statement: v.push(x) is a statement, v.size() a value
            {
                if (tree.kind(kids[0]) == NodeKind::Call) typed(kids[0], call(kids[0], true));
                else expr(kids[0]);
                return;
            }
            case NodeKind::If:
            {
                condition(kids[0]);
                check_block(kids[1]);
                for (size_t i = 2; i < kids.size(); i++)
                {
                    vector<uint32_t> part = tree.children(kids[i]);  // Elif: condition, Block; Else: Block
                    if (part.size() == 2) condition(part[0]);
                    check_block(part.back());
                }
                return;
            }
            case NodeKind::While:
            {
                condition(kids[0]);
                check_block(kids[1]);
                return;
            }
            case NodeKind::DoWhile:
            {
                check_block(kids[0]);
                condition(kids[1]);
                return;
            }
            case NodeKind::For:
            {
                if (tree.kind(kids[0]) != NodeKind::Empty) check_statement(kids[0]);
                if (tree.kind(kids[1]) != NodeKind::Empty) condition(kids[1]);
                if (tree.kind(kids[2]) != NodeKind::Empty) check_statement(kids[2]);
                check_block(kids[3]);
                return;                
            }
            case NodeKind::Switch:
            {
                expr(kids[0]);
                for (size_t i = 1; i < kids.size(); i++)
                {
                    vector<uint32_t> part = tree.children(kids[i]);  // Case: value, Block; Default: Block
                    if (part.size() == 2) expr(part[0]);
                    check_block(part.back());
                }
                return;
            }
            case NodeKind::Return:
            {
                check_return(n, kids);
                return;
            }
            default:  // Break, Continue, Pass
            {
                return;
            }
        }
    }

    // Each value must fit the function's return type. A function without '-> types' takes its first return's type.
    void Typer::check_return(uint32_t n, const vector<uint32_t>& values)
    {
        vector<TypeId> types;
        for (uint32_t v : values) types.push_back(expr(v));
        if (context.function == NoNode || values.empty()) return;  // outside a function (reported with the names)
        Signature& sig = signature(context.function, context.method, context.instance);
        const string& f = context.label;
        if (context.method && sig.inferred) return;  // a method without '-> types' returns Unknown
        if (sig.inferred && sig.returns.empty())
        {
            if (types.size() > 1)
            {
                error(n, "function '" + f + "' returns several values: write their types after '->'");
            }
            else if (context.selfCalled && (types[0] == Float || types[0] == Double || types[0] == Tensor || types[0] == String))
            {
                error(n, "function '" + f + "' calls itself before its first return, so its return type must be written: "
                         "add '-> " + name(types[0]) + "'");
            }
            sig.returns = {types[0]};
        }
        if (types.size() != sig.returns.size())
        {
            error(n, "function '" + f + "' returns " + to_string(sig.returns.size()) + " value(s) but this return gives " + 
                     to_string(types.size()));
            return;
        }
        for (size_t i = 0; i < types.size(); i++)
        {
            TypeId want = sig.returns[i], got = types[i];
            if (fits(got, want)) continue;
            if ((kind(want) == TypeKind::Class && kind(got) == TypeKind::Class) || (is_container(want) && kind(got) == kind(want)))
            {
                error(n, "'" + f + "' returns " + name(want) + ", not " + name(got));  // another class, or element type
            }
            else
            {
                error(n, "'" + f + "' returns " + name(got) + " but is declared '-> " + name(want) + "'");
            }
        }
    }

    // A value stored in the variable named by node n's token, of type 'declared'. An object variable takes only an
    // object (Box b = 5 does not make a Box), of its class or a subclass; a container variable only a container of
    // its own type.
    void Typer::check_assign(uint32_t n, TypeId value, TypeId declared)
    {
        if (fits(value, declared)) return;
        if (kind(declared) == TypeKind::Class && !is_object(value))
        {
            error(n, "a " + name(declared) + " is created with " + name(declared) + "(...), which runs its init");
        }
        else if (kind(declared) == TypeKind::Class || is_container(declared))
        {
            error(n, "'" + text(n) + "' is a " + name(declared) + ", not " + a_name(value));
        }
        else
        {
            error(n, "cannot assign " + name(value) + " to " + name(declared) + " variable '" + text(n) + "'");
        }
    }

    // Any number (or tensor, or object) can be a condition, as in C; a string cannot.
    void Typer::condition(uint32_t n)
    {
        if (expr(n) == String) error(n, "a string is not a condition; compare it instead, e.g. (s == \"yes\")");
    }

    // A value stored into a variable (the first known value fixes an implicitly declared variable's type, and an
    // object fixes one that was none), a field, a container element, a tensor element or an array element.
    void Typer::store(uint32_t target, TypeId value)
    {
        if (tree.kind(target) == NodeKind::Name)
        {
            uint32_t e = out.refs[target];
            if (e == NoEntity || out.entities[e].kind != EntityKind::Variable) return;
            Entity& var = out.entities[e];
            if (var.type == Unknown || (var.type == None && is_object(value)))
            {
                var.type = value;  // x = 5 declares x as an int
            }
            else
            {
                check_assign(target, value, var.type);
            }
            typed(target, var.type);
            return;
        }
        if (tree.kind(target) == NodeKind::Member)
        {
            store_field(target, value);
            return;
        }
        if (tree.kind(target) == NodeKind::Index)
        {
            vector<uint32_t> kids = tree.children(target);
            vector<TypeId> types;
            for (uint32_t k : kids) types.push_back(expr(k));
            if (types[0] == Tensor)
            {
                types.push_back(value);
                tensor_args(target, "element assignment", tensor_op_signature(TEN_SET), types, {});
            }
            else if (is_container(types[0]) && types.size() == 2)
            {
                check_key(target, kids[0], types[0], types[1]);
                check_element(target, kids[0], types[0], value);
            }
            return;
        }
        expr(target);
    }

    // obj.x = value: the field takes values as a variable of its type does, and an array field only element by
    // element. A struct's fields are checked the same way.
    void Typer::store_field(uint32_t target, TypeId value)
    {
        TypeId object = expr(tree.children(target)[0]);
        bool isClass = kind(object) == TypeKind::Class;
        if (!isClass && kind(object) != TypeKind::Struct) return;
        string field = text(target), owner = string(isClass ? "class '" : "struct '") + name(object) + "'";
        uint32_t f = find_field(out.types[object].a, field);
        if (f == NoNode)
        {
            error(target, owner + " has no field '" + field + "'");
            return;
        }
        TypeId want = typed(target, field_type(f, binding(object, f)));
        if (want == Array)
        {
            error(target, "'" + field + "' is an array field: assign its elements, " + field + "[i] = x");
        }
        else if (fits(value, want))
        {
            return;
        }
        else if (is_object(want) || is_object(value) || (is_container(want) && is_container(value)))
        {
            error(target, "'" + path(target) + "' is " + (is_container(want) ? "a " : "") + a_name(want) + ", not " +
                          a_name(value));
        }
        else
        {
            error(target, "cannot assign " + name(value) + " to " + name(want) + " field '" + field + "' of " + owner);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Expressions
    // ─────────────────────────────────────────────────────────────────────────────

    TypeId Typer::expr(uint32_t n)
    {
        switch (tree.kind(n))
        {
            case NodeKind::IntLiteral:          return typed(n, Int);
            case NodeKind::FloatLiteral:        return typed(n, Float);
            case NodeKind::StringLiteral:       return typed(n, String);
            case NodeKind::BoolLiteral:         return typed(n, Bool);
            case NodeKind::Name:
            {
                uint32_t e = out.refs[n];
                if (e == NoEntity) return Unknown;
                if (out.entities[e].kind == EntityKind::Variable) return typed(n, out.entities[e].type);
                return typed(n, out.entities[e].kind == EntityKind::None ? None : Unknown);
            }
            case NodeKind::Self:        return typed(n, out.refs[n] == NoEntity ? Unknown : class_type(out.refs[n]));
            case NodeKind::Paren:       return typed(n, expr(tree.children(n)[0]));
            case NodeKind::Unary:       return typed(n, unary(n));
            case NodeKind::Binary:
            {
                vector<uint32_t> kids = tree.children(n);
                TypeId lt = expr(kids[0]);
                return typed(n, binary(n, tree.token(n).kind, lt, expr(kids[1])));
            }
            case NodeKind::Call:        return typed(n, call(n, false));
            case NodeKind::Index:       return typed(n, index(n));
            case NodeKind::Member:      return typed(n, member(n));
            case NodeKind::KeywordArg:
            {
                error(n, "a keyword argument (" + text(n) + "=...) is allowed only in an hf.* or sol.* call");
                return expr(tree.children(n)[0]);
            }
            default:                    return Unknown;
        }
    }

    TypeId Typer::unary(uint32_t n)
    {
        TypeId t = expr(tree.children(n)[0]);
        if (tree.token(n).kind == NOT)
        {
            if (t == String) error(n, "a string is not a condition; compare it instead, e.g. (s == \"yes\")");
            return Bool;
        }
        if (t == Unknown || t == Int || t == Float || t == Double || t == Tensor) return t;  // -x
        error(n, "unary '-' needs a number, got " + name(t));
        return Unknown;
    }

    // left OP right. Comparisons and and / or give a bool. Numbers: int with int gives int, float or double with
    // float or double gives double if either is; int and float do not mix (convert with float(x) / int(x)). A 
    // string on the left of + concatenates. A Tensor on either side, or @, is a tensor operation.
    TypeId Typer::binary(uint32_t n, TokenType op, TypeId lt, TypeId rt)
    {
        switch (op)
        {
            case AND:
            case OR:
            {
                if (lt == String || rt == String) error(n, "a string is not a condition; compare it instead, e.g. (s == \"yes\")");
                return Bool;
            }
            case EQEQ: case NOTEQUAL: case LESS: case GREATER: case LESSEQ: case GREATEREQ:
            {
                if (is_object(lt) || is_object(rt))  // p == q, n.next != none: the same object or not (no order)
                {
                    if (op != EQEQ && op != NOTEQUAL)
                    {
                        error(n, "objects are compared with == and != only");
                    }
                    else if (lt != Unknown && rt != Unknown && is_object(lt) != is_object(rt))
                    {
                        error(n, "cannot compare " + name(lt) + " with " + name(rt));
                    }
                    else if (!fits(lt, rt) && !fits(rt, lt))
                    {
                        error(n, "cannot compare a " + name(lt) + " with a " + name(rt));
                    }
                    return Bool;
                }
                if ((lt == String) != (rt == String) && lt != Unknown && rt != Unknown && !is_reference(lt) && !is_reference(rt))
                {
                    error(n, "cannot compare " + name(lt) + " with " + name(rt));
                }
                return Bool;
            }
            default: break;
        }
        if (op == AT || lt == Tensor || rt == Tensor) return tensor_binary(n, op, lt, rt);
        if (lt == Unknown || rt == Unknown)  // what is known about the result
        {
            TypeId other = lt == Unknown ? rt : lt;
            if (other == Double || other == Float) return other;
            return (other == String && op == PLUS) ? String : Unknown;
        }
        if (op == MOD)
        {
            if (lt != Int || rt != Int) error(n, "'%' needs int operands, got " + name(lt) + " and " + name(rt));
            return Int;
        }
        if (op == PLUS && lt == String)
        {
            if (rt != String) error(n, "cannot concatenate string with " + name(rt));
            return String;
        }
        if (lt == Float || lt == Double)
        {
            if (rt != Float && rt != Double) error(n, "arithmetic requires float/double operands, got " + name(rt));
            return (lt == Double || rt == Double) ? Double : Float;
        }
        if (lt != Int) error(n, "arithmetic requires int operands, got " + name(lt));
        if (rt != Int) error(n, "arithmetic requires int operands, got " + name(rt));
        return Int;
    } 

    // Two tensors: + - * / element by element (+ and - also add a 1xN row to every row), @ is matmul. A tensor and a 
    // number: * scales (either order), / divides (the number on the right), + and - shift every element.
    TypeId Typer::tensor_binary(uint32_t n, TokenType op, TypeId lt, TypeId rt)
    {
        const char* symbol = op == PLUS ? "+" : op == MINUS ? "-" : op == MULT ? "*" : op == DIV ? "/" : op == MOD ? "%" : "@";
        string what = string("'") + symbol + "'";
        if (op == AT)
        {
            tensor_args(n, what, tensor_op_signature(TEN_MATMUL), {lt, rt}, {});
            return Tensor;
        }
        bool lT = lt == Tensor, rT = rt == Tensor;
        if (lT && rT && op != MOD) return Tensor;
        if (lt == Unknown || rt == Unknown) return Unknown;
        if (op == MULT || op == PLUS || op == MINUS)
        {
            tensor_args(n, what, tensor_op_signature(TEN_SCALE), {Tensor, lT ? rt : lt}, {});
            return Tensor;
        }
        if (op == DIV && lT)
        {
            if (!is_number(rt)) error(n, "arithmetic requires float/double operands, got " + name(rt));
            return Tensor;
        }
        error(n, what + " is not defined between " + name(lt) + " and " + name(rt));
        return Unknown;
    }

    // The arguments of a tensor built-in, a tensor operator or a tensor index, against its signature (tensor_ops.h): one
    // letter per argument, T a Tensor, n a number, s a string; a final * repeats the letter before it. argNames names
    // the arguments that are plain  variables, for the message.
    void Typer::tensor_args(uint32_t n, const string& what, const char* signature, const vector<TypeId>& types,
                            const vector<string>& argNames)
    {
        size_t len = strlen(signature);
        bool repeat = len > 0 && signature[len - 1] == '*';
        size_t fixed = repeat ? len - 1 : len;
        for (size_t i = 0; i < types.size() && i < 8; i++)
        {
            char want = i < fixed ? signature[i] : repeat ? signature[fixed - 1] : 0;
            TypeId t = types[i];
            if (want == 0 || t == Unknown) continue;
            if ((want == 'T' && t == Tensor) || (want == 'n' && is_number(t)) || (want == 's' && t == String)) continue;
            string got = i < argNames.size() && !argNames[i].empty() ? "but '" + argNames[i] + "' is " + name(t)
                                                                     : "got " + name(t);
            error(n, "argument " + to_string(i + 1) + " of " + what + " must be " + 
                     (want == 'T' ? "a Tensor" : want == 'n' ? "a number" : "a string") + ", " + got);
        }
    }

    // statement: the call is a statement on its own (v.push(x)), not a value
    TypeId Typer::call(uint32_t n, bool statement)
    {
        vector<uint32_t> kids = tree.children(n);
        uint32_t callee = kids[0];
        vector<uint32_t> args(kids.begin() + 1, kids.end());
        switch (tree.kind(callee))
        {
            case NodeKind::Name:        return call_name(n, callee, args);
            case NodeKind::Member:      return call_member(n, callee, args, statement);
            case NodeKind::TypeName:        // float(x), int(x), double(x): a conversion
            case NodeKind::GenericType:     // vector<int>(), map<string, int>(), Box<int>(5)
            {
                TypeId t = written(callee, true);
                vector<TypeId> types;
                for (uint32_t a : args) types.push_back(expr(a));
                switch (tree.token(callee).kind)
                {
                    case INT_TYPE:          return Int;
                    case FLOAT_TYPE:        return Float;
                    case DOUBLE_TYPE:       return Double;
                    default:
                    {
                        if (kind(t) == TypeKind::Class) construct(n, t, types);
                        return is_container(t) || kind(t) == TypeKind::Class ? t : Unknown;
                    }
                }
            }
            default:
            {
                expr(callee);
                for (uint32_t a : args) expr(a);
                return Unknown;
            }
        }
    }

    TypeId Typer::call_name(uint32_t n, uint32_t callee, const vector<uint32_t>& args)
    {
        vector<TypeId> types;
        for (uint32_t a : args) types.push_back(expr(a));
        uint32_t e = out.refs[callee];
        if (e == NoEntity) return Unknown;
        const Entity& ent = out.entities[e];
        switch (ent.kind)
        {
            case EntityKind::Function:      return call_function(n, ent.decl, text(callee), types);
            case EntityKind::Builtin:       return call_builtin(n, text(callee), args, types);
            case EntityKind::Struct:        // Point(1, 2): one value per field, in order
            {
                size_t fields = 0;
                for (uint32_t f : tree.children(ent.decl)) fields += tree.kind(f) == NodeKind::FieldDecl;
                if (args.size() != fields)
                {
                    error(n, text(callee) + " has " + to_string(fields) + " field(s); give one value for each");
                }
                return out.types.intern({TypeKind::Struct, e});
            }
            case EntityKind::Class:
            {
                if (is_generic_class(e)) return Unknown;  // reported with the names: Box needs its type arguments
                TypeId t = class_type(e);
                construct(n, t, types);
                return t;
            }
            default:                        return Unknown;
        }
    }

    // P(args), Box<int>(args): a new object, its init (its own, or the nearest parent's) run with the arguments
    void Typer::construct(uint32_t n, TypeId type, const vector<TypeId>& args)
    {
        string cls = name(type);
        uint32_t owner = NoEntity;
        uint32_t init = find_method(out.types[type].a, "init", &owner);
        if (init == NoNode)
        {
            error(n, "class '" + cls + "' has no constructor 'init'");
            return;
        }
        Signature sig = signature(init, true, binding(type, init));
        if (args.size() != sig.params.size())
        {
            error(n, "'" + cls + "' constructor takes " + to_string(sig.params.size()) + " argument(s) but got " +
                     to_string(args.size()));
        }
        for (size_t i = 0; i < args.size() && i < sig.params.size(); i++)
        {
            if (!fits(args[i], sig.params[i]))
            {
                error(n, "argument " + to_string(i + 1) + " of '" + cls + "' constructor expects " + name(sig.params[i]) +
                         " but got " + name(args[i]));
            }
        }
    }

    // f(args). A generic function's call uses the copy for its argument types, made and checked the first time.
    TypeId Typer::call_function(uint32_t n, uint32_t fn, const string& f, const vector<TypeId>& args)
    {
        Signature sig = signature(fn, false);  // a copy: checking the callee below adds signatures
        if (args.size() != sig.params.size())
        {
            error(n, "function '" + f + "' expects " + to_string(sig.params.size()) + " argument(s) but got " +
                     to_string(args.size()));
            return Unknown;
        }
        uint32_t instance = NoInstance;
        string label = f;
        if (sig.generic)
        {
            instance = make_copy(fn, args);
            label = copy_name(f, args);
            sig = signature(fn, false, instance);
        }
        for (size_t i = 0; i < args.size(); i++)
        {
            if (!fits(args[i], sig.params[i]))
            {
                error(n, "argument " + to_string(i + 1) + " of '" + label + "' expects " + name(sig.params[i]) + " but got " +
                         name(args[i]));
            }
        }
        if (sig.inferred || instance != NoInstance)  // its return type is its first return's, or it is a new copy
        {
            if (states[state_key(instance, fn)] != State::Checking)
            {
                check_function(fn, label, false, instance);
            }
            else if (signature(fn, false, instance).returns.empty())  // a call back into it before its first return
            {
                if (fn == context.function && instance == context.instance) context.selfCalled = true;
                return Unknown;
            }
            sig = signature(fn, false, instance);
        }
        return sig.returns.empty() ? Unknown : sig.returns[0];
    }

    // zeros, matmul, tensor_print, ... (each checked against its signature), and print, input and array
    TypeId Typer::call_builtin(uint32_t n, const string& f, const vector<uint32_t>& args, const vector<TypeId>& types)
    {
        if (f == "print") return Unknown;
        if (f == "input") return Int;
        if (f == "array") return Array;
        auto it = TENSOR_OP_MAP.find(f);
        if (it == TENSOR_OP_MAP.end()) return Unknown;
        TensorOp op = it->second;
        if (types.size() > 8) error(n, "'" + f + "' takes at most 8 arguments");

        // the ops that take one argument per dimension
        int lo = -1, hi = -1;
        const int R = TENSOR_MAX_RANK;
        switch (op)
        {
            case TEN_ZEROS: case TEN_ONES: case TEN_RANDN:  lo = 1; hi = R; break;
            case TEN_GET:                                   lo = 2; hi = R + 1; break;
            case TEN_SET:                                   lo = 3; hi = R + 2; break;
            case TEN_RESHAPE:                               lo = 2; hi = R + 1; break;
            default: break;
        }
        if (lo >= 0 && ((int)types.size() < lo || (int)types.size() > hi))
        {
            error(n, "'" + f + "' takes " + to_string(lo) + " to " + to_string(hi) + " arguments, got " + to_string(types.size()));
        }

        vector<string> argNames;
        for (uint32_t a : args) argNames.push_back(tree.kind(a) == NodeKind::Name ? text(a) : "");
        tensor_args(n, "'" + f + "'", tensor_op_signature(op), types, argNames);
        if (TENSOR_VOID_OPS.count(op)) return Unknown;
        if (TENSOR_DOUBLE_OPS.count(op)) return Double;
        if (TENSOR_INT_OPS.count(op)) return Int;
        return Tensor;
    }

    // obj.m(...), v.push(x), m.has(k), hf.infer(...), sol.finetune(...)
    TypeId Typer::call_member(uint32_t n, uint32_t callee, const vector<uint32_t>& args, bool statement)
    {
        uint32_t base = tree.children(callee)[0];
        uint32_t e = tree.kind(base) == NodeKind::Name ? out.refs[base] : NoEntity;
        if (e != NoEntity && out.entities[e].kind == EntityKind::Module) return call_module(n, text(base), text(callee), args);
        TypeId b = expr(base);
        vector<TypeId> types;
        for (uint32_t a : args) types.push_back(expr(a));
        if (kind(b) == TypeKind::Class) return call_method(n, callee, b, types);
        if (is_container(b)) return call_container(n, callee, b, types, statement);
        if (b == Tensor) error(callee, "a Tensor has no member '" + text(callee) + "' (it has .T, .rows, .cols, and .rank)");
        return Unknown;
    }

    // hf.infer(model, input), hf.generate(model, prompt, max_tokens), hf.classify(model, text), hf.dataset(name, split),
    // hf.set_token(token); sol.finetune(model=, task=, dataset=, epochs=, lr=, batch_size=), sol.wait(job),
    // sol.predict(job, input), sol.save(job, path), sol.set_key(path). The values are strings, except max_tokens,
    // epochs, batch_size and lr: the instruction holds those, so they are numbers written out.
    TypeId Typer::call_module(uint32_t n, const string& module, const string& method, const vector<uint32_t>& args)
    {
        struct Method
        {
            const char* name;
            size_t least, most;     // positional arguments
            set<string> keywords;   // name=value arguments
            bool returns;           // a string
        };
        static const vector<Method> hf = {
            {"infer", 2, 2, {}, true},      {"generate", 2, 3, {"max_tokens"}, true}, {"classify", 2, 2, {}, true},
            {"dataset", 1, 2, {}, true},    {"set_token", 1, 1, {}, false},
        };
        static const vector<Method> sol = {
            {"finetune", 0, 0, {"model", "task", "dataset", "epochs", "lr", "batch_size"}, true},
            {"wait", 1, 1, {}, false},      {"predict", 2, 2, {}, true},  {"save", 2, 2, {}, false},
            {"set_key", 1, 1, {}, false},
        };
        const Method* m = nullptr;
        for (const Method& x : module == "hf" ? hf : sol)
        {
            if (method == x.name) m = &x;
        }
        if (!m)
        {
            for (uint32_t a : args) expr(tree.kind(a) == NodeKind::KeywordArg ? tree.children(a)[0] : a);
            error(n, "unknown " + module + " method: " + module + "." + method + "()");
            return Unknown;
        }
        string label = "'" + module + "." + method + "'";
        set<string> given;
        size_t count = 0;  // positional arguments
        for (uint32_t a : args)
        {
            bool keyword = tree.kind(a) == NodeKind::KeywordArg;
            uint32_t v = keyword ? tree.children(a)[0] : a;
            TypeId t = expr(v);
            if (!keyword) count++;
            string what = keyword ? text(a) : method == "generate" && count == 3 ? "max_tokens" : "";
            if (keyword && !m->keywords.count(what))
            {
                error(a, label + " has no argument named '" + what + "'");
                continue;
            }
            if (!what.empty() && !given.insert(what).second)
            {
                error(a, "'" + what + "' is given twice");
                continue;
            }
            if (what == "max_tokens" || what == "epochs" || what == "batch_size" || what == "lr")
            {
                bool real = what == "lr";
                NodeKind k = tree.kind(v);
                if (k != NodeKind::IntLiteral && !(real && k == NodeKind::FloatLiteral))
                {
                    error(v, "'" + what + "' must be " + (real ? "a number" : "a whole number") + " written out, e.g. " +
                             what + "=" + (real ? "0.00002" : "3"));
                }
            }
            else if (!fits(t, String))
            {
                error(v, (keyword ? "'" + what + "'" : "argument " + to_string(count)) + " of " + label +
                         " expects string but got " + name(t));
            }
        }
        if (method == "finetune")
        {
            if (count) error(n, label + " takes name=value arguments only: sol.finetune(model=\"...\", task=\"...\", ...)");
            else if (!given.count("model") || !given.count("task")) error(n, label + " needs model=\"...\" and task=\"...\"");
        }
        else if (count < m->least || count > m->most)
        {
            string takes = to_string(m->least) + (m->least == m->most ? "" : " or " + to_string(m->most));
            error(n, label + " takes " + takes + " argument(s) but got " + to_string(count));
        }
        return m->returns ? String : Unknown;
    }

    // obj.m(args): the method its class runs (its own, or the nearest parent's), its arguments checked as a
    // function's are
    TypeId Typer::call_method(uint32_t n, uint32_t callee, TypeId object, const vector<TypeId>& args)
    {
        string cls = name(object), method = text(callee), label = "'" + cls + "." + method + "'";
        uint32_t owner = NoEntity;
        uint32_t m = find_method(out.types[object].a, method, &owner);
        if (m == NoNode)
        {
            error(n, "class '" + cls + "' has no method '" + method + "'");
            return Unknown;
        }
        Signature sig = signature(m, true, binding(object, m));
        for (size_t i = 0; i < args.size() && i < sig.params.size(); i++)
        {
            if (!fits(args[i], sig.params[i]))
            {
                error(n, "argument " + to_string(i + 1) + " of " + label + " expects " + name(sig.params[i]) + " but got " +
                         name(args[i]));
            }
        }
        if (args.size() != sig.params.size())
        {
            error(n, label + " takes " + to_string(sig.params.size()) + " argument(s) but got " + to_string(args.size()));
        }
        return sig.returns.size() == 1 ? sig.returns[0] : Unknown;
    }

    // A vector's or a map's method. As a value: size(); for a vector pop(), contains(x), index_of(x),
    // binary_search(x), min(), max(), sum(); for a map has(k). As a statement: clear(); for a vector push(x), pop(),
    // sort(), reverse(); for a map remove(k). The algorithm methods need elements they can compare (sum(): numbers);
    // objects compare by identity only, so they allow contains() and index_of() alone.
    TypeId Typer::call_container(uint32_t n, uint32_t callee, TypeId container, const vector<TypeId>& args, bool statement)
    {
        uint32_t base = tree.children(callee)[0];
        bool isMap = kind(container) == TypeKind::Map, noArgs = args.empty();
        TypeId elem = isMap ? out.types[container].b : out.types[container].a;
        string v = path(base), m = text(callee);
        auto allows = [&]()
        {
            bool ok = elem == Unknown ? true :
                      m == "sum" ? (elem == Int || elem == Float || elem == Double) :
                      kind(elem) == TypeKind::Class ? (m == "contains" || m == "index_of") : elem != Tensor;
            if (!ok)
            {
                error(n, "'" + v + "." + m + "()' needs " + (m == "sum" ? "numbers" : "comparable elements") + ", but '" + v +
                         "' holds " + name(elem));
            }
            return ok;
        };
        if (!statement)
        {
            if (m == "size" && noArgs) return Int;
            if (isMap && m == "has" && !noArgs)
            {
                check_key(n, base, container, args[0]);
                return Bool;
            }
            if (!isMap && m == "pop" && noArgs) return elem;
            if (!isMap && (m == "contains" || m == "index_of" || m == "binary_search") && !noArgs)
            {
                check_element(n, base, container, args[0]);
                if (!allows()) return Unknown;
                return m == "contains" ? Bool : Int;
            }
            if (!isMap && (m == "min" || m == "max" || m == "sum") && noArgs) return allows() ? elem : Unknown;
            error(n, "'" + v + "." + m + "(...)' does not give a value here; a " +
                     (isMap ? "map has m[k], has(k), size()"
                            : "vector has v[i], size(), pop(), contains(x), index_of(x), binary_search(x), min(), max(), sum()"));
            return Unknown;
        }
        if (m == "clear" && noArgs) return Unknown;
        if (isMap && m == "remove" && !noArgs)
        {
            check_key(n, base, container, args[0]);
            return Unknown;
        }
        if (!isMap && m == "push" && !noArgs)
        {
            check_element(n, base, container, args[0]);
            return Unknown;
        }
        if (!isMap && (m == "pop" || m == "reverse") && noArgs) return Unknown;
        if (!isMap && m == "sort" && noArgs)
        {
            allows();
            return Unknown;
        }
        error(n, "'" + v + "." + m + "(...)' is not a " + (isMap ? "map" : "vector") + " method: a " +
                 (isMap ? "map has m[k] = x, remove(k), clear()" : "vector has v[i] = x, push(x), pop(), sort(), reverse(), clear()"));
        return Unknown;
    }

    // A vector's index must be an int, and a map's key of its key type
    void Typer::check_key(uint32_t n, uint32_t container, TypeId type, TypeId key)
    {
        bool isMap = kind(type) == TypeKind::Map;
        TypeId want = isMap ? out.types[type].a : Int;
        if (!fits(key, want))
        {
            error(n, string(isMap ? "key" : "index") + " of '" + path(container) + "' must be " + name(want) + ", got " +
                     name(key));
        }
    }

    // A value stored in a container, or looked for in one, must be of its element type
    void Typer::check_element(uint32_t n, uint32_t container, TypeId type, TypeId value)
    {
        TypeId elem = kind(type) == TypeKind::Map ? out.types[type].b : out.types[type].a;
        if (!fits(value, elem)) error(n, "'" + path(container) + "' holds " + name(elem) + ", not " + name(value));
    }

    // t[i, j] reads one element of a Tensor (a double); a[i] one of an array (an int); v[i] and m[k] one of a container
    TypeId Typer::index(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        vector<TypeId> types;
        for (uint32_t k : kids) types.push_back(expr(k));
        if (types[0] == Tensor)
        {
            tensor_args(n, "indexing", tensor_op_signature(TEN_GET), types, {});
            return Double;
        }
        if (is_container(types[0]) && types.size() == 2)
        {
            check_key(n, kids[0], types[0], types[1]);
            return kind(types[0]) == TypeKind::Map ? out.types[types[0]].b : out.types[types[0]].a;
        }
        return types[0] == Array ? Int : Unknown;
    }

    // A field of an object or a struct; t.T, t.rows, t.cols, t.rank on a Tensor
    TypeId Typer::member(uint32_t n)
    {
        TypeId b = expr(tree.children(n)[0]);
        string m = text(n);
        if (kind(b) == TypeKind::Class || kind(b) == TypeKind::Struct)
        {
            uint32_t f = find_field(out.types[b].a, m);
            if (f != NoNode) return field_type(f, binding(b, f));
            error(n, string(kind(b) == TypeKind::Class ? "class '" : "struct '") + name(b) + "' has no field '" + m + "'");
            return Unknown;
        }
        if (b != Tensor) return Unknown;
        if (m == "T") return Tensor;
        if (m == "rows" || m == "cols" || m == "rank") return Int;
        error(n, "a Tensor has no member '" + m + "' (it has .T, .rows, .cols, and .rank)");
        return Unknown;
    }
}  // namespace

void check_types(const ParseTree& tree, CheckResult& out)
{
    Typer(tree, out).run();
}

