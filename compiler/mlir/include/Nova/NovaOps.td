// The nova dialect's types and ops. Step C1 has the first of each: the tensor type, and the ops that make, multiply
// and print tensors.
#ifndef NOVA_OPS
#define NOVA_OPS

include "Nova/NovaDialect.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

// !nova.tensor<f64>: a reference to a tensor of that element type, whose rank and sizes are known at run time
// (docs/DESIGN.md, C3: a Nova tensor can change in place and be shared, so it is a reference, not a value)
def Nova_TensorType : TypeDef<Nova_Dialect, "Tensor"> {
    let mnemonic = "tensor";
    let summary = "a reference to a Nova tensor";
    let parameters = (ins "::mlir::Type":$elementType);
    let assemblyFormat = "`<` $elementType `>`";
}

// ─────────────────────────────────────────────────────────────────────────────
// Ops
// ─────────────────────────────────────────────────────────────────────────────

// %t = nova.zeros %rows, %cols : !nova.tensor<f64>   (Nova's zeros(r, c))
def Nova_ZerosOp : Nova_Op<"zeros", [Pure]> {
    let summary = "a new tensor of zeros";
    let arguments = (ins Variadic<Index>:$sizes);
    let results = (outs Nova_TensorType:$result);
    let assemblyFormat = "$sizes attr-dict `:` qualified(type($result))";
}

// %c = nova.matmul %a, %b : !nova.tensor<f64>   (Nova's a @ b, batched and broadcast as the interpreter's)
// Both operands and the result have one type (AllTypesMatch), so the assembly writes it once; the verifier adds
// what a trait cannot say: matmul needs floats.
def Nova_MatmulOp : Nova_Op<"matmul", [Pure, AllTypesMatch<["lhs", "rhs", "result"]>]> {
    let summary = "matrix product, batched when an operand has rank 3 or more";
    let arguments = (ins Nova_TensorType:$lhs, Nova_TensorType:$rhs);
    let results = (outs Nova_TensorType:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` qualified(type($result))";  // !nova.tensor<f64>, not <f64>
    let hasVerifier = 1;  // MatmulOp::verify in NovaOps.cpp
}

// nova.print %t : !nova.tensor<f64>   (Nova's print and tensor_print: writes, so not Pure)
def Nova_PrintOp : Nova_Op<"print"> {
    let summary = "print a value";
    let arguments = (ins AnyType:$value);
    let assemblyFormat = "$value attr-dict `:` type($value)";
}

#endif  // NOVA_OPS
