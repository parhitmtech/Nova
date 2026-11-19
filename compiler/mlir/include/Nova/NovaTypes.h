// The nova dialect's types (nova::TensorType, ...), as TableGen writes them from NovaOps.td. Not to be confused with
// MLIR's own mlir::TensorType: refer to Nova's as nova::TensorType.
#ifndef NOVA_NOVATYPES_H
#define NOVA_NOVATYPES_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Types.h"

#define GET_TYPEDEF_CLASSES
#include "Nova/NovaOpsTypes.h.inc"

#endif  // NOVA_NOVATYPES_H
