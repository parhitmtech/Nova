// The check pass (check.h): what every name refers to, with scopes.
#include <map>
#include "check.h"
#include "compiler.h"

using namespace std;

extern const map<string, TensorOp> TENSOR_OP_MAP;  // the tensor built-ins (parser_state.cc)

namespace {
    // Every name in scope, in one table. visible[name] is the innermost binding of that name, and each binding 
    // remembers the one it hides, so leaving a scope pops its bindings and the outer names are back. Looking a name up
    // is one index into an array: there is no table per scope.
    class Scopes
    {
        public: 
            struct Binding 
            {
                Symbol name;
                uint32_t entity;
                uint32_t depth;  // the scope it was declared in (0: the built-ins)
                uint32_t hidden;  // the binding it hides, or None
            };

            void push() { marks.push_back(bindings.size()); }
            void pop()
            {
                for (size_t i = bindings.size(); i-- > marks.back();) visible[bindings[i].name] = bindings[i].hidden;
                bindings.resize(marks.back());
                marks.pop_back();
            }
            void declare(Symbol name, uint32_t entity)
            {
                if (name >= visible.size()) visible.resize(name + 1, None);
                bindings.push_back({name, entity, depth(), visible[name]});
                visible[name] = (uint32_t)bindings.size() - 1;
            }
            const Binding* find(Symbol name) const 
            {
                return name < visible.size() && visible[name] != None ? &bindings[visible[name]] : nullptr;
            }
            uint32_t depth() const { return (uint32_t)marks.size(); }
        
        private:
            static constexpr uint32_t None = UINT32_MAX;
            vector<Binding> bindings;
            vector<uint32_t> visible;  // by name
            vector<size_t> marks;      // where each open scope's bindings start
    };

    bool is_type_keyword(TokenType k)
    {
        return k == INT_TYPE || k == FLOAT_TYPE || k == DOUBLE_TYPE || k == BOOL_TYPE || k == STRING_TYPE || k == TENSOR_TYPE;
    }

    class Checker
    {
        public:
            Checker(const ParseTree& tree, CheckResult& out) : tree(tree), out(out) { out.refs.assign(tree.nodes.size(), NoEntity); }
            void check_file();

        private:
            const ParseTree& tree;
            CheckResult& out;
            Scopes scopes;
            int loops = 0;          // the loops around the current statement, in the current function
            bool inFunction = false;        // whether 'return' is allowed here
            uint32_t selfClass = NoEntity;  // the class whose method is being checked

            Symbol name(uint32_t n) const { return tree.token(n).symbol; }
            string text(uint32_t n) const { return string(tree.spelling(n)); }
            void error(uint32_t n, const string& message)
            {
                out.errors.push_back("Error at line " + to_string(tree.token(n).line) + ": " + message);
            }
            uint32_t add_entity(EntityKind kind, Symbol name, uint32_t decl)
            {
                out.entities.push_back({kind, name, decl});
                return (uint32_t)out.entities.size() - 1;
            }
            uint32_t lookup(Symbol s) const  // the entity a name refers to here, or NoEntity
            {
                const Scopes::Binding* b = scopes.find(s);
                return b ? b->entity : NoEntity;
            }
            bool is_variable(Symbol s) const 
            {
                uint32_t e = lookup(s);
                return e != NoEntity && out.entities[e].kind == EntityKind::Variable;
            }
            bool is_generic_class(const Entity& e) const 
            {
                return e.kind == EntityKind::Class && tree.kind(tree.children(e.decl)[0]) == NodeKind::TypeParams;
            }

            void declare_builtins();
            void declare_definition(uint32_t n);
            void declare_variable(uint32_t n);
            bool has_main() const;
            void check_function(uint32_t n);
            void check_struct(uint32_t n);
            void check_class(uint32_t n);
            void check_field(uint32_t n);
            void check_type(uint32_t n, const char* unknown = "unknown type");
            void check_type_argument(uint32_t n);
            void check_block(uint32_t n);
            void check_statement(uint32_t n);
            void check_expression(uint32_t n);
            void refer(uint32_t n, const char* unknown);
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // Definitions
    // ─────────────────────────────────────────────────────────────────────────────

    void Checker::check_file()
    {
        declare_builtins();
        scopes.push();  // the file's definitions; one can hide a built-in of the same name
        vector<uint32_t> items = tree.children(tree.root());
        for (uint32_t n : items)
        {
            if (tree.kind(n) != NodeKind::Ignite) declare_definition(n);
        }

        uint32_t ignite = NoNode;
        for (uint32_t n : items)
        {
            switch (tree.kind(n))
            {
                case NodeKind::FunctionDecl: check_function(n); break;
                case NodeKind::StructDecl:   check_struct(n); break;
                case NodeKind::ClassDecl:    check_class(n); break;
                case NodeKind::Ignite:
                    if (ignite != NoNode) error(n, "a file can have only one 'ignite' block");
                    ignite = n;
                    check_block(tree.children(n)[0]);
                    break;
                default: break;
            }
        }

        if (ignite == NoNode && !has_main())
        {
            // at the last line with code on it, however the file ends
            const vector<LexToken>& toks = *tree.tokens;
            int line = 1;
            for (size_t i = toks.size(); i-- > 0;)
            {
                TokenType k = toks[i].kind;
                if (k != NEWLINE && k != INDENT && k != DEDENT && k != END_OF_FILE)
                {
                    line = toks[i].line;
                    break;
                }
            }
            out.errors.push_back("Error at line " + to_string(line) + 
                                 ": no entry point: add an 'ignite:' block at the end of the file (or define 'def main()')");
        }
        scopes.pop();
    }

    // The names every program can use without defining them
    void Checker::declare_builtins()
    {
        auto add = [&](EntityKind kind, string_view text) {
            Symbol s = names.intern(text);
            scopes.declare(s, add_entity(kind, s, NoNode));
        };
        for (const auto& op : TENSOR_OP_MAP) add(EntityKind::Builtin, op.first);
        for (const char* b : {"print", "input", "array"}) add(EntityKind::Builtin, b);
        add(EntityKind::BuiltinType, "vector");
        add(EntityKind::BuiltinType, "map");
        add(EntityKind::Module, "hf");
        add(EntityKind::Module, "sol");
        add(EntityKind::None, "none");
    }

    void Checker::declare_definition(uint32_t n)
    {
        EntityKind kind = tree.kind(n) == NodeKind::FunctionDecl ? EntityKind::Function : 
                          tree.kind(n) == NodeKind::StructDecl   ? EntityKind::Struct : EntityKind::Class;
        const Scopes::Binding* b = scopes.find(name(n));
        if (b && b->depth == scopes.depth())
        {
            const char* what = kind == EntityKind::Function ? "function" : kind == EntityKind::Struct ? "struct" : "class";
            error(n, string("duplicate ") + what + " definition '" + text(n) + "'");
            return;
        }
        uint32_t e = add_entity(kind, name(n), n);
        out.refs[n] = e;
        scopes.declare(name(n), e);
    }

    // A new variable, named by node n's token. A visible variable or parameter cannot be declared again (variables do
    // not shadow each other), but a variable can hide a definition or a built-in of the same name.
    void Checker::declare_variable(uint32_t n)
    {
        if (is_variable(name(n)))
        {
            error(n, "redeclaration of '" + text(n) + "'");
            return;
        }
        uint32_t e = add_entity(EntityKind::Variable, name(n), n);
        out.refs[n] = e;
        scopes.declare(name(n), e);
    }

    // def main() with no parameters is the entry point of a file without an ignite block
    bool Checker::has_main() const 
    {
        uint32_t e = lookup(names.intern("main"));
        if (e == NoEntity || out.entities[e].kind != EntityKind::Function) return false;
        return tree.children(tree.children(out.entities[e].decl)[0]).empty();
    }

    // The parameters, then the body. A method is checked with selfClass set to its class.
    void Checker::check_function(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);  // ParamList, ReturnTypes if written, Block
        bool outerInFunction = inFunction;
        int outerLoops = loops;
        inFunction = true;
        loops = 0;
        scopes.push();
        for (uint32_t p : tree.children(kids[0]))
        {
            vector<uint32_t> type = tree.children(p);
            if (!type.empty()) check_type(type[0]);
            if (tree.token(p).kind != SELF) declare_variable(p);
        }
        if (kids.size() == 3)
        {
            for (uint32_t t : tree.children(kids[1])) check_type(t);
        }
        check_block(kids.back());
        scopes.pop();
        inFunction = outerInFunction;
        loops = outerLoops;
    }

    void Checker::check_struct(uint32_t n)
    {
        for (uint32_t f : tree.children(n))
        {
            if (tree.kind(f) == NodeKind::FieldDecl) check_field(f);
        }
    }

    // The type parameters are in scope for the whole class; each method knows its class, for 'self'.
    void Checker::check_class(uint32_t n)
    {
        uint32_t outerClass = selfClass;
        selfClass = out.refs[n];
        scopes.push();
        for (uint32_t m : tree.children(n))
        {
            switch (tree.kind(m))
            {
                case NodeKind::TypeParams:
                {
                    for (uint32_t t : tree.children(m))
                    {
                        uint32_t e = add_entity(EntityKind::TypeParam, name(t), t);
                        out.refs[t] = e;
                        scopes.declare(name(t), e);
                    }
                    break;
                }
                case NodeKind::Extends:
                {
                    uint32_t t = tree.children(m)[0];
                    uint32_t e = tree.kind(t) == NodeKind::TypeName ? lookup(name(t)) : NoEntity;
                    if (e == NoEntity || out.entities[e].kind != EntityKind::Class)
                    {
                        error(t, "unknown parent class '" + text(t) + "'");
                    }
                    else
                    {
                        out.refs[t] = e;
                    }
                    break;
                }
                case NodeKind::FieldDecl:    check_field(m); break;
                case NodeKind::FunctionDecl: check_function(m); break;
                default: break;
            }
        }
        scopes.pop();
        selfClass = outerClass;
    }

    // A field's type, and an array field's size
    void Checker::check_field(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        check_type(kids[0], "unknown field type");
        if (kids.size() > 1) check_expression(kids[1]);
    }

    // A written type: a type keyword, a struct, a class, a type parameter, or vector / map / a generic class with its
    // type arguments.
    void Checker::check_type(uint32_t n, const char* unknown)
    {
        bool hasArguments = tree.kind(n) == NodeKind::GenericType;
        if (!is_type_keyword(tree.token(n).kind))
        {
            uint32_t e = lookup(name(n));
            EntityKind k = e == NoEntity ? EntityKind::Variable : out.entities[e].kind;
            if (k != EntityKind::Struct && k != EntityKind::Class && k != EntityKind::TypeParam && k != EntityKind::BuiltinType)
            {
                error(n, string(unknown) + " '" + text(n) + "'");
                return;
            }
            out.refs[n] = e;
            bool generic = k == EntityKind::BuiltinType || is_generic_class(out.entities[e]);
            if (generic && !hasArguments)
            {
                error(n, (k == EntityKind::Class ? "class '" : "'") + text(n) + "' is generic: write its type arguments, e.g. " + 
                          text(n) + "<int>");
                return;
            }
            if (!generic && hasArguments)
            {
                error(n, "'" + text(n) + "' takes no type arguments");
                return;
            }
        }
        for (uint32_t arg : tree.children(n)) check_type_argument(arg);
    }

    void Checker::check_type_argument(uint32_t n)
    {
        if (tree.kind(n) == NodeKind::TypeName && !is_type_keyword(tree.token(n).kind))
        {
            uint32_t e = lookup(name(n));
            EntityKind k = e == NoEntity ? EntityKind::Variable : out.entities[e].kind;
            if (k != EntityKind::Struct && k != EntityKind::Class && k != EntityKind::TypeParam)
            {
                error(n, "'" + text(n) + "' cannot be a type argument: use int, float, double, bool, string, Tensor, a class, "
                         "vector<T> or map<K, V>");
                return;
            }
        }
        check_type(n);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Statements and expressions
    // ─────────────────────────────────────────────────────────────────────────────

    void Checker::check_block(uint32_t n)
    {
        scopes.push();
        for (uint32_t s : tree.children(n)) check_statement(s);
        scopes.pop();
    }

    void Checker::check_statement(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        switch (tree.kind(n))
        {
            case NodeKind::VarDecl:  // the value is checked first: in "int n = n + 1" the n on the right is not this one
            {
                check_type(kids[0]);
                if (kids.size() > 1) check_expression(kids[1]);
                declare_variable(n);
                return;
            }
            case NodeKind::MultiDecl:
            {
                check_expression(kids.back());
                for (size_t i = 0; i + 1 < kids.size(); i++)
                {
                    check_type(tree.children(kids[i])[0]);
                    declare_variable(kids[i]);
                }
                return;
            }
            case NodeKind::Assign:  // "x = 5" declares x when no variable x is visible
            {
                check_expression(kids[1]);
                if (tree.kind(kids[0]) == NodeKind::Name && !is_variable(name(kids[0]))) declare_variable(kids[0]);
                else check_expression(kids[0]);
                return;
            }
            case NodeKind::CompoundAssign:
            case NodeKind::ExprStatement:
            {
                for (uint32_t c : kids) check_expression(c);
                return;
            }
            case NodeKind::If:
            {
                check_expression(kids[0]);
                check_block(kids[1]);
                for (size_t i = 2; i < kids.size(); i++)
                {
                    vector<uint32_t> part = tree.children(kids[i]);  // Elif: condition, Block; Else: Block
                    if (part.size() == 2) check_expression(part[0]);
                    check_block(part.back());
                }
                return;
            }
            case NodeKind::While:
            {
                check_expression(kids[0]);
                loops++;
                check_block(kids[1]);
                loops--;
                return;
            }
            case NodeKind::DoWhile:
            {
                loops++;
                check_block(kids[0]);
                loops--;
                check_expression(kids[1]);
                return;
            }
            case NodeKind::For:  // the start runs in the enclosing scope, so "for (i = 0; ...)" declares i there
            {
                if (tree.kind(kids[0]) != NodeKind::Empty) check_statement(kids[0]);
                if (tree.kind(kids[1]) != NodeKind::Empty) check_expression(kids[1]);
                if (tree.kind(kids[2]) != NodeKind::Empty) check_statement(kids[2]);
                loops++;
                check_block(kids[3]);
                loops--;
                return;
            }
            case NodeKind::Switch:
            {
                check_expression(kids[0]);
                for (size_t i = 1; i < kids.size(); i++)
                {
                    vector<uint32_t> part = tree.children(kids[i]);  // Case: value, Block; Default: Block
                    if (part.size() == 2) check_expression(part[0]);
                    check_block(part.back());
                }
                return;
            }
            case NodeKind::Return:
            {
                if (!inFunction) error(n, "return statement outside of a function");
                for (uint32_t c : kids) check_expression(c);
                return;
            }
            case NodeKind::Break:
            case NodeKind::Continue:
            {
                if (loops == 0) error(n, "'" + text(n) + "' is only allowed inside a loop");
                return;
            }
            default: //Pass
                return;
        }
    }

    void Checker::check_expression(uint32_t n)
    {
        switch (tree.kind(n))
        {
            case NodeKind::Name:
            {
                refer(n, "undeclared variable");
                return;
            }
            case NodeKind::Self:
            {
                if (selfClass == NoEntity) error(n, "'self' used outside of a class method");
                else out.refs[n] = selfClass;
                return;
            }
            case NodeKind::Call:
            {
                vector<uint32_t> kids = tree.children(n);
                switch (tree.kind(kids[0]))
                {
                    case NodeKind::Name:        refer(kids[0], "call to undefined function"); break;
                    case NodeKind::TypeName:                                        // float(x)
                    case NodeKind::GenericType: check_type(kids[0]); break;         // Box<int>(5)
                    default:                    check_expression(kids[0]); break;   // a method: v.push(x)
                }
                for (size_t i = 1; i < kids.size(); i++) check_expression(kids[i]);
                return;
            }
            case NodeKind::Member:      // the member itself is found from the value's type
            {
                check_expression(tree.children(n)[0]);
                return;
            }
            default:
            {
                for (uint32_t c : tree.children(n)) check_expression(c);
                return;
            }
        }
    }

    // A name used in an expression
    void Checker::refer(uint32_t n, const char* unknown)
    {
        uint32_t e = lookup(name(n));
        if (e == NoEntity) error(n, string(unknown) + " '" + text(n) + "'");
        else out.refs[n] = e;
    }
}  // namespace

CheckResult check_tree(const ParseTree& tree)
{
    CheckResult out;
    Checker(tree, out).check_file();
    return out;
}