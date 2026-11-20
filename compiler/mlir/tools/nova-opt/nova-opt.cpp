// nova-opt: Nova's version of mlir-opt. It reads a .mlir file, runs the passes named on the command line
// (--canonicalize, --cse, and later Nova's own), and prints the result; the dialect tests are written against it.
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/UB/IR/UBOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "mlir/Transforms/Passes.h"

#include "Nova/NovaDialect.h"

int main(int argc, char** argv)
{
    mlir::registerTransformsPasses();  // the general passes: canonicalize, cse, loop-invariant code motion, ...

    // The dialects a .mlir file may use: nova, and the upstream ones Nova lowers to (docs/DESIGN.md, C2)
    mlir::DialectRegistry registry;
    registry.insert<nova::NovaDialect, mlir::arith::ArithDialect, mlir::func::FuncDialect, mlir::scf::SCFDialect,
                    mlir::ub::UBDialect>();

    return mlir::asMainReturnCode(mlir::MlirOptMain(argc, argv, "Nova's MLIR driver\n", registry));
}
