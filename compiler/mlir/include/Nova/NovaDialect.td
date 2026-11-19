// The nova dialect: what every op and type of it shares. TableGen's ODS (Operation Definition Specification)
// describes them declaratively, and mlir-tblgen writes the C++ classes.
#ifndef NOVA_DIALECT
#define NOVA_DIALECT

include "mlir/IR/OpBase.td"

def Nova_Dialect : Dialect {
    let name = "nova";
    let summary = "Nova's own operations: tensors, objects, containers and the runtime";
    let description = [{
        The operations of the Nova language that no upstream dialect has. Numbers and their arithmetic use
        `arith`, control flow `scf`, and functions `func` (docs/DESIGN.md, C2).
    }];
    let cppNamespace = "::nova";
    let useDefaultTypePrinterParser = 1;  // !nova.<type> is printed and parsed by each type's assembly format
}

// The base class of every nova op: Nova_Op<"matmul"> is the op nova.matmul
class Nova_Op<string mnemonic, list<Trait> traits = []> : Op<Nova_Dialect, mnemonic, traits>;

#endif  // NOVA_DIALECT
