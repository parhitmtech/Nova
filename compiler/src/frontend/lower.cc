// Lowering (lower.h): one walk over the checked tree, writing each function's instructions and then the ignite
// block's. A value lives in a slot of mem[] (an int, a bool, or a string as its index in strMem), fmem[] (a float)
// or dmem[] (a double), laid out as the old parser lays them out: a function's int slots are one range, which a call
// saves and restores so a recursive call does not overwrite its caller's locals.
//
// Functions are lowered in file order, and a function a call needs before its turn is lowered right then. A call
// into a function whose body is still being lowered (a recursive call) gets its target when that body is done.
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "lower.h"

using namespace std;

extern map<string, int> symbolTable;        // the ignite block's variables, which the debugger shows
extern map<string, int> floatSymbolTable;   // (parser_state.cc)
extern map<string, int> doubleSymbolTable;
extern map<string, vector<int>> functionReturnSlots;  // read by the tensor-free pass

namespace
{
    // ─────────────────────────────────────────────────────────────────────────────
    // What lowering covers: 2c-1, numbers, bools, strings, arrays, control flow and functions
    // ─────────────────────────────────────────────────────────────────────────────

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
            case TypeKind::Array:       return true;
            default:                    return false;
        }
    }

    // Whether lowering covers node n (each node is asked on its own)
    bool covered(const ParseTree& tree, const CheckResult& checked, uint32_t n)
    {
        auto entity = [&](uint32_t m) { return checked.refs[m] == NoEntity ? nullptr : &checked.entities[checked.refs[m]]; };
        vector<uint32_t> kids = tree.children(n);
        switch (tree.kind(n))
        {
            case NodeKind::StructDecl:
            case NodeKind::ClassDecl:
            case NodeKind::GenericType:
            case NodeKind::Self:
            case NodeKind::Member:          return false;
            case NodeKind::FunctionDecl:    // not a generic function, nor a method (a generic class's are in its copies)
            {
                auto it = checked.signatures.find(n);
                return it != checked.signatures.end() && !it->second.generic;
            }
            case NodeKind::TypeName:
            {
                TokenType k = tree.token(n).kind;
                return k == INT_TYPE || k == FLOAT_TYPE || k == DOUBLE_TYPE || k == BOOL_TYPE || k == STRING_TYPE;
            }
            case NodeKind::MultiDecl:       // int a, double b = f(): a call to a function
            {
                uint32_t value = kids.back();
                if (tree.kind(value) != NodeKind::Call) return false;
                const Entity* e = entity(tree.children(value)[0]);
                return e && e->kind == EntityKind::Function;
            }
            case NodeKind::Case:            return checked.types[checked.nodeTypes[kids[0]]].kind == TypeKind::Int;
            case NodeKind::Binary:
            {
                if (tree.token(n).kind == AT) return false;
                break;
            }
            case NodeKind::Index:           // a[i]: one index into an array variable
            {
                if (kids.size() != 2 || tree.kind(kids[0]) != NodeKind::Name) return false;
                break;
            }
            case NodeKind::Call:
            {
                if (tree.kind(kids[0]) == NodeKind::TypeName)  // a conversion: int(x), float(x), double(x)
                {
                    TokenType k = tree.token(kids[0]).kind;
                    if (k != INT_TYPE && k != FLOAT_TYPE && k != DOUBLE_TYPE) return false;
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
                    return f == "print" || f == "input" || f == "array";
                }
                if (e->kind != EntityKind::Variable && e->kind != EntityKind::Function) return false;
                break;
            }
            default:                        break;
        }
        bool expression = tree.kind(n) >= NodeKind::IntLiteral;
        return !expression || covered_type(checked, checked.nodeTypes[n]);
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
            // is placed when it runs, and slot and size are the int slots that hold its place and length.
            struct Variable
            {
                int slot = -1;
                int size = 0;
                bool dynamic = false;
            };
            unordered_map<uint32_t, Variable> variables;  // by entity

            struct Function
            {
                enum State : uint8_t { NotLowered, Lowering, Done } state = NotLowered;
                InstructionNode* head = nullptr;
                int slotBase = 0, slotCount = 0;  // its int slots
                vector<int> params, returns;      // their slots
                vector<VarType> paramTypes, returnTypes;
                vector<InstructionNode*> pending;  // calls made before its body was done
            };
            unordered_map<uint32_t, Function> functions;  // by FunctionDecl

            struct Loop
            {
                InstructionNode* breakTo;
                InstructionNode* continueTo;
            };
            vector<Loop> loops;  // the loops around the statement being lowered, innermost last
            uint32_t function = NoNode;  // the FunctionDecl being lowered; NoNode in the ignite block

            VarType type_of(uint32_t n) const { return var_type(in.types[in.nodeTypes[n]].kind); }
            VarType var_type_of(uint32_t entity) const { return var_type(in.types[in.entities[entity].type].kind); }
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
            int declare(uint32_t entity);

            // Definitions
            Function& lower_function(uint32_t fn);
            void call_main();

            // Statements
            void block(uint32_t n);
            void statement(uint32_t n);
            void var_decl(uint32_t n);
            void multi_decl(uint32_t n);
            void assign_statement(uint32_t target, uint32_t value, int line);
            void store(uint32_t target, int value, VarType type, int line);
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
            InstructionNode* call_instruction(uint32_t n, uint32_t fn, const vector<uint32_t>& args);
            int call_function(uint32_t n, uint32_t fn, const vector<uint32_t>& args);
            void print(uint32_t n, uint32_t arg);
            void array_access(InstructionType type, uint32_t array, int index, int value, int line);
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

    int Lowerer::new_slot(VarType t)
    {
        if (t == TYPE_FLOAT) return alloc_float_slot();
        if (t == TYPE_DOUBLE) return alloc_double_slot();
        return alloc_slot();
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

    // A variable's slot, made where it is declared
    int Lowerer::declare(uint32_t entity)
    {
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

    // Every function, in file order, then the ignite block (or a call to main() when there is none)
    InstructionNode* Lowerer::run()
    {
        uint32_t ignite = NoNode;
        for (uint32_t n : tree.children(tree.root()))
        {
            if (tree.kind(n) == NodeKind::FunctionDecl) lower_function(n);
            else if (tree.kind(n) == NodeKind::Ignite) ignite = n;
        }
        if (ignite != NoNode) block(tree.children(ignite)[0]);
        else call_main();
        if (head == nullptr) add(NOOP, ignite != NoNode ? line(ignite) : 0);
        return head;
    }

    // A function's body, once. Its parameters, return slots and locals are its int slot range; it ends with a return
    // of whatever is in its return slot, for a body that ends without one.
    Lowerer::Function& Lowerer::lower_function(uint32_t fn)
    {
        Function& f = functions[fn];
        if (f.state != Function::NotLowered) return f;
        f.state = Function::Lowering;
        InstructionNode* outerHead = head;
        InstructionNode* outerTail = tail;
        vector<Loop> outerLoops = loops;
        uint32_t outerFunction = function;
        head = tail = nullptr;
        loops.clear();
        function = fn;

        const Signature& sig = in.signatures.at(fn);
        vector<uint32_t> kids = tree.children(fn);  // ParamList, ReturnTypes if written, Block
        f.slotBase = next_available;
        for (uint32_t p : tree.children(kids[0]))
        {
            f.params.push_back(declare(in.refs[p]));
            f.paramTypes.push_back(var_type_of(in.refs[p]));
        }
        for (TypeId t : sig.returns) f.returnTypes.push_back(var_type(in.types[t].kind));
        if (f.returnTypes.empty()) f.returnTypes.push_back(TYPE_UNKNOWN);  // returns nothing
        for (VarType t : f.returnTypes) f.returns.push_back(new_slot(t));
        string name = text(fn);
        functionParamTypes[name] = f.paramTypes;
        functionReturnType[name] = f.returnTypes[0];
        functionReturnSlots[name] = f.returns;

        block(kids.back());
        add(RET, line(fn))->ret_inst = {f.returns[0], f.returnTypes[0]};
        f.head = head;
        f.slotCount = next_available - f.slotBase;
        functionTable[name] = head;
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
        return f;
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
            case NodeKind::ExprStatement:   expr(kids[0]); return;
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

    // int x = e, or int x: a number starts at 0 each time the declaration runs (a loop body runs it again)
    void Lowerer::var_decl(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        uint32_t e = in.refs[n];
        VarType t = var_type_of(e);
        if (kids.size() == 1)
        {
            int start = zero(t);
            assign(t, declare(e), start, OPERATOR_NONE, 0, line(n));
            return;
        }
        int value = expr(kids[1]);
        copy(declare(e), t, value, type_of(kids[1]), line(n));
    }

    // int q, int r = divmod(a, b): one variable per value the function returns, each copied from its return slot
    void Lowerer::multi_decl(uint32_t n)
    {
        vector<uint32_t> kids = tree.children(n);
        vector<uint32_t> call = tree.children(kids.back());
        uint32_t fn = in.entities[in.refs[call[0]]].decl;
        InstructionNode* node = call_instruction(kids.back(), fn, vector<uint32_t>(call.begin() + 1, call.end()));
        const Function& f = functions[fn];
        node->call_inst.all_ret_slots = array_of(f.returns);
        node->call_inst.ret_slot_types = array_of(f.returnTypes);
        node->call_inst.num_ret_slots = (int)f.returns.size();
        for (size_t i = 0; i + 1 < kids.size(); i++)
        {
            uint32_t e = in.refs[kids[i]];
            copy(declare(e), var_type_of(e), f.returns[i], f.returnTypes[i], line(n));
        }
    }

    // x = e (declaring x if this is its first assignment), a[i] = e
    void Lowerer::assign_statement(uint32_t target, uint32_t value, int line)
    {
        if (tree.kind(target) == NodeKind::Name)
        {
            uint32_t e = in.refs[target];
            bool declares = in.entities[e].decl == target;
            if (declares && in.types[in.entities[e].type].kind == TypeKind::Array)
            {
                declare_array(e, value, line);
                return;
            }
            int v = expr(value);
            if (declares) declare(e);
            store(target, v, type_of(value), line);
            return;
        }
        store(target, expr(value), type_of(value), line);
    }

    // The value in slot value, of the given type, into a variable or an array's element
    void Lowerer::store(uint32_t target, int value, VarType type, int line)
    {
        if (tree.kind(target) == NodeKind::Index)
        {
            vector<uint32_t> kids = tree.children(target);
            int index = expr(kids[1]);
            array_access(ARRAY_WRITE, kids[0], index, value, this->line(kids[0]));
            return;
        }
        uint32_t e = in.refs[target];
        copy(variables[e].slot, var_type_of(e), value, type, line);
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
        const Function& f = functions[function];
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
            case NodeKind::Name:            return variables[in.refs[n]].slot;
            case NodeKind::Paren:           return expr(tree.children(n)[0]);
            case NodeKind::Unary:           return unary(n);
            case NodeKind::Binary:          return binary(n);
            case NodeKind::Call:            return call(n);
            case NodeKind::Index:           // a[i]
            {
                vector<uint32_t> kids = tree.children(n);
                int index = expr(kids[1]);
                int value = alloc_slot();
                array_access(ARRAY_READ, kids[0], index, value, line(kids[1]));
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

    // not x; -x, where a literal folds into its constant (-2, -0.5) and anything else is 0 - x
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
        int v = expr(x);
        VarType t = type_of(x) == TYPE_FLOAT || type_of(x) == TYPE_DOUBLE ? type_of(x) : TYPE_INT;
        int dst = new_slot(t);
        assign(t, dst, zero(t), OPERATOR_MINUS, v, line(n));
        return dst;
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
            return result;
        }
        VarType t;
        return arith(op, left, type_of(kids[0]), right, type_of(kids[1]), line(n), t);
    }

    // left op right: string + string concatenates; numbers are computed in the wider of their types (an int with an
    // int stays an int). a % b on ints is a - (a / b) * b, as in C. The result's type goes in t.
    int Lowerer::arith(TokenType op, int left, VarType lt, int right, VarType rt, int line, VarType& t)
    {
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

    // Falls through when condition n holds and jumps to fail when it does not. A comparison jumps on itself;
    // any other value is true when it is not zero.
    void Lowerer::condition(uint32_t n, InstructionNode* fail)
    {
        while (tree.kind(n) == NodeKind::Paren) n = tree.children(n)[0];
        ConditionalOperatorType op;
        if (tree.kind(n) == NodeKind::Binary && comparison(tree.token(n).kind, op))
        {
            vector<uint32_t> kids = tree.children(n);
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

    // f(args), print(x), input(), int(x) / float(x) / double(x). array(n) is lowered by the assignment it sets.
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
        const Entity& e = in.entities[in.refs[callee]];
        if (e.kind == EntityKind::Function) return call_function(n, e.decl, args);
        string_view f = names.text(e.name);
        if (f == "print") print(n, args[0]);
        if (f != "input") return 0;
        int slot = alloc_slot();
        add(IN, line(n))->input_inst.var_index = slot;
        return slot;
    }

    // The arguments, then the CALL: it copies them into the function's parameter slots and saves its int slots
    InstructionNode* Lowerer::call_instruction(uint32_t n, uint32_t fn, const vector<uint32_t>& args)
    {
        vector<int> slots;
        vector<VarType> types;
        for (uint32_t a : args)
        {
            slots.push_back(expr(a));
            types.push_back(type_of(a));
        }
        Function& f = lower_function(fn);
        InstructionNode* node = add(CALL, line(n));
        node->call_inst.ret_val_index = f.returns[0];
        node->call_inst.ret_type = f.returnTypes[0];
        node->call_inst.num_params = (int)f.params.size();
        if (!f.params.empty())
        {
            node->call_inst.param_slots = array_of(f.params);
            node->call_inst.param_types = array_of(f.paramTypes);
            node->call_inst.arg_val_slots = array_of(slots);
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

    // f(args) as a value: its first result, copied out of the function's return slot before another call reuses it
    int Lowerer::call_function(uint32_t n, uint32_t fn, const vector<uint32_t>& args)
    {
        InstructionNode* node = call_instruction(n, fn, args);
        VarType t = node->call_inst.ret_type;
        int result = new_slot(t);
        assign(t, result, node->call_inst.ret_val_index, OPERATOR_NONE, 0, line(n));
        return result;
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
}  // namespace

bool lowers(const ParseTree& tree, const CheckResult& checked)
{
    for (uint32_t n = 0; n < tree.nodes.size(); n++)
    {
        if (!covered(tree, checked, n)) return false;
    }
    return true;
}

InstructionNode* lower_tree(const ParseTree& tree, const CheckResult& checked)
{
    return Lowerer(tree, checked).run();
}
