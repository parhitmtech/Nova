// Lowering the checked tree to MLIR (LowerTree.h). It covers the ignite block's numbers and bools, variables,
// arithmetic, comparisons, print, and if / elif / else and and / or (as scf.if). Anything else stops lowering with
// an error that names it, as lower.cc's unlowered() does.
//
// SSA. A Nova variable is assigned many times; an SSA value is defined once. Lowering keeps, for each variable, the
// SSA value it holds at this point of the program ('values'): a declaration or an assignment makes a new value and
// points the variable at it, and a use of the variable reads the value it points at. In straight-line code that is
// all SSA construction is.
//
// Where branches join, a variable can hold a different value depending on the branch taken. LLVM IR writes that
// as a phi; MLIR's structured scf.if writes it as the op's results: each branch ends in scf.yield with the values
// its variables hold, and after the scf.if the variables hold its results.
#include "Nova/LowerTree.h"

#include <cstdlib>
#include <string>

#include "check.h"  // the front end: the parse tree and the check pass's result
#include "lexer.h"  // token kinds

#include "Nova/NovaOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
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
        std::string error;                             // the first thing not lowered; once set, lowering stops

        bool ok() const { return error.empty(); }
        mlir::Value fail(uint32_t n, const std::string& what);
        mlir::Location loc(uint32_t n);
        mlir::Type type(TypeId t);  // null if lowering has no type for it yet
        mlir::Type type_of(uint32_t n) { return type(in.nodeTypes[n]); }

        void block(uint32_t n);
        void statement(uint32_t n);
        void assign(uint32_t target, mlir::Value value);

        std::vector<uint32_t> assigned(uint32_t n);  // the variables declared before n that n may assign
        void if_chain(const std::vector<uint32_t>& parts, size_t i, const std::vector<uint32_t>& vars);
        void enter(mlir::Block* b);  // put the builder in a branch: before its scf.yield if it has one
        void yield(uint32_t n, const std::vector<uint32_t>& vars);

        mlir::Value expr(uint32_t n);
        mlir::Value literal(uint32_t n);
        mlir::Value unary(uint32_t n);
        mlir::Value binary(uint32_t n);
        mlir::Value logical(uint32_t n);
        mlir::Value arithmetic(uint32_t n, TokenType op, mlir::Value left, mlir::Value right, mlir::Type t);
        mlir::Value compare(uint32_t n, TokenType op, mlir::Value left, mlir::Value right);
        mlir::Value call(uint32_t n);
        mlir::Value convert(uint32_t n, mlir::Value v, mlir::Type to);
        mlir::Value truth(uint32_t n, mlir::Value v);  // v != 0, as an i1
};

mlir::Value Lowerer::fail(uint32_t n, const std::string& what)
{
    if (ok()) error = "Error at line " + std::to_string(tree.token(n).line) + ": nova-lower cannot lower " + what + " yet";
    return nullptr;
}

// Every op records the Nova line it came from: MLIR's errors cite it, and --mlir-print-debuginfo prints it
mlir::Location Lowerer::loc(uint32_t n)
{
    return mlir::FileLineColLoc::get(builder.getContext(), file, tree.token(n).line, 1);
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
        default:                return nullptr;
    }
}

mlir::OwningOpRef<mlir::ModuleOp> Lowerer::run(std::string& errorOut)
{
    mlir::OwningOpRef<mlir::ModuleOp> module = mlir::ModuleOp::create(loc(tree.root()));
    builder.setInsertionPointToEnd(module->getBody());
    for (uint32_t d : tree.children(tree.root()))
    {
        if (tree.kind(d) != NodeKind::Ignite)
        {
            fail(d, "functions, structs and classes");
            break;
        }
        // ignite: becomes func.func @main(), its block the function's body
        auto main = builder.create<mlir::func::FuncOp>(loc(d), "main", builder.getFunctionType({}, {}));
        builder.setInsertionPointToStart(main.addEntryBlock());
        block(tree.children(d)[0]);
        builder.create<mlir::func::ReturnOp>(loc(d));
        builder.setInsertionPointToEnd(module->getBody());
    }
    if (!ok())
    {
        errorOut = error;
        return nullptr;
    }
    return module;
}

void Lowerer::block(uint32_t n)
{
    for (uint32_t s : tree.children(n))
    {
        statement(s);
        if (!ok()) return;
    }
}

void Lowerer::statement(uint32_t n)
{
    std::vector<uint32_t> kids = tree.children(n);
    switch (tree.kind(n))
    {
        case NodeKind::VarDecl:  // int x = 10: x's first value; int x alone starts at zero
        {
            uint32_t var = in.refs[n];
            mlir::Type t = type(in.entities[var].type);
            if (!t) { fail(n, "a variable of type " + type_name(in, in.entities[var].type)); return; }
            mlir::Value v = kids.size() == 2 ? convert(n, expr(kids[1]), t)
                                             : builder.create<mlir::arith::ConstantOp>(loc(n), builder.getZeroAttr(t));
            if (ok()) values[var] = v;
            return;
        }
        case NodeKind::Assign:  // x = value: x now holds a new value
            assign(kids[0], expr(kids[1]));
            return;
        case NodeKind::CompoundAssign:  // x += value is x = x + value
        {
            static const TokenType ops[] = {PLUS, MINUS, MULT, DIV};
            TokenType op = ops[tree.token(n).kind - PLUSEQ];  // PLUSEQ, MINUSEQ, MULTEQ, DIVEQ, in that order
            mlir::Value old = expr(kids[0]);
            mlir::Value value = expr(kids[1]);
            if (ok()) assign(kids[0], arithmetic(n, op, old, convert(n, value, old.getType()), old.getType()));
            return;
        }
        case NodeKind::ExprStatement:
            expr(kids[0]);
            return;
        case NodeKind::Pass:
            return;
        case NodeKind::If:  // children: the condition, Block, then Elif... and Else
            if_chain(kids, 0, assigned(n));
            return;
        case NodeKind::While:    fail(n, "a while loop");     return;
        case NodeKind::For:      fail(n, "a for loop");       return;
        default:                 fail(n, "this statement");   return;
    }
}

// target = value, for a variable: an element (a[i] = v) or a field (p.x = v) is memory, not an SSA value
void Lowerer::assign(uint32_t target, mlir::Value value)
{
    if (!ok()) return;
    if (tree.kind(target) != NodeKind::Name) { fail(target, "an assignment to an element or a field"); return; }
    uint32_t var = in.refs[target];
    mlir::Value v = convert(target, value, type(in.entities[var].type));
    if (ok()) values[var] = v;
}

// The variables an if (or, later, a loop) must pass out: those it may assign that were declared before it. A
// variable declared inside a branch is gone after it (block scope), so it is not passed out. The tree is in
// postorder, so n's subtree is the size(n) nodes ending at n: one pass over them, in source order.
std::vector<uint32_t> Lowerer::assigned(uint32_t n)
{
    std::vector<uint32_t> vars;
    llvm::DenseSet<uint32_t> seen;
    for (uint32_t m = n + 1 - tree.nodes[n].size; m < n; m++)
    {
        if (tree.kind(m) != NodeKind::Assign && tree.kind(m) != NodeKind::CompoundAssign) continue;
        uint32_t target = tree.children(m)[0];
        if (tree.kind(target) != NodeKind::Name) continue;
        uint32_t var = in.refs[target];
        if (values.count(var) && seen.insert(var).second) vars.push_back(var);
    }
    return vars;
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
    llvm::SmallVector<mlir::Value> out;
    for (uint32_t v : vars) out.push_back(values[v]);
    builder.create<mlir::scf::YieldOp>(loc(n), out);
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
        case NodeKind::Call:         return call(n);
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
    switch (op)
    {
        case EQEQ: case NOTEQUAL: case LESS: case GREATER: case LESSEQ: case GREATEREQ:
            return compare(n, op, left, right);
        default:
        {
            mlir::Type t = type_of(n);  // the check pass's: int with int, or the wider of float and double
            if (!t) return fail(n, "arithmetic on " + type_name(in, in.nodeTypes[n]));
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

// print(x): nova.print. The other built-ins come with their slices.
mlir::Value Lowerer::call(uint32_t n)
{
    std::vector<uint32_t> kids = tree.children(n);
    uint32_t callee = kids[0];
    bool print = tree.kind(callee) == NodeKind::Name && in.refs[callee] != NoEntity &&
                 in.entities[in.refs[callee]].kind == EntityKind::Builtin && tree.spelling(callee) == "print";
    if (!print) return fail(n, "a call to '" + std::string(tree.spelling(callee)) + "'");
    mlir::Value v = expr(kids[1]);
    if (!ok()) return nullptr;
    builder.create<nova::PrintOp>(loc(n), v);
    return nullptr;  // print gives nothing
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
