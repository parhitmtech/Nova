// Lowering the nova dialect toward LLVM. A program in nova, arith, scf and func becomes LLVM IR in two steps:
//
//   1. --convert-nova-to-runtime (here): each nova op becomes a call to a function of Nova's C runtime
//      (runtime/nova_runtime.c), so what is left is func, arith and scf, which MLIR itself lowers. @main returns an
//      i32 0, as C's main does, so the program links as a C program's entry point.
//   2. MLIR's own conversions: scf to cf (structured ifs and loops to branches between blocks), then arith, cf, func
//      and ub to the llvm dialect, which mlir-translate writes as LLVM IR. --nova-to-llvm runs both steps.
//
// Step 1 uses MLIR's dialect conversion: a ConversionTarget says which ops may remain (legal) and which must go
// (illegal: the whole nova dialect), and one pattern per op rewrites it. If an illegal op has no pattern that
// applies, the conversion fails and names it, so a program that uses what is not lowered yet stops with an error
// instead of producing a binary that is wrong. So far: print of a number or bool. Tensors come next, as a pointer to
// the runtime's tensor and a call per op.
#include "Nova/Passes.h"

#include "Nova/NovaOps.h"
#include "mlir/Conversion/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/DialectConversion.h"

namespace
{
// The runtime function name, in module m: declared (a func.func with no body, private) the first time it is used.
// The declaration goes at the start of the module, before the function being rewritten.
mlir::func::FuncOp runtime_function(mlir::PatternRewriter& rewriter, mlir::ModuleOp m, llvm::StringRef name,
                                    mlir::FunctionType type)
{
    if (auto f = m.lookupSymbol<mlir::func::FuncOp>(name)) return f;
    mlir::OpBuilder::InsertionGuard guard(rewriter);  // put the rewriter back where it was afterwards
    rewriter.setInsertionPointToStart(m.getBody());
    auto f = rewriter.create<mlir::func::FuncOp>(m.getLoc(), name, type);
    f.setPrivate();  // a declaration: the runtime library defines it
    return f;
}

// nova.print %x : i32 is func.call @nova_print_i32(%x), one runtime function per type, each printing as the
// interpreter does: an int (and a bool, as 1 or 0) with %d, a float or double with %f, then a newline.
struct PrintLowering : mlir::OpConversionPattern<nova::PrintOp>
{
    using OpConversionPattern::OpConversionPattern;

    mlir::LogicalResult matchAndRewrite(nova::PrintOp op, OpAdaptor adaptor,
                                        mlir::ConversionPatternRewriter& rewriter) const override
    {
        mlir::Value x = adaptor.getValue();
        mlir::Type t = x.getType();
        llvm::StringRef name;
        if (t.isInteger(1))  // a bool prints as the int 1 or 0
        {
            t = rewriter.getI32Type();
            x = rewriter.create<mlir::arith::ExtUIOp>(op.getLoc(), t, x);
        }
        if (t.isInteger(32)) name = "nova_print_i32";
        else if (t.isF32()) name = "nova_print_f32";
        else if (t.isF64()) name = "nova_print_f64";
        else return rewriter.notifyMatchFailure(op, "no runtime print for this type yet");

        auto type = rewriter.getFunctionType({t}, {});
        runtime_function(rewriter, op->getParentOfType<mlir::ModuleOp>(), name, type);
        rewriter.replaceOpWithNewOp<mlir::func::CallOp>(op, name, mlir::TypeRange{}, x);
        return mlir::success();
    }
};

// Nova's @main returns nothing; C's main returns an int, the program's exit status. So @main returns 0, from each
// of its returns. (Not a conversion pattern: it changes a function's type, not an op of the nova dialect.)
void make_entry(mlir::ModuleOp m)
{
    auto main = m.lookupSymbol<mlir::func::FuncOp>("main");
    if (!main || main.getNumResults() != 0 || main.isExternal()) return;
    mlir::OpBuilder b(m.getContext());
    main.setType(b.getFunctionType(main.getArgumentTypes(), {b.getI32Type()}));
    main.walk([&](mlir::func::ReturnOp ret) {
        b.setInsertionPoint(ret);
        mlir::Value zero = b.create<mlir::arith::ConstantOp>(ret.getLoc(), b.getI32IntegerAttr(0));
        b.create<mlir::func::ReturnOp>(ret.getLoc(), zero);
        ret.erase();
    });
}

struct ConvertNovaToRuntime : mlir::PassWrapper<ConvertNovaToRuntime, mlir::OperationPass<mlir::ModuleOp>>
{
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(ConvertNovaToRuntime)

    llvm::StringRef getArgument() const override { return "convert-nova-to-runtime"; }
    llvm::StringRef getDescription() const override
    {
        return "Lower nova ops to calls to Nova's C runtime, and make @main return an i32";
    }
    // The dialects the pass creates ops of, which the context must load before it runs
    void getDependentDialects(mlir::DialectRegistry& registry) const override
    {
        registry.insert<mlir::arith::ArithDialect, mlir::func::FuncDialect>();
    }

    void runOnOperation() override
    {
        mlir::ModuleOp m = getOperation();
        make_entry(m);

        mlir::ConversionTarget target(getContext());
        target.addIllegalDialect<nova::NovaDialect>();  // every nova op must go
        target.addLegalDialect<mlir::arith::ArithDialect, mlir::func::FuncDialect, mlir::scf::SCFDialect,
                               mlir::ub::UBDialect>();

        mlir::RewritePatternSet patterns(&getContext());
        patterns.add<PrintLowering>(&getContext());

        // Partial: ops the target does not name (none yet) may stay; an illegal op left over fails the pass
        if (mlir::failed(mlir::applyPartialConversion(m, target, std::move(patterns)))) signalPassFailure();
    }
};
}  // namespace

std::unique_ptr<mlir::Pass> nova::createConvertNovaToRuntimePass()
{
    return std::make_unique<ConvertNovaToRuntime>();
}

void nova::registerPasses()
{
    mlir::PassRegistration<ConvertNovaToRuntime>();

    // nova, arith, scf and func, all the way to the llvm dialect: the input mlir-translate --mlir-to-llvmir takes
    mlir::PassPipelineRegistration<>(
        "nova-to-llvm", "Lower a Nova program to the llvm dialect", [](mlir::OpPassManager& pm) {
            pm.addPass(nova::createConvertNovaToRuntimePass());
            pm.addPass(mlir::createConvertSCFToCFPass());          // scf.if, scf.while: blocks and branches
            pm.addPass(mlir::createArithToLLVMConversionPass());
            pm.addPass(mlir::createConvertControlFlowToLLVMPass());
            pm.addPass(mlir::createConvertFuncToLLVMPass());
            pm.addPass(mlir::createUBToLLVMConversionPass());
            pm.addPass(mlir::createReconcileUnrealizedCastsPass());  // the casts the conversions leave between them
        });
}
