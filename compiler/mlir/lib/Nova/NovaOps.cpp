// The nova dialect's ops: TableGen's generated definitions, and the checks ODS cannot express
#include "Nova/NovaOps.h"

#include "mlir/IR/Builders.h"  // the generated parsers build types through the parser's Builder
#include "mlir/IR/OpImplementation.h"

#define GET_OP_CLASSES
#include "Nova/NovaOps.cpp.inc"

using namespace mlir;

// A matrix product of floats. AllTypesMatch has already made both operands and the result one type, so only its
// element type is left to check.
LogicalResult nova::MatmulOp::verify()
{
    Type element = llvm::cast<nova::TensorType>(getResult().getType()).getElementType();
    if (!llvm::isa<FloatType>(element)) return emitOpError("needs tensors of floats, not of ") << element;
    return success();
}
