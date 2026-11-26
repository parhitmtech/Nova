// Lowering the checked tree to MLIR (LowerTree.h). It covers numbers and bools, variables, arithmetic, comparisons,
// print, if / elif / else and and / or (as scf.if), while, do-while and for (as scf.while) with break and
// continue, functions and calls (func), and tensors: made, combined, and read and written by element (nova).
// Anything else stops lowering with an error that names it, as lower.cc's unlowered() does.
//
// SSA. A Nova variable is assigned many times; an SSA value is defined once. Lowering keeps, for each variable, the
// SSA value it holds at this point of the program ('values'): a declaration or an assignment makes a new value and
// points the variable at it, and a use of the variable reads the value it points at. In straight-line code that is
// all SSA construction is.
//
// Where branches join, a variable can hold a different value depending on the branch taken. LLVM IR writes that
// as a phi; MLIR's structured scf.if writes it as the op's results: each branch ends in scf.yield with the values
// its variables hold, and after the scf.if the variables hold its results.
//
// A loop is a join too: at the top of an iteration a variable holds either its value from before the loop or the
// one the last iteration left. scf.while writes that as block arguments: the loop's operands are the first values,
// and each iteration ends by passing the next ones back to the top.
//
// break and continue. scf ops have no jumps: a region always runs to its end. So a loop with a break carries one
// more variable, a flag that break sets: the code after a statement that may set it runs only if it is not set
// (an scf.if), and the loop tests it with its condition. continue sets a second flag, cleared at the start of
// each iteration, which skips the rest of the body but not for's step. The flags live in 'values' like variables
// (flag()), so if and loop pass them out the same way.
//
// Functions. Each becomes a func.func, its parameters the entry block's arguments, and a call a func.call. return
// is a jump too: it sets the function's return flag and one slot per value returned; the code it skips tests the
// flag, every loop around it ends, and the func.return at the end returns the slots.
//
// Tensors. A Nova Tensor is a reference to doubles whose shape is known only at run time: !nova.tensor<f64>.
// zeros(...) is nova.zeros, its sizes cast to index (MLIR's type for sizes and subscripts); a @ b and matmul(a, b)
// are nova.matmul; print(t) is nova.print. A tensor variable or return slot with no value yet holds ub.poison:
// there is no tensor to start from, and no path reads it before something is assigned. t[i, j] is nova.get and
// t[i, j] = x nova.set: a write changes the tensor in place, so it is an effect, not a new SSA value for t.
// + - * / of two tensors are nova.add, sub, mul and div; a tensor and a number are nova.scale or nova.add_scalar,
// in the forms the interpreter computes (tensor_arith()), so both give the same doubles, rounding and all.
//
// Generic functions. A function with an untyped parameter has no one type for it, and a func.func needs one. The
// check pass has already made a copy of it for each set of argument types a call passes (check.h's Instance), with
// the copy's own types for every node and variable in its body, and which copy each call in it calls. So each copy
// is lowered as a func.func of its own, named as messages name it: @"scale<int, int>". That is monomorphization,
// as C++ does for templates: no type is left to decide at run time.
#include "Nova/LowerTree.h"

#include <cstdlib>
#include <string>

#include "check.h"  // the front end: the parse tree and the check pass's result
#include "lexer.h"  // token kinds

#include "Nova/NovaOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

// No 'using namespace mlir': the check pass has its own Type (check.h), so mlir::Type is always written out.
namespace
{
class Lowerer
{
    public:
        Lowerer(mlir::MLIRContext& context, const ParseTree& tree, const CheckResult& checked, const std::string& file)
            : builder(&context), tree(tree), in(checked), file(file) {}

        mlir::OwningOpRef<mlir::ModuleOp> run(std::string& error);

    private:
        mlir::OpBuilder builder;  // creates ops at its insertion point
        const ParseTree& tree;
        const CheckResult& in;
        std::string file;
        llvm::DenseMap<uint32_t, mlir::Value> values;  // by entity: the SSA value a variable holds at this point
        llvm::DenseMap<uint32_t, uint32_t> owner;      // by break or continue node: the loop it leaves
        llvm::SmallVector<mlir::Type> results;         // the result types of the function being lowered
        std::string error;                             // the first thing not lowered; once set, lowering stops
        uint32_t copy = NoInstance;                    // the generic function's copy being lowered, if one is

        bool ok() const { return error.empty(); }
        mlir::Value fail(uint32_t n, const std::string& what);
        mlir::Location loc(uint32_t n);
        mlir::Type type(TypeId t);  // null if lowering has no type for it yet
        mlir::Type type_of(uint32_t n) { return type(node_type(n)); }
        // The check pass's types; in a copy, the copy's own (check.h's Instance)
        TypeId node_type(uint32_t n);
        TypeId var_type(uint32_t entity);
        const Signature& signature(uint32_t fn, uint32_t c);  // function fn's, or its copy c's
        uint32_t called_copy(uint32_t call);                  // the copy a call calls, or NoInstance
        std::string symbol(uint32_t fn, uint32_t c);          // the func.func's name: main, scale, scale<int, int>
        bool lower_types(uint32_t n, const std::vector<TypeId>& from, llvm::SmallVector<mlir::Type>& to);
        mlir::Value initial(uint32_t n, mlir::Type t);  // what a variable holds before anything is assigned

        uint32_t first(uint32_t n) { return n + 1 - tree.nodes[n].size; }  // n's subtree is [first(n), n]
        // a loop's break flag and continue flag: keys in 'values' past the last entity, so never a variable's
        uint32_t flag(uint32_t loop, bool cont) { return uint32_t(in.entities.size()) + 2 * loop + cont; }
        // the function's return flag, then its return slots: keys past every loop's flags
        uint32_t ret_flag() { return uint32_t(in.entities.size() + 2 * tree.nodes.size()); }
        uint32_t ret_value(size_t k) { return ret_flag() + 1 + uint32_t(k); }
        bool is_flag(uint32_t key) { return key >= in.entities.size() && key <= ret_flag(); }
        bool contains(uint32_t n, NodeKind k);  // whether n's subtree has a node of kind k
        void find_loops(uint32_t n, uint32_t loop);

        void function(uint32_t d, uint32_t body, const std::vector<uint32_t>& params, const Signature& sig);

        void block(uint32_t n);
        void statements(const std::vector<uint32_t>& list, size_t i);
        void unless(const std::vector<uint32_t>& flags, const std::vector<uint32_t>& list, size_t i);
        void statement(uint32_t n);
        void assign(uint32_t target, mlir::Value value);

        std::vector<uint32_t> assigned(uint32_t from, uint32_t to);  // the live variables nodes [from, to) may assign
        std::vector<uint32_t> jumps(uint32_t n);                     // the flags n may set
        void if_chain(const std::vector<uint32_t>& parts, size_t i, const std::vector<uint32_t>& vars);
        void enter(mlir::Block* b);  // put the builder in a branch: before its scf.yield if it has one
        void yield(uint32_t n, const std::vector<uint32_t>& vars);
        llvm::SmallVector<mlir::Value> current(const std::vector<uint32_t>& vars);  // the values they hold here
        void loop(uint32_t n, uint32_t cond, uint32_t body, uint32_t step, bool bodyFirst);
        void iteration(mlir::Region& r, const std::vector<uint32_t>& vars, uint32_t n);
        mlir::Value condition(uint32_t cond);
        mlir::Value any(uint32_t n, const std::vector<uint32_t>& flags);  // flag or flag ...

        mlir::Value expr(uint32_t n);
        mlir::Value literal(uint32_t n);
        mlir::Value unary(uint32_t n);
        mlir::Value binary(uint32_t n);
        mlir::Value logical(uint32_t n);
        mlir::Value arithmetic(uint32_t n, TokenType op, mlir::Value left, mlir::Value right, mlir::Type t);
        mlir::Value compare(uint32_t n, TokenType op, mlir::Value left, mlir::Value right);
        mlir::Value tensor_arith(uint32_t n, TokenType op, mlir::Value left, mlir::Value right);  // a tensor on a side
        mlir::Value negate(uint32_t n, mlir::Value t);  // -t, a new tensor
        llvm::SmallVector<mlir::Value> call(uint32_t n);  // the values it gives: none, one, or several
        llvm::SmallVector<mlir::Value> builtin(uint32_t n, llvm::StringRef name, const std::vector<uint32_t>& args);
        bool subscript(uint32_t n, mlir::Value& t, llvm::SmallVector<mlir::Value>& at);  // t[i, j]: t, and i, j
        mlir::Value to_index(uint32_t n, mlir::Value v);  // an int as a size or subscript
        mlir::Value convert(uint32_t n, mlir::Value v, mlir::Type to);
        mlir::Value truth(uint32_t n, mlir::Value v);  // v != 0, as an i1
};

mlir::Value Lowerer::fail(uint32_t n, const std::string& what)
{
    if (ok())
    {
        error = "nova-lower cannot lower " + what + " yet";
        report_node(tree, n, DiagCode::NotLowered, error);
    }
    return nullptr;
}

// Every op records the Nova line it came from: MLIR's errors cite it, and --mlir-print-debuginfo prints it
mlir::Location Lowerer::loc(uint32_t n)
{
    PresumedLoc p = sourceManager.presumed(tree.token(n).loc);
    return mlir::FileLineColLoc::get(builder.getContext(), file, tree.token(n).line, p.column > 0 ? p.column : 1);
}

// Nova's types as MLIR's builtin ones (docs/DESIGN.md, C2): int is i32, as the interpreter's int is
mlir::Type Lowerer::type(TypeId t)
{
    switch (in.types[t].kind)
    {
        case TypeKind::Int:     return builder.getI32Type();
        case TypeKind::Float:   return builder.getF32Type();
        case TypeKind::Double:  return builder.getF64Type();
        case TypeKind::Bool:    return builder.getI1Type();
        case TypeKind::Tensor:  return nova::TensorType::get(builder.getContext(), builder.getF64Type());
        default:                return nullptr;
    }
}

// A copy's body has its own types; the check pass's nodeTypes holds only the last copy checked's
TypeId Lowerer::node_type(uint32_t n)
{
    if (copy == NoInstance) return in.nodeTypes[n];
    auto it = in.instances[copy].nodeTypes.find(n);
    return it == in.instances[copy].nodeTypes.end() ? TypeTable::Unknown : it->second;
}

// A copy's parameters and untyped locals have its types; a typed variable has the one it was declared with
TypeId Lowerer::var_type(uint32_t entity)
{
    if (copy != NoInstance)
    {
        auto it = in.instances[copy].varTypes.find(entity);
        if (it != in.instances[copy].varTypes.end()) return it->second;
    }
    return in.entities[entity].type;
}

const Signature& Lowerer::signature(uint32_t fn, uint32_t c)
{
    return c == NoInstance ? in.signatures.at(fn) : in.instances[c].signatures.at(fn);
}

// The copy a call to a generic function calls, as the check pass chose it: by the call node, in the body being
// lowered (a call in a copy is in the copy's table: sum_sq<int, int> calls sq<int>, sum_sq<float, float> sq<float>)
uint32_t Lowerer::called_copy(uint32_t call)
{
    const std::unordered_map<uint32_t, uint32_t>& calls = copy == NoInstance ? in.calls : in.instances[copy].calls;
    auto it = calls.find(call);
    return it == calls.end() ? NoInstance : it->second;
}

// A copy's name is the function's with its argument types, as the interpreter's messages write it. It is not a
// valid identifier, so MLIR prints it quoted: @"scale<int, int>".
std::string Lowerer::symbol(uint32_t fn, uint32_t c)
{
    if (tree.kind(fn) == NodeKind::Ignite) return "main";
    std::string s(tree.spelling(fn));
    if (c == NoInstance) return s;
    const std::vector<TypeId>& args = in.instances[c].args;
    for (size_t k = 0; k < args.size(); k++) s += (k == 0 ? "<" : ", ") + type_name(in, args[k]);
    return s + ">";
}

// A number starts at zero, as the interpreter's do. A tensor has no zero to start from (zeros of what shape?), so
// it starts as ub.poison: a value of the type that is not any tensor, and that nothing reads.
mlir::Value Lowerer::initial(uint32_t n, mlir::Type t)
{
    if (llvm::isa<nova::TensorType>(t)) return builder.create<mlir::ub::PoisonOp>(loc(n), t);
    return builder.create<mlir::arith::ConstantOp>(loc(n), builder.getZeroAttr(t));
}

// Several types at once, as for a function's parameters or results; false (and lowering stops) at the first one
// lowering has no type for
bool Lowerer::lower_types(uint32_t n, const std::vector<TypeId>& from, llvm::SmallVector<mlir::Type>& to)
{
    for (TypeId t : from)
    {
        mlir::Type m = type(t);
        if (!m)
        {
            fail(n, "a value of type " + type_name(in, t));
            return false;
        }
        to.push_back(m);
    }
    return true;
}

mlir::OwningOpRef<mlir::ModuleOp> Lowerer::run(std::string& errorOut)
{
    mlir::OwningOpRef<mlir::ModuleOp> module = mlir::ModuleOp::create(loc(tree.root()));
    builder.setInsertionPointToEnd(module->getBody());
    find_loops(tree.root(), 0);
    for (uint32_t d : tree.children(tree.root()))
    {
        std::vector<uint32_t> kids = tree.children(d);
        if (tree.kind(d) == NodeKind::Ignite) function(d, kids[0], {}, Signature{});  // func.func @main()
        else if (tree.kind(d) == NodeKind::FunctionDecl)  // children: ParamList, ReturnTypes if written, Block
        {
            if (!in.signatures.at(d).generic) function(d, kids.back(), tree.children(kids[0]), in.signatures.at(d));
            for (uint32_t c = 0; c < in.instances.size() && ok(); c++)  // a generic one: each copy the check pass
            {                                                            // made (one no call makes is never lowered)
                if (in.instances[c].decl != d) continue;
                copy = c;
                builder.setInsertionPointToEnd(module->getBody());
                function(d, kids.back(), tree.children(kids[0]), signature(d, c));
                copy = NoInstance;
            }
        }
        else fail(d, "this declaration");
        if (!ok()) break;
        builder.setInsertionPointToEnd(module->getBody());
    }
    if (!ok())
    {
        errorOut = error;
        return nullptr;
    }
    return module;
}

// A function (or ignite, as main): a func.func whose entry block's arguments are the parameters. Its variables
// start afresh: a function sees only its parameters. If it has a return, the return flag starts false; the return
// slots start at zero, so every path has a value to return, and the func.return at the end returns them.
void Lowerer::function(uint32_t d, uint32_t body, const std::vector<uint32_t>& params, const Signature& sig)
{
    llvm::SmallVector<mlir::Type> ins;
    results.clear();
    if (!lower_types(d, sig.params, ins) || !lower_types(d, sig.returns, results)) return;
    auto f = builder.create<mlir::func::FuncOp>(loc(d), symbol(d, copy), builder.getFunctionType(ins, results));
    mlir::Block* entry = f.addEntryBlock();
    builder.setInsertionPointToStart(entry);

    values.clear();
    for (size_t k = 0; k < params.size(); k++) values[in.refs[params[k]]] = entry->getArgument(k);
    if (contains(body, NodeKind::Return))
        values[ret_flag()] = builder.create<mlir::arith::ConstantOp>(loc(d), builder.getBoolAttr(false));
    for (size_t k = 0; k < results.size(); k++)
        values[ret_value(k)] = initial(d, results[k]);

    block(body);
    if (!ok()) return;
    llvm::SmallVector<mlir::Value> out;
    for (size_t k = 0; k < results.size(); k++) out.push_back(values[ret_value(k)]);
    builder.create<mlir::func::ReturnOp>(loc(d), out);
}

bool Lowerer::contains(uint32_t n, NodeKind k)
{
    for (uint32_t m = first(n); m <= n; m++)
        if (tree.kind(m) == k) return true;
    return false;
}

// The loop each break and continue leaves: the innermost one around it. (The check pass has made sure there is one.)
void Lowerer::find_loops(uint32_t n, uint32_t loop)
{
    NodeKind k = tree.kind(n);
    if (k == NodeKind::Break || k == NodeKind::Continue) owner[n] = loop;
    if (k == NodeKind::While || k == NodeKind::DoWhile || k == NodeKind::For) loop = n;
    for (uint32_t c : tree.children(n)) find_loops(c, loop);
}

void Lowerer::block(uint32_t n)
{
    statements(tree.children(n), 0);
}

// The statements from list[i] on. After one that may break or continue, the rest run only if it did not.
void Lowerer::statements(const std::vector<uint32_t>& list, size_t i)
{
    for (; i < list.size(); i++)
    {
        statement(list[i]);
        if (!ok()) return;
        std::vector<uint32_t> flags = jumps(list[i]);
        if (!flags.empty() && i + 1 < list.size())
        {
            unless(flags, list, i + 1);
            return;
        }
    }
}

// The statements from list[i] on, run only when none of 'flags' is set: scf.if (not (flag or flag ...)). Like an if
// with no else, it passes out the variables they may assign, and the flags they may set.
void Lowerer::unless(const std::vector<uint32_t>& flags, const std::vector<uint32_t>& list, size_t i)
{
    if (flags.empty()) return statements(list, i);
    uint32_t n = list[i];
    mlir::Value jumped = any(n, flags);
    mlir::Value one = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(true));
    mlir::Value go = builder.create<mlir::arith::XOrIOp>(loc(n), jumped, one);

    std::vector<uint32_t> vars = assigned(first(n), list.back() + 1);
    llvm::SmallVector<mlir::Type> types;
    for (uint32_t v : vars) types.push_back(values[v].getType());
    auto op = builder.create<mlir::scf::IfOp>(loc(n), types, go, !vars.empty());

    llvm::DenseMap<uint32_t, mlir::Value> before = values;
    enter(op.thenBlock());
    statements(list, i);
    yield(n, vars);
    values = before;
    if (!vars.empty() && ok())
    {
        enter(op.elseBlock());  // skipped: everything keeps its value
        yield(n, vars);
    }
    if (!ok()) return;
    builder.setInsertionPointAfter(op);
    for (size_t k = 0; k < vars.size(); k++) values[vars[k]] = op.getResult(k);
}

void Lowerer::statement(uint32_t n)
{
    std::vector<uint32_t> kids = tree.children(n);
    switch (tree.kind(n))
    {
        case NodeKind::VarDecl:  // int x = 10: x's first value; int x alone starts at zero
        {
            uint32_t var = in.refs[n];
            mlir::Type t = type(var_type(var));
            if (!t) { fail(n, "a variable of type " + type_name(in, var_type(var))); return; }
            mlir::Value v = kids.size() == 2 ? convert(n, expr(kids[1]), t) : initial(n, t);
            if (ok()) values[var] = v;
            return;
        }
        case NodeKind::MultiDecl:  // int q, int r = divmod(17, 5): children: VarDecl..., then the call
        {
            llvm::SmallVector<mlir::Value> got = call(kids.back());
            for (size_t k = 0; k + 1 < kids.size() && ok(); k++)
            {
                uint32_t var = in.refs[kids[k]];
                mlir::Type t = type(var_type(var));
                if (!t) { fail(kids[k], "a variable of type " + type_name(in, var_type(var))); return; }
                values[var] = convert(kids[k], got[k], t);
            }
            return;
        }
        case NodeKind::Return:  // children: the values. Fills the return slots and sets the return flag.
        {
            for (size_t k = 0; k < kids.size(); k++)
            {
                mlir::Value v = convert(kids[k], expr(kids[k]), results[k]);
                if (!ok()) return;
                values[ret_value(k)] = v;
            }
            values[ret_flag()] = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(true));
            return;
        }
        case NodeKind::Assign:  // x = value: x now holds a new value
            assign(kids[0], expr(kids[1]));
            return;
        case NodeKind::CompoundAssign:  // x += value is x = x + value
        {
            static const TokenType ops[] = {PLUS, MINUS, MULT, DIV};
            TokenType op = ops[tree.token(n).kind - PLUSEQ];  // PLUSEQ, MINUSEQ, MULTEQ, DIVEQ, in that order
            if (tree.kind(kids[0]) == NodeKind::Index)  // t[i, j] += x: get, then set; t and i, j are lowered once
            {
                mlir::Value t;
                llvm::SmallVector<mlir::Value> at;
                if (!subscript(kids[0], t, at)) return;
                mlir::Type element = llvm::cast<nova::TensorType>(t.getType()).getElementType();
                mlir::Value old = builder.create<nova::GetOp>(loc(n), element, t, at);
                mlir::Value now = arithmetic(n, op, old, convert(n, expr(kids[1]), element), element);
                if (ok()) builder.create<nova::SetOp>(loc(n), now, t, at);
                return;
            }
            mlir::Value old = expr(kids[0]);
            mlir::Value value = expr(kids[1]);
            if (!ok()) return;
            if (llvm::isa<nova::TensorType>(old.getType()))  // t += u, t *= k: a new tensor, which t then names
                assign(kids[0], tensor_arith(n, op, old, value));
            else assign(kids[0], arithmetic(n, op, old, convert(n, value, old.getType()), old.getType()));
            return;
        }
        case NodeKind::ExprStatement:
            expr(kids[0]);
            return;
        case NodeKind::Pass:
            return;
        case NodeKind::If:  // children: the condition, Block, then Elif... and Else
            if_chain(kids, 0, assigned(first(n), n));
            return;
        case NodeKind::While:    // children: the condition, Block
            loop(n, kids[0], kids[1], 0, false);
            return;
        case NodeKind::DoWhile:  // children: Block, the condition. The body runs before the first test.
            loop(n, kids[1], kids[0], 0, true);
            return;
        case NodeKind::For:      // children: the start, the condition, the step (each may be Empty), Block
            // the start runs once, in the enclosing scope: for (int i = 0; ...) declares an i that outlives the loop
            if (tree.kind(kids[0]) != NodeKind::Empty) statement(kids[0]);
            loop(n, kids[1], kids[3], kids[2], false);
            return;
        case NodeKind::Break:    // sets its loop's flag; what it skips tests the flag (statements(), loop())
        case NodeKind::Continue:
            values[flag(owner[n], tree.kind(n) == NodeKind::Continue)] =
                builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(true));
            return;
        default:                 fail(n, "this statement");  return;
    }
}

// target = value, for a variable: an element (a[i] = v) or a field (p.x = v) is memory, not an SSA value
void Lowerer::assign(uint32_t target, mlir::Value value)
{
    if (!ok()) return;
    if (tree.kind(target) == NodeKind::Index)  // t[i, j] = x changes the tensor t names, not which tensor that is
    {
        mlir::Value t;
        llvm::SmallVector<mlir::Value> at;
        if (!subscript(target, t, at)) return;
        mlir::Value v = convert(target, value, llvm::cast<nova::TensorType>(t.getType()).getElementType());
        if (ok()) builder.create<nova::SetOp>(loc(target), v, t, at);
        return;
    }
    if (tree.kind(target) != NodeKind::Name) { fail(target, "an assignment to a field"); return; }
    uint32_t var = in.refs[target];
    mlir::Value v = convert(target, value, type(var_type(var)));
    if (ok()) values[var] = v;
}

// The variables an if, a loop or the code after a break must pass out: those the nodes [from, to) may assign that
// are live here, so declared before them. One declared inside is gone after it (block scope), so it is not passed
// out. A break or continue assigns its loop's flag, which is live only inside that loop, and a return the return
// flag and slots. The tree is in postorder, so a subtree, or a run of sibling statements with their subtrees, is a
// range of nodes: one pass, in source order.
std::vector<uint32_t> Lowerer::assigned(uint32_t from, uint32_t to)
{
    std::vector<uint32_t> vars;
    llvm::DenseSet<uint32_t> seen;
    for (uint32_t m = from; m < to; m++)
    {
        llvm::SmallVector<uint32_t, 4> keys;
        NodeKind k = tree.kind(m);
        if (k == NodeKind::Break || k == NodeKind::Continue) keys.push_back(flag(owner[m], k == NodeKind::Continue));
        else if (k == NodeKind::Return)
        {
            keys.push_back(ret_flag());
            for (size_t r = 0; r < results.size(); r++) keys.push_back(ret_value(r));
        }
        else if (k == NodeKind::Assign || k == NodeKind::CompoundAssign)
        {
            uint32_t target = tree.children(m)[0];
            if (tree.kind(target) == NodeKind::Name) keys.push_back(in.refs[target]);
        }
        for (uint32_t key : keys)
            if (values.count(key) && seen.insert(key).second) vars.push_back(key);
    }
    return vars;
}

// The flags statement n may set: the return flag, and those of the loops around it, not of loops inside it (theirs
// are not live here)
std::vector<uint32_t> Lowerer::jumps(uint32_t n)
{
    std::vector<uint32_t> flags;
    for (uint32_t key : assigned(first(n), n + 1))
        if (is_flag(key)) flags.push_back(key);
    return flags;
}

// parts[i] is a condition and parts[i + 1] its block; after them come the Elif and Else nodes. One scf.if per
// condition: 'if a: A elif b: B else: C' is 'if a: A else: (if b: B else: C)'. The scf.if gives one result per
// variable in 'vars'; each branch yields what they hold at its end, and after it they hold its results.
void Lowerer::if_chain(const std::vector<uint32_t>& parts, size_t i, const std::vector<uint32_t>& vars)
{
    uint32_t cond = parts[i], body = parts[i + 1];
    mlir::Value c = convert(cond, expr(cond), builder.getI1Type());
    if (!ok()) return;

    llvm::SmallVector<mlir::Type> types;
    for (uint32_t v : vars) types.push_back(values[v].getType());
    size_t rest = i + 2;  // the next Elif or Else, if any
    bool otherwise = rest < parts.size() || !vars.empty();  // a branch with results needs both sides
    auto op = builder.create<mlir::scf::IfOp>(loc(cond), types, c, otherwise);

    llvm::DenseMap<uint32_t, mlir::Value> before = values;  // each side starts from the values before the if
    enter(op.thenBlock());
    block(body);
    yield(body, vars);
    values = before;
    if (otherwise && ok())
    {
        enter(op.elseBlock());
        if (rest < parts.size() && tree.kind(parts[rest]) == NodeKind::Elif)
        {
            std::vector<uint32_t> elif = tree.children(parts[rest]);  // the condition, Block
            std::vector<uint32_t> next = {elif[0], elif[1]};
            next.insert(next.end(), parts.begin() + rest + 1, parts.end());
            if_chain(next, 0, vars);
        }
        else if (rest < parts.size()) block(tree.children(parts[rest])[0]);  // Else: its Block
        yield(rest < parts.size() ? parts[rest] : cond, vars);
        values = before;
    }
    if (!ok()) return;
    builder.setInsertionPointAfter(op);
    for (size_t k = 0; k < vars.size(); k++) values[vars[k]] = op.getResult(k);
}

// An scf.if without results gets its scf.yield when it is made; one with results gets it from yield()
void Lowerer::enter(mlir::Block* b)
{
    if (!b->empty() && b->back().hasTrait<mlir::OpTrait::IsTerminator>()) builder.setInsertionPoint(&b->back());
    else builder.setInsertionPointToEnd(b);
}

void Lowerer::yield(uint32_t n, const std::vector<uint32_t>& vars)
{
    if (!ok() || vars.empty()) return;
    builder.create<mlir::scf::YieldOp>(loc(n), current(vars));
}

llvm::SmallVector<mlir::Value> Lowerer::current(const std::vector<uint32_t>& vars)
{
    llvm::SmallVector<mlir::Value> out;
    for (uint32_t v : vars) out.push_back(values[v]);
    return out;
}

// while (cond): body, with for's step after the body; with bodyFirst, do: body while (cond). One scf.while:
//
//   %results = scf.while (%a = <values before the loop>) {     the 'before' region: is there another iteration?
//     scf.condition(%cond) <values>                            false: they are the loop's results
//   } do {                                                     true: they go to the 'after' region
//   ^bb0(%b):
//     <body, step>
//     scf.yield <values>                                       back to the top, as the next iteration's %a
//   }
//
// The before region may do work ahead of the test, so a do-while's body goes there and its after region only
// passes the values back.
//
// With a break, the break flag starts false and is carried like a variable; the condition becomes
// 'brk ? false : cond' (an scf.if, so cond is not run after a break), and for's step runs only without a break.
// A return in the body ends the loop the same way, with the return flag. With a continue, the continue flag is
// false at the start of each body and only skips the rest of it.
void Lowerer::loop(uint32_t n, uint32_t cond, uint32_t body, uint32_t step, bool bodyFirst)
{
    uint32_t brk = flag(n, false), cont = flag(n, true);
    bool breaks = false, continues = false;
    for (uint32_t m = first(body); m < body; m++)
    {
        if (tree.kind(m) == NodeKind::Break && owner[m] == n) breaks = true;
        if (tree.kind(m) == NodeKind::Continue && owner[m] == n) continues = true;
    }
    std::vector<uint32_t> stops;  // the flags that end the loop early
    if (breaks) stops.push_back(brk);
    if (contains(body, NodeKind::Return)) stops.push_back(ret_flag());
    mlir::Value no;  // false: the flags' first value, and the condition once one of 'stops' is set
    if (continues || !stops.empty()) no = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(false));
    if (breaks) values[brk] = no;

    std::vector<uint32_t> vars = assigned(first(n), n);  // with brk, if there is a break
    llvm::SmallVector<mlir::Type> types;
    for (uint32_t v : vars) types.push_back(values[v].getType());
    auto op = builder.create<mlir::scf::WhileOp>(loc(n), types, current(vars));
    llvm::DenseMap<uint32_t, mlir::Value> before = values;

    auto run_body = [&]
    {
        if (continues) values[cont] = no;
        block(body);
        values.erase(cont);  // the continue flag lives in the body only
    };

    iteration(op.getBefore(), vars, n);
    if (bodyFirst) run_body();
    mlir::Value c;
    if (!stops.empty() && ok())
    {
        auto test = builder.create<mlir::scf::IfOp>(loc(n), mlir::TypeRange{builder.getI1Type()}, any(n, stops), true);
        builder.setInsertionPointToEnd(test.thenBlock());
        builder.create<mlir::scf::YieldOp>(loc(n), no);
        builder.setInsertionPointToEnd(test.elseBlock());
        mlir::Value more = condition(cond);
        if (ok()) builder.create<mlir::scf::YieldOp>(loc(n), more);
        builder.setInsertionPointAfter(test);
        c = test.getResult(0);
    }
    else c = condition(cond);
    if (!ok()) return;
    builder.create<mlir::scf::ConditionOp>(loc(n), c, current(vars));

    iteration(op.getAfter(), vars, n);
    if (!bodyFirst) run_body();
    if (step && tree.kind(step) != NodeKind::Empty)
        unless(stops, {step}, 0);
    if (!ok()) return;
    builder.create<mlir::scf::YieldOp>(loc(n), current(vars));

    values = before;
    builder.setInsertionPointAfter(op);
    for (size_t k = 0; k < vars.size(); k++) values[vars[k]] = op.getResult(k);
    values.erase(brk);  // the break flag ends with the loop
}

mlir::Value Lowerer::any(uint32_t n, const std::vector<uint32_t>& flags)
{
    mlir::Value v = values[flags[0]];
    for (size_t k = 1; k < flags.size(); k++) v = builder.create<mlir::arith::OrIOp>(loc(n), v, values[flags[k]]);
    return v;
}

// A loop's condition as an i1. A for with none, for (;;), tests true.
mlir::Value Lowerer::condition(uint32_t cond)
{
    if (tree.kind(cond) == NodeKind::Empty)
        return builder.create<mlir::arith::ConstantOp>(loc(cond), builder.getBoolAttr(true));
    return convert(cond, expr(cond), builder.getI1Type());
}

// One of scf.while's regions: a block with an argument per variable, which the variables hold inside it.
// createBlock also puts the builder at the block's end.
void Lowerer::iteration(mlir::Region& r, const std::vector<uint32_t>& vars, uint32_t n)
{
    llvm::SmallVector<mlir::Type> types;
    for (uint32_t v : vars) types.push_back(values[v].getType());
    llvm::SmallVector<mlir::Location> locs(vars.size(), loc(n));
    mlir::Block* b = builder.createBlock(&r, {}, types, locs);
    for (size_t k = 0; k < vars.size(); k++) values[vars[k]] = b->getArgument(k);
}

mlir::Value Lowerer::expr(uint32_t n)
{
    if (!ok()) return nullptr;
    switch (tree.kind(n))
    {
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:  return literal(n);
        case NodeKind::Name:         // the value the variable holds here
        {
            auto v = values.find(in.refs[n]);
            if (v == values.end()) return fail(n, "'" + std::string(tree.spelling(n)) + "' as a value");
            return v->second;
        }
        case NodeKind::Paren:        return expr(tree.children(n)[0]);
        case NodeKind::Unary:        return unary(n);
        case NodeKind::Binary:       return binary(n);
        case NodeKind::Index:        // t[i, j]: one element
        {
            mlir::Value t;
            llvm::SmallVector<mlir::Value> at;
            if (!subscript(n, t, at)) return nullptr;
            mlir::Type element = llvm::cast<nova::TensorType>(t.getType()).getElementType();
            return builder.create<nova::GetOp>(loc(n), element, t, at);
        }
        case NodeKind::Call:         // as a value: its one result (a call that gives none is a statement)
        {
            llvm::SmallVector<mlir::Value> got = call(n);
            return got.empty() ? nullptr : got[0];
        }
        default:                     return fail(n, "this expression");
    }
}

// 10, 2.5, true: an arith.constant of the literal's type
mlir::Value Lowerer::literal(uint32_t n)
{
    mlir::Type t = type_of(n);
    std::string text(tree.spelling(n));
    mlir::TypedAttr value;
    switch (tree.kind(n))
    {
        case NodeKind::IntLiteral:    value = builder.getIntegerAttr(t, std::strtoll(text.c_str(), nullptr, 10)); break;
        case NodeKind::FloatLiteral:  value = builder.getFloatAttr(t, std::strtod(text.c_str(), nullptr)); break;
        default:                      value = builder.getIntegerAttr(t, tree.token(n).kind == TRUE); break;
    }
    return builder.create<mlir::arith::ConstantOp>(loc(n), value);
}

// not x; -x. arith has no negation for integers (0 - x) and no 'not' (x xor true).
mlir::Value Lowerer::unary(uint32_t n)
{
    mlir::Value x = expr(tree.children(n)[0]);
    if (!ok()) return nullptr;
    if (llvm::isa<nova::TensorType>(x.getType()) && tree.token(n).kind != NOT) return negate(n, x);
    if (tree.token(n).kind == NOT)
    {
        mlir::Value one = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(true));
        return builder.create<mlir::arith::XOrIOp>(loc(n), truth(n, x), one);
    }
    if (llvm::isa<mlir::FloatType>(x.getType())) return builder.create<mlir::arith::NegFOp>(loc(n), x);
    mlir::Value zero = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getZeroAttr(x.getType()));
    return builder.create<mlir::arith::SubIOp>(loc(n), zero, x);
}

mlir::Value Lowerer::binary(uint32_t n)
{
    TokenType op = tree.token(n).kind;
    if (op == AND || op == OR) return logical(n);
    std::vector<uint32_t> kids = tree.children(n);
    mlir::Value left = expr(kids[0]);
    mlir::Value right = expr(kids[1]);
    if (!ok()) return nullptr;
    if (op == AT) return builder.create<nova::MatmulOp>(loc(n), left.getType(), left, right);
    bool tensors = llvm::isa<nova::TensorType>(left.getType()) || llvm::isa<nova::TensorType>(right.getType());
    switch (op)
    {
        case EQEQ: case NOTEQUAL: case LESS: case GREATER: case LESSEQ: case GREATEREQ:
            if (tensors) return fail(n, "a comparison of tensors");
            return compare(n, op, left, right);
        default:
        {
            if (tensors) return tensor_arith(n, op, left, right);
            mlir::Type t = type_of(n);  // the check pass's: int with int, or the wider of float and double
            if (!t) return fail(n, "arithmetic on " + type_name(in, node_type(n)));
            return arithmetic(n, op, convert(n, left, t), convert(n, right, t), t);
        }
    }
}

// a and b; a or b. The right side runs only when the left does not decide (short-circuit), so it is control flow,
// not arith.andi: 'a and b' is scf.if a { yield b } else { yield false }, and 'a or b' is scf.if a { yield true }
// else { yield b }.
mlir::Value Lowerer::logical(uint32_t n)
{
    bool isAnd = tree.token(n).kind == AND;
    std::vector<uint32_t> kids = tree.children(n);
    mlir::Value left = convert(kids[0], expr(kids[0]), builder.getI1Type());
    if (!ok()) return nullptr;
    auto op = builder.create<mlir::scf::IfOp>(loc(n), mlir::TypeRange{builder.getI1Type()}, left, true);

    builder.setInsertionPointToEnd(isAnd ? op.thenBlock() : op.elseBlock());  // where the right side decides
    mlir::Value right = convert(kids[1], expr(kids[1]), builder.getI1Type());
    if (!ok()) return nullptr;
    builder.create<mlir::scf::YieldOp>(loc(n), right);

    builder.setInsertionPointToEnd(isAnd ? op.elseBlock() : op.thenBlock());  // where the left side decides
    mlir::Value decided = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getBoolAttr(!isAnd));
    builder.create<mlir::scf::YieldOp>(loc(n), decided);

    builder.setInsertionPointAfter(op);
    return op.getResult(0);
}

// One arith op per operator and kind of number. Integer division and remainder are signed (divsi, remsi), as C's.
mlir::Value Lowerer::arithmetic(uint32_t n, TokenType op, mlir::Value l, mlir::Value r, mlir::Type t)
{
    if (!ok()) return nullptr;
    mlir::Location at = loc(n);
    if (llvm::isa<mlir::FloatType>(t))
    {
        switch (op)
        {
            case PLUS:   return builder.create<mlir::arith::AddFOp>(at, l, r);
            case MINUS:  return builder.create<mlir::arith::SubFOp>(at, l, r);
            case MULT:   return builder.create<mlir::arith::MulFOp>(at, l, r);
            case DIV:    return builder.create<mlir::arith::DivFOp>(at, l, r);
            default:     return fail(n, "this operator on floats");
        }
    }
    switch (op)
    {
        case PLUS:   return builder.create<mlir::arith::AddIOp>(at, l, r);
        case MINUS:  return builder.create<mlir::arith::SubIOp>(at, l, r);
        case MULT:   return builder.create<mlir::arith::MulIOp>(at, l, r);
        case DIV:    return builder.create<mlir::arith::DivSIOp>(at, l, r);
        case MOD:    return builder.create<mlir::arith::RemSIOp>(at, l, r);
        default:     return fail(n, "this operator");
    }
}

// a < b and the rest give an i1. Both sides become one type first: the wider float if either is one, else i32.
// Floats compare 'ordered' (olt, ...): false when either side is NaN, as C's < is.
mlir::Value Lowerer::compare(uint32_t n, TokenType op, mlir::Value l, mlir::Value r)
{
    mlir::Type lt = l.getType(), rt = r.getType();
    mlir::Location at = loc(n);
    if (llvm::isa<mlir::FloatType>(lt) || llvm::isa<mlir::FloatType>(rt))
    {
        mlir::Type t = (lt.isF64() || rt.isF64()) ? builder.getF64Type() : builder.getF32Type();
        mlir::arith::CmpFPredicate p;
        switch (op)
        {
            case EQEQ:      p = mlir::arith::CmpFPredicate::OEQ; break;
            case NOTEQUAL:  p = mlir::arith::CmpFPredicate::UNE; break;  // NaN != x is true
            case LESS:      p = mlir::arith::CmpFPredicate::OLT; break;
            case GREATER:   p = mlir::arith::CmpFPredicate::OGT; break;
            case LESSEQ:    p = mlir::arith::CmpFPredicate::OLE; break;
            default:        p = mlir::arith::CmpFPredicate::OGE; break;
        }
        return builder.create<mlir::arith::CmpFOp>(at, p, convert(n, l, t), convert(n, r, t));
    }
    mlir::Type t = lt == rt ? lt : builder.getI32Type();  // a bool with an int: the bool widens to 0 or 1
    mlir::arith::CmpIPredicate p;
    switch (op)
    {
        case EQEQ:      p = mlir::arith::CmpIPredicate::eq;  break;
        case NOTEQUAL:  p = mlir::arith::CmpIPredicate::ne;  break;
        case LESS:      p = mlir::arith::CmpIPredicate::slt; break;
        case GREATER:   p = mlir::arith::CmpIPredicate::sgt; break;
        case LESSEQ:    p = mlir::arith::CmpIPredicate::sle; break;
        default:        p = mlir::arith::CmpIPredicate::sge; break;
    }
    return builder.create<mlir::arith::CmpIOp>(at, p, convert(n, l, t), convert(n, r, t));
}

// + - * / with a tensor on a side; the check pass has allowed only these: two tensors, t * k and k * t, t / k, and
// t + k, k + t, t - k and k - t. Two tensors are an element-wise op. With a number, the forms are the interpreter's
// (lower.cc's tensor_binary): t / k is t * (1 / k), t - k is t + -k, and k - t is -t + k. Dividing each element by
// k would round differently from multiplying it by 1 / k, and the two lowerings are to give the same doubles.
mlir::Value Lowerer::tensor_arith(uint32_t n, TokenType op, mlir::Value l, mlir::Value r)
{
    if (!ok()) return nullptr;
    mlir::Location at = loc(n);
    bool lT = llvm::isa<nova::TensorType>(l.getType()), rT = llvm::isa<nova::TensorType>(r.getType());
    if (lT && rT)
    {
        switch (op)
        {
            case PLUS:   return builder.create<nova::AddOp>(at, l.getType(), l, r);
            case MINUS:  return builder.create<nova::SubOp>(at, l.getType(), l, r);
            case MULT:   return builder.create<nova::MulOp>(at, l.getType(), l, r);
            default:     return builder.create<nova::DivOp>(at, l.getType(), l, r);
        }
    }
    mlir::Value t = lT ? l : r;
    mlir::Type element = llvm::cast<nova::TensorType>(t.getType()).getElementType();
    mlir::Value k = convert(n, lT ? r : l, element);  // the number, as the tensor's element type
    if (!ok()) return nullptr;
    if (op == MULT) return builder.create<nova::ScaleOp>(at, t.getType(), t, k);
    if (op == DIV)
    {
        mlir::Value one = builder.create<mlir::arith::ConstantOp>(at, builder.getFloatAttr(element, 1.0));
        mlir::Value inverse = builder.create<mlir::arith::DivFOp>(at, one, k);
        return builder.create<nova::ScaleOp>(at, t.getType(), t, inverse);
    }
    if (op == MINUS && lT) k = builder.create<mlir::arith::NegFOp>(at, k);  // t - k: t + -k
    if (op == MINUS && rT) t = negate(n, t);                                 // k - t: -t + k
    return builder.create<nova::AddScalarOp>(at, t.getType(), t, k);
}

// -t is t * -1, a new tensor, as the interpreter computes it
mlir::Value Lowerer::negate(uint32_t n, mlir::Value t)
{
    mlir::Type element = llvm::cast<nova::TensorType>(t.getType()).getElementType();
    mlir::Value minusOne = builder.create<mlir::arith::ConstantOp>(loc(n), builder.getFloatAttr(element, -1.0));
    return builder.create<nova::ScaleOp>(loc(n), t.getType(), t, minusOne);
}

// A call to one of the program's functions: func.call, each argument converted to its parameter's type. A call to
// a generic function calls the copy the check pass chose for it, by that copy's name. A built-in is its nova op
// (builtin()).
llvm::SmallVector<mlir::Value> Lowerer::call(uint32_t n)
{
    std::vector<uint32_t> kids = tree.children(n);
    uint32_t callee = kids[0];
    uint32_t e = tree.kind(callee) == NodeKind::Name ? in.refs[callee] : NoEntity;
    EntityKind kind = e == NoEntity ? EntityKind::None : in.entities[e].kind;
    if (kind == EntityKind::Builtin)
        return builtin(n, tree.spelling(callee), std::vector<uint32_t>(kids.begin() + 1, kids.end()));
    if (kind != EntityKind::Function)
    {
        fail(n, "a call to '" + std::string(tree.spelling(callee)) + "'");
        return {};
    }

    uint32_t fn = in.entities[e].decl, c = called_copy(n);
    const Signature& sig = signature(fn, c);
    llvm::SmallVector<mlir::Type> params, returns;
    if (!lower_types(n, sig.params, params) || !lower_types(n, sig.returns, returns)) return {};
    llvm::SmallVector<mlir::Value> args;
    for (size_t k = 1; k < kids.size(); k++) args.push_back(convert(kids[k], expr(kids[k]), params[k - 1]));
    if (!ok()) return {};
    auto op = builder.create<mlir::func::CallOp>(loc(n), symbol(fn, c), returns, args);
    return llvm::SmallVector<mlir::Value>(op.getResults());
}

// The built-ins the nova dialect has ops for: print and tensor_print, zeros and ones (tensor_zeros, tensor_ones),
// matmul, tensor_add, _sub, _mul, _div and _scale, tensor_get and tensor_set. The check pass has checked their
// arguments' number and types; the rest of the built-ins come with their ops.
llvm::SmallVector<mlir::Value> Lowerer::builtin(uint32_t n, llvm::StringRef name, const std::vector<uint32_t>& args)
{
    llvm::SmallVector<mlir::Value> vals;
    for (uint32_t a : args) vals.push_back(expr(a));
    if (!ok()) return {};
    if (name == "print" || name == "tensor_print")
    {
        builder.create<nova::PrintOp>(loc(n), vals[0]);
        return {};  // print gives nothing
    }
    bool zeros = name == "zeros" || name == "tensor_zeros";
    if (zeros || name == "ones" || name == "tensor_ones")  // zeros(rows, cols), or one size per dimension
    {
        llvm::SmallVector<mlir::Value> sizes;
        for (size_t k = 0; k < vals.size(); k++) sizes.push_back(to_index(args[k], vals[k]));
        if (!ok()) return {};
        mlir::Type t = type(TypeTable::Tensor);
        if (zeros) return {builder.create<nova::ZerosOp>(loc(n), t, sizes)};
        return {builder.create<nova::OnesOp>(loc(n), t, sizes)};
    }
    if (name == "matmul") return {builder.create<nova::MatmulOp>(loc(n), vals[0].getType(), vals[0], vals[1])};
    if (name == "tensor_add" || name == "tensor_sub" || name == "tensor_mul" || name == "tensor_div" ||
        name == "tensor_scale")  // the operators' ops: tensor_add(a, b) is a + b, tensor_scale(t, k) is t * k
    {
        TokenType op = name == "tensor_add" ? PLUS : name == "tensor_sub" ? MINUS : name == "tensor_div" ? DIV : MULT;
        return {tensor_arith(n, op, vals[0], vals[1])};
    }
    if (name == "tensor_get" || name == "tensor_set")  // tensor_get(t, i, j); tensor_set(t, i, j, x)
    {
        bool set = name == "tensor_set";
        llvm::SmallVector<mlir::Value> at;
        for (size_t k = 1; k + set < vals.size(); k++) at.push_back(to_index(args[k], vals[k]));
        if (!ok()) return {};
        mlir::Type element = llvm::cast<nova::TensorType>(vals[0].getType()).getElementType();
        if (!set) return {builder.create<nova::GetOp>(loc(n), element, vals[0], at)};
        mlir::Value v = convert(args.back(), vals.back(), element);
        if (ok()) builder.create<nova::SetOp>(loc(n), v, vals[0], at);
        return {};
    }
    fail(n, "a call to '" + name.str() + "'");
    return {};
}

// t[i, j], to read or write: the tensor, then each subscript as an index. Only a tensor's elements so far.
bool Lowerer::subscript(uint32_t n, mlir::Value& t, llvm::SmallVector<mlir::Value>& at)
{
    std::vector<uint32_t> kids = tree.children(n);  // the tensor, then the subscripts
    t = expr(kids[0]);
    if (!ok()) return false;
    if (!llvm::isa<nova::TensorType>(t.getType())) { fail(n, "an element of this type"); return false; }
    for (size_t k = 1; k < kids.size(); k++) at.push_back(to_index(kids[k], expr(kids[k])));
    return ok();
}

// Sizes and subscripts are index, MLIR's type for them (as wide as a pointer, chosen when lowering to LLVM)
mlir::Value Lowerer::to_index(uint32_t n, mlir::Value v)
{
    if (!ok()) return nullptr;
    if (!v.getType().isInteger(32)) return fail(n, "a size or subscript that is not an int");
    return builder.create<mlir::arith::IndexCastOp>(loc(n), builder.getIndexType(), v);
}

// The conversions the check pass allows without a cast: a float to a double, a bool to an int, an int to a float
// (in a comparison), and a number to a bool where a condition is wanted
mlir::Value Lowerer::convert(uint32_t n, mlir::Value v, mlir::Type to)
{
    if (!ok()) return nullptr;
    mlir::Type from = v.getType();
    if (from == to) return v;
    mlir::Location at = loc(n);
    if (to.isInteger(1)) return truth(n, v);
    if (from.isInteger(1) && to.isInteger(32)) return builder.create<mlir::arith::ExtUIOp>(at, to, v);
    if (from.isF32() && to.isF64()) return builder.create<mlir::arith::ExtFOp>(at, to, v);
    if (from.isInteger(32) && llvm::isa<mlir::FloatType>(to)) return builder.create<mlir::arith::SIToFPOp>(at, to, v);
    return fail(n, "a conversion between these types");
}

mlir::Value Lowerer::truth(uint32_t n, mlir::Value v)
{
    mlir::Type t = v.getType();
    if (t.isInteger(1)) return v;
    if (!t.isIntOrFloat()) return fail(n, "a tensor as a condition");
    mlir::Location at = loc(n);
    mlir::Value zero = builder.create<mlir::arith::ConstantOp>(at, builder.getZeroAttr(t));
    if (llvm::isa<mlir::FloatType>(t))
        return builder.create<mlir::arith::CmpFOp>(at, mlir::arith::CmpFPredicate::UNE, v, zero);
    return builder.create<mlir::arith::CmpIOp>(at, mlir::arith::CmpIPredicate::ne, v, zero);
}
}  // namespace

mlir::OwningOpRef<mlir::ModuleOp> nova::lower_to_mlir(mlir::MLIRContext& context, const ParseTree& tree,
                                                      const CheckResult& checked, const std::string& file,
                                                      std::string& error)
{
    return Lowerer(context, tree, checked, file).run(error);
}
