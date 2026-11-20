// nova-lower: reads a Nova program, runs the front end (the scanner, the parser and the check pass, as the
// interpreter does), lowers the checked tree to MLIR (LowerTree.cpp) and prints the module. Its output is
// nova-opt's input: nova-lower prog.nova | nova-opt --canonicalize
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "lexer.h"

#include "Nova/LowerTree.h"
#include "Nova/NovaDialect.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        llvm::errs() << "usage: nova-lower file.nova\n";
        return 1;
    }
    std::ifstream file(argv[1]);
    if (!file)
    {
        llvm::errs() << "Error: cannot open '" << argv[1] << "'\n";
        return 1;
    }
    std::stringstream source;
    source << file.rdbuf();

    // The front end, as the interpreter's driver runs it (main.cc), without imports for now
    std::vector<LexToken> tokens = scan_program(source.str());
    ParseTree tree = build_parse_tree(tokens);
    std::vector<std::string> errors = tree.errors;
    CheckResult checked;
    if (errors.empty())
    {
        checked = check_tree(tree);
        errors = checked.errors;
    }
    for (const std::string& e : errors) llvm::errs() << e << "\n";
    if (!errors.empty()) return 1;

    // The dialects lowering writes: their ops can be made only once they are loaded
    mlir::MLIRContext context;
    context.loadDialect<nova::NovaDialect, mlir::arith::ArithDialect, mlir::func::FuncDialect,
                        mlir::scf::SCFDialect>();

    std::string error;
    mlir::OwningOpRef<mlir::ModuleOp> module = nova::lower_to_mlir(context, tree, checked, argv[1], error);
    if (!module)
    {
        llvm::errs() << error << "\n";
        return 1;
    }
    // A last check on lowering itself: every op's own verifier, and SSA's rule that a value is defined before it
    // is used. A failure here is a bug in LowerTree.cpp, not in the program.
    if (mlir::failed(mlir::verify(*module)))
    {
        llvm::errs() << "Error: lowering made invalid MLIR (a bug in nova-lower)\n";
        return 1;
    }
    module->print(llvm::outs());
    return 0;
}
