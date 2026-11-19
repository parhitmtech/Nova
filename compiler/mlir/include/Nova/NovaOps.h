// The nova dialect's ops (nova::MatmulOp, ...), as TableGen writes them from NovaOps.td
#ifndef NOVA_NOVAOPS_H
#define NOVA_NOVAOPS_H

#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "Nova/NovaDialect.h"
#include "Nova/NovaTypes.h"

#define GET_OP_CLASSES
#include "Nova/NovaOps.h.inc"

#endif  // NOVA_NOVAOPS_H
