// The check pass, second step (check.h): the type of every expression, with the rules for numbers, strings,
// tensors, arrays, structs and function calls. Names are already resolved (check.cc), so this step reads entities
// through refs and needs no scopes: it can check a function's body whenever a caller first needs its return type.
//
// A value this step does not type yet (a field, a method's result, a container's element, an untyped parameter) is
// Unknown, and Unknown fits everywhere, so it never causes an error. Objects, containers and generics come next.
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
        case TypeKind::Struct:
        case TypeKind::Class:       return string(names.text(checked.entities[t.a].name));
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
            // messages (f, or Linear.forward).
            struct Context
            {
                uint32_t function = NoNode;
                string label;
                bool selfCalled = false;  // it called itself before its first return, so its return type was not known yet
            };
            Context context;
            enum class State : uint8_t { NotChecked, Checking, Done };
            unordered_map<uint32_t, State> states;  // by FunctionDecl

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
            TypeId class_type(uint32_t classEntity)  // a generic class's copies are typed with generics
            {
                return is_generic_class(classEntity) ? Unknown : out.types.intern({TypeKind::Class, classEntity});
            }

            bool fits(TypeId value, TypeId declared) const;
            TypeId written(uint32_t n, bool report);
            string spell(uint32_t n) const;
            Signature& signature(uint32_t fn, bool method);
            void check_function(uint32_t fn, const string& label, bool method);
            void check_field(uint32_t n);
            void set_variable(uint32_t declNode, TypeId t);

            void check_block(uint32_t n);
            void check_statement(uint32_t n);
            void check_return(uint32_t n, const vector<uint32_t>& values);
            void check_assign(uint32_t n, TypeId value, TypeId declared);
            void condition(uint32_t n);
            void store(uint32_t target, TypeId value);

            TypeId expr(uint32_t n);
            TypeId unary(uint32_t n);
            TypeId binary(uint32_t n, TokenType op, TypeId lt, TypeId rt);
            TypeId tensor_binary(uint32_t n, TokenType op, TypeId lt, TypeId rt);
            void tensor_args(uint32_t n, const string& what, const char* signature, const vector<TypeId>& types,
                             const vector<string>& argNames);
            TypeId call(uint32_t n);
            TypeId call_name(uint32_t n, uint32_t callee, const vector<uint32_t>& args);
            TypeId call_function(uint32_t n, uint32_t fn, const string& f, const vector<TypeId>& args);
            TypeId call_builtin(uint32_t n, const string& f, const vector<uint32_t>& args, const vector<TypeId>& types);
            TypeId call_member(uint32_t n, uint32_t callee, const vector<uint32_t>& args);
            TypeId index(uint32_t n);
            TypeId member(uint32_t n);
    };
    
    // May a value of type 'value' go where 'declared' is expected (a variable, a parameter, a return value)? The same
    // type, a float widening to a double, or either one Unknown. Objects and containers get their own rules with
    // objects; until then any two of them fit.
    bool Typer::fits(TypeId value, TypeId declared) const
    {
        if (value == declared || value == Unknown || declared == Unknown) return true;
        if (value == Float && declared == Double) return true;
        return is_reference(value) && is_reference(declared);
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
                    if (tree.kind(members[0]) == NodeKind::TypeParams) break;  // each copy is checked with generics
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
    // be. A generic class's copy and a type parameter are Unknown until generics are checked.
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
            if (report && !args.empty() && args.size() != want)
            {
                error(n, "class '" + text(n) + "' takes " + to_string(want) + " type argument(s) but got " + 
                         to_string(args.size()));
            }
            else if (report)
            {
                for (uint32_t a : args)
                {
                    bool nested = false;  // a container of containers, or of a generic class's copy
                    for (uint32_t inner : tree.children(a)) nested |= tree.kind(inner) == NodeKind::GenericType;
                    uint32_t ae = out.refs[a];
                    bool isStruct = ae != NoEntity && out.entities[ae].kind == EntityKind::Struct;
                    bool isGenericClass = ae != NoEntity && out.entities[ae].kind == EntityKind::Class && is_generic_class(ae);
                    if (nested || isStruct || isGenericClass)
                    {
                        error(n, "'" + spell(a) + "' cannot be a type argument: use int, float, double, bool, string, Tensor, "
                                 "a class, vector<T> or map<K, V>");
                    }
                    written(a, report);
                }
            }
            return Unknown;
        }        
        if (ent.kind != EntityKind::BuiltinType) return Unknown;  // a type parameter

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

    // A written type as the source spells it, for messages: vector<vector<int>>
    string Typer::spell(uint32_t n) const 
    {
        string s = text(n);
        vector<uint32_t> args = tree.children(n);
        for (size_t i = 0; i < args.size(); i++) s += (i == 0 ? "<" : ", ") + spell(args[i]);
        return args.empty() ? s : s + ">";
    }

    // A function's parameter and return types, as written. Only a function (not a method) with an untyped parameter
    // is generic: a method's untyped parameter is Unknown.
    Signature& Typer::signature(uint32_t fn, bool method)
    {
        auto it = out.signatures.find(fn);
        if (it != out.signatures.end()) return it->second;
        Signature s;
        vector<uint32_t> kids = tree.children(fn);  // ParamList, ReturnTypes if written, Block
        for (uint32_t p : tree.children(kids[0]))
        {
            if (tree.token(p).kind == SELF) continue;
            vector<uint32_t> type = tree.children(p);
            if (type.empty() && !method) s.generic = true;
            s.params.push_back(type.empty() ? Unknown : written(type[0], false));
        }
        s.inferred = kids.size() == 2;
        if (!s.inferred)
        {
            for (uint32_t t : tree.children(kids[1])) s.returns.push_back(written(t, false));
        }
        return out.signatures[fn] = s;
    }

    // A function's body, once: in file order, or earlier when a caller first needs its return type. A generic
    // function's body is checked for each copy, with generics.
    void Typer::check_function(uint32_t fn, const string& label, bool method)
    {
        if (states[fn] != State::NotChecked) return;
        states[fn] = State::Checking;
        if (signature(fn, method).generic)
        {
            states[fn] = State::Done;
            return;
        }
        Context outer = context;
        context = Context();
        context.function = fn;
        context.label = label;
        vector<uint32_t> kids = tree.children(fn);
        for (uint32_t p : tree.children(kids[0]))
        {
            vector<uint32_t> type = tree.children(p);
            if (!type.empty()) set_variable(p, written(type[0], true));
        }
        if (kids.size() == 3)
        {
            for (uint32_t t : tree.children(kids[1])) written(t, true);
        }
        check_block(kids.back());
        context = outer;
        states[fn] = State::Done;
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
            case NodeKind::ExprStatement:
            {
                expr(kids[0]);
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
        Signature& sig = out.signatures[context.function];
        const string& f = context.label;
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
            if (!fits(types[i], sig.returns[i]))
            {
                error(n, "'" + f + "' returns " + name(types[i]) + " but is declared '-> " + name(sig.returns[i]) + "'");
            }
        }
    }

    // A value stored in the variable named by node n's token, of type 'declared'. An object variable takes only an 
    // object: Box b = 5 does not make a Box.
    void Typer::check_assign(uint32_t n, TypeId value, TypeId declared)
    {
        if (kind(declared) == TypeKind::Class && value != Unknown && value != None && kind(value) != TypeKind::Class)
        {
            error(n, "a " + name(declared) + " is created with " + name(declared) + "(...), which runs its init");
        }
        else if (!fits(value, declared))
        {
            error(n, "cannot assign " + name(value) + " to " + name(declared) + " variable '" + text(n) + "'");
        }
    }

    // Any number (or tensor, or object) can be a condition, as in C; a string cannot.
    void Typer::condition(uint32_t n)
    {
        if (expr(n) == String) error(n, "a string is not a condition; compare it instead, e.g. (s == \"yes\")");
    }

    // A value stored into a variable (the first known value fixes an implicitly declared variable's type), a tensor
    // element or an array element. Fields and container elements are checked with objects and containers.
    void Typer::store(uint32_t target, TypeId value)
    {
        if (tree.kind(target) == NodeKind::Name)
        {
            uint32_t e = out.refs[target];
            if (e == NoEntity || out.entities[e].kind != EntityKind::Variable) return;
            Entity& var = out.entities[e];
            if (var.type == Unknown)
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
            return;
        }
        expr(target);
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
            case NodeKind::Call:        return typed(n, call(n));
            case NodeKind::Index:       return typed(n, index(n));
            case NodeKind::Member:      return typed(n, member(n));
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
        error(n, "arithmetic requires int operands, got " + name(t));
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
                // objects compare by their own rules, checked with objects
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

    TypeId Typer::call(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        uint32_t callee = kids[0];
        vector<uint32_t> args(kids.begin() + 1, kids.end());
        switch (tree.kind(callee))
        {
            case NodeKind::Name:        return call_name(n, callee, args);
            case NodeKind::Member:      return call_member(n, callee, args);
            case NodeKind::TypeName:        // float(x), int(x), double(x): a conversion
            case NodeKind::GenericType:     // vector<int>(), map<string, int>(), Box<int>(5)
            {
                TypeId t = written(callee, true);
                for (uint32_t a : args) expr(a);
                switch (tree.token(callee).kind)
                {
                    case INT_TYPE:          return Int;
                    case FLOAT_TYPE:        return Float;
                    case DOUBLE_TYPE:       return Double;
                    default:                return kind(t) == TypeKind::Vector || kind(t) == TypeKind::Map ? t : Unknown;
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
            case EntityKind::Class:         return class_type(e);  // its init's arguments are checked with objects
            default:                        return Unknown;
        }
    }

    TypeId Typer::call_function(uint32_t n, uint32_t fn, const string& f, const vector<TypeId>& args)
    {
        Signature sig = signature(fn, false);  // a copy: checking the callee below adds signatures
        if (args.size() != sig.params.size())
        {
            error(n, "function '" + f + "' expects " + to_string(sig.params.size()) + " argument(s) but got " + 
                     to_string(args.size()));
            return Unknown;
        }
        if (sig.generic) return Unknown;  // each call's copy is checked with generics
        for (size_t i = 0; i < args.size(); i++)
        {
            if (!fits(args[i], sig.params[i]))
            {
                error(n, "argument " + to_string(i + 1) + " of '" + f + "' expects " + name(sig.params[i]) + " but got " +
                         name(args[i]));
            }
        }
        if (sig.inferred)  // its return type is its first return's: check it now if it has not been
        {
            if (states[fn] == State::Checking)  // a call back into a function whose first return is not reached yet
            {
                if (out.signatures[fn].returns.empty())
                {
                    if (fn == context.function) context.selfCalled = true;
                    return Unknown;
                }
            }
            else
            {
                check_function(fn, f, false);
            }
            sig = out.signatures[fn];
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

    // hf.infer(...) and sol.finetune(...); a method of an object or a container is checked with those 
    TypeId Typer::call_member(uint32_t n, uint32_t callee, const vector<uint32_t>& args)
    {
        uint32_t base = tree.children(callee)[0];
        uint32_t e = tree.kind(base) == NodeKind::Name ? out.refs[base] : NoEntity;
        if (e != NoEntity && out.entities[e].kind == EntityKind::Module)
        {
            for (uint32_t a : args) expr(a);
            string module = text(base), method = text(callee);
            if (module == "hf")
            {
                static const set<string> methods = {"infer", "generate", "classify", "set_token", "dataset"};
                if (!methods.count(method))
                {
                    error(n, "unknown hf method: hf." + method + "()");
                    return Unknown;
                }
                return method == "set_token" ? Unknown : String;
            }
            static const set<string> methods = {"finetune", "wait", "predict", "save", "set_key"};
            if (!methods.count(method))
            {
                error(n, "unknown sol method: sol." + method + "()");
                return Unknown;
            }
            return (method == "finetune" || method == "predict") ? String : Unknown;
        }
        TypeId b = expr(base);
        for (uint32_t a : args) expr(a);
        if (b == Tensor) error(callee, "a Tensor has no member '" + text(callee) + "' (it has .T, .rows, .cols, and .rank)");
        return Unknown;
    }

    // t[i, j] reads one element of a Tensor (a double); a[i] one of an array (an int)
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
        return types[0] == Array ? Int : Unknown;
    }

    // t.T, t.rows, t.cols, t.rank on a Tensor; fields are typed with objects
    TypeId Typer::member(uint32_t n)
    {
        if (expr(tree.children(n)[0]) != Tensor) return Unknown;
        string m = text(n);
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

