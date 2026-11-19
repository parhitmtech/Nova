// The nova dialect: TableGen's generated definitions, and the registration of its ops and types
#include "Nova/NovaDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

#include "Nova/NovaOps.h"
#include "Nova/NovaTypes.h"

using namespace mlir;
using namespace nova;

#include "Nova/NovaOpsDialect.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "Nova/NovaOpsTypes.cpp.inc"

// Called once when a context loads the dialect: it makes the dialect's ops and types known to that context, so the
// parser accepts nova.matmul and !nova.tensor<f64>
void NovaDialect::initialize()
{
    addOperations<
#define GET_OP_LIST
#include "Nova/NovaOps.cpp.inc"
        >();
    addTypes<
#define GET_TYPEDEF_LIST
#include "Nova/NovaOpsTypes.cpp.inc"
        >();
}
