// The nova dialect's types and ops. Step C1 has the first of each: the tensor type, and the ops that make and
// multiply tensors, read and write their elements, and print them.
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

// Memory effects. A Nova tensor is a reference that nova.set changes in place (docs/DESIGN.md, C3), so no op that
// makes a tensor is Pure: two zeros are two tensors, and a write to one must not show in the other. Instead each
// op says what it does to memory, value by value: MemAlloc on a result it makes, MemRead or MemWrite on a tensor
// it reads or writes. MLIR's passes act on that, with no code of Nova's:
//   - CSE merges two ops only if neither has an effect other than reads, and nothing writes between them: two
//     nova.get of one element merge, two nova.zeros never do.
//   - LICM hoists only ops with no effects, so a zeros in a loop stays there, one new tensor per iteration.
//   - An op whose only effects are allocating its results and reading is dead when nothing uses its results, so
//     canonicalize still removes a zeros or a matmul nothing uses.

// A new tensor filled with one value: %t = nova.zeros %rows, %cols : !nova.tensor<f64>. A TableGen class, so zeros
// and ones are written once; each def below is one op.
class Nova_FillOp<string mnemonic, string what> : Nova_Op<mnemonic> {
    let summary = "a new tensor of " # what;
    let arguments = (ins Variadic<Index>:$sizes);
    let results = (outs Res<Nova_TensorType, "the new tensor", [MemAlloc]>:$result);
    let assemblyFormat = "$sizes attr-dict `:` qualified(type($result))";
}

def Nova_ZerosOp : Nova_FillOp<"zeros", "zeros">;  // Nova's zeros(r, c) and tensor_zeros
def Nova_OnesOp : Nova_FillOp<"ones", "ones">;     // Nova's ones(r, c) and tensor_ones

// %c = nova.matmul %a, %b : !nova.tensor<f64>   (Nova's a @ b and matmul(a, b), batched and broadcast as the
// interpreter's)
// Both operands and the result have one type (AllTypesMatch), so the assembly writes it once; the verifier adds
// what a trait cannot say: matmul needs floats. It reads its operands and makes a new tensor.
def Nova_MatmulOp : Nova_Op<"matmul", [AllTypesMatch<["lhs", "rhs", "result"]>]> {
    let summary = "matrix product, batched when an operand has rank 3 or more";
    let arguments = (ins Arg<Nova_TensorType, "", [MemRead]>:$lhs, Arg<Nova_TensorType, "", [MemRead]>:$rhs);
    let results = (outs Res<Nova_TensorType, "the product, a new tensor", [MemAlloc]>:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` qualified(type($result))";  // !nova.tensor<f64>, not <f64>
    let hasVerifier = 1;  // MatmulOp::verify in NovaOps.cpp
}

// The type of a tensor's elements, for TypesMatchWith: $_self is the tensor's type
defvar ElementType = "::llvm::cast<::nova::TensorType>($_self).getElementType()";

// %x = nova.get %t[%i, %j] : !nova.tensor<f64>   (Nova's t[i, j] and tensor_get(t, i, j))
// One subscript per dimension; the rank is known only at run time, so the verifier cannot count them. The result
// is the tensor's element type (TypesMatchWith), so the assembly does not write it.
def Nova_GetOp : Nova_Op<"get", [TypesMatchWith<"the result is the tensor's element type", "tensor", "result",
                                                ElementType>]> {
    let summary = "read one element of a tensor";
    let arguments = (ins Arg<Nova_TensorType, "the tensor read", [MemRead]>:$tensor, Variadic<Index>:$indices);
    let results = (outs AnyType:$result);
    let assemblyFormat = "$tensor `[` $indices `]` attr-dict `:` qualified(type($tensor))";
}

// nova.set %x, %t[%i, %j] : !nova.tensor<f64>   (Nova's t[i, j] = x and tensor_set(t, i, j, x))
// It changes the tensor in place: the SSA value %t still names the same tensor afterwards.
def Nova_SetOp : Nova_Op<"set", [TypesMatchWith<"the value is the tensor's element type", "tensor", "value",
                                                ElementType>]> {
    let summary = "write one element of a tensor";
    let arguments = (ins AnyType:$value, Arg<Nova_TensorType, "the tensor written", [MemWrite]>:$tensor,
                         Variadic<Index>:$indices);
    let assemblyFormat = "$value `,` $tensor `[` $indices `]` attr-dict `:` qualified(type($tensor))";
}

// nova.print %t : !nova.tensor<f64>   (Nova's print and tensor_print: writes, so not Pure)
def Nova_PrintOp : Nova_Op<"print"> {
    let summary = "print a value";
    let arguments = (ins AnyType:$value);
    let assemblyFormat = "$value attr-dict `:` type($value)";
}

#endif  // NOVA_OPS
