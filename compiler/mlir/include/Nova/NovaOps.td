// The nova dialect's types and ops. Step C1 has the first of each: the tensor type, and the ops that make tensors,
// combine them (matmul, element-wise, with a number), read and write their elements, and print them.
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

// Element-wise: %c = nova.add %a, %b : !nova.tensor<f64>   (Nova's a + b and tensor_add(a, b); sub, mul, div)
// A new tensor, each element the two operands' elements combined. The operands have the same shape, or (add and
// sub) one is a 1xN row, which is combined with every row of the other: for add either one, for sub the right one.
// Shapes are known only at run time, so the runtime checks them, as the interpreter's does.
class Nova_ElementwiseOp<string mnemonic, string what> : Nova_Op<mnemonic, [AllTypesMatch<["lhs", "rhs", "result"]>]> {
    let summary = "element-wise " # what # " of two tensors";
    let arguments = (ins Arg<Nova_TensorType, "", [MemRead]>:$lhs, Arg<Nova_TensorType, "", [MemRead]>:$rhs);
    let results = (outs Res<Nova_TensorType, "a new tensor", [MemAlloc]>:$result);
    let assemblyFormat = "$lhs `,` $rhs attr-dict `:` qualified(type($result))";
}

def Nova_AddOp : Nova_ElementwiseOp<"add", "sum">;
def Nova_SubOp : Nova_ElementwiseOp<"sub", "difference">;
def Nova_MulOp : Nova_ElementwiseOp<"mul", "product">;  // the Hadamard product; a @ b is nova.matmul
def Nova_DivOp : Nova_ElementwiseOp<"div", "quotient">;

// A tensor and one number: %s = nova.scale %t, %k : !nova.tensor<f64>   (Nova's t * k and k * t; t / k is
// t * (1 / k), and -t is t * -1, as the interpreter computes them). nova.add_scalar adds k to every element (t + k;
// t - k is t + -k). The number is the tensor's element type, so the assembly writes only the tensor's type.
class Nova_ScalarOp<string mnemonic, string what>
    : Nova_Op<mnemonic, [AllTypesMatch<["tensor", "result"]>,
                         TypesMatchWith<"the number is the tensor's element type", "tensor", "scalar", ElementType>]> {
    let summary = what;
    let arguments = (ins Arg<Nova_TensorType, "", [MemRead]>:$tensor, AnyType:$scalar);
    let results = (outs Res<Nova_TensorType, "a new tensor", [MemAlloc]>:$result);
    let assemblyFormat = "$tensor `,` $scalar attr-dict `:` qualified(type($tensor))";
}

def Nova_ScaleOp : Nova_ScalarOp<"scale", "every element times a number">;
def Nova_AddScalarOp : Nova_ScalarOp<"add_scalar", "every element plus a number">;

// nova.print %t : !nova.tensor<f64>   (Nova's print and tensor_print: writes, so not Pure)
def Nova_PrintOp : Nova_Op<"print"> {
    let summary = "print a value";
    let description = [{
        Prints a value, then a newline: `nova.print %x : i32`. With `continues`, a space instead of the newline, for
        the values of `print(a, b, ...)` before the last: `nova.print %x {continues} : i32`. (A tensor ends its own
        line either way.)
    }];
    let arguments = (ins AnyType:$value, UnitAttr:$continues);
    let assemblyFormat = "$value attr-dict `:` type($value)";
}

def Nova_NewlineOp : Nova_Op<"newline"> {
    let summary = "print an empty line: print()";
    let assemblyFormat = "attr-dict";
}

#endif  // NOVA_OPS
