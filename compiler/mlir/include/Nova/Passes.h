// Nova's passes: lowering the nova dialect toward LLVM (lib/Conversion)
#ifndef NOVA_PASSES_H
#define NOVA_PASSES_H

#include <memory>

#include "mlir/Pass/Pass.h"

namespace nova
{
// --convert-nova-to-runtime: nova ops become calls to Nova's C runtime (runtime/nova_runtime.c), and @main returns
// an i32, as C's main does
std::unique_ptr<mlir::Pass> createConvertNovaToRuntimePass();

// Registers the passes above and the --nova-to-llvm pipeline with nova-opt's command line
void registerPasses();
}  // namespace nova

#endif  // NOVA_PASSES_H
