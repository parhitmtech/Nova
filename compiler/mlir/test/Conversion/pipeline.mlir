// RUN: nova-opt %s --nova-to-llvm | FileCheck %s

// --nova-to-llvm lowers all the way to the llvm dialect: the loop becomes blocks and branches (scf to cf, then cf
// to llvm), arith becomes llvm arithmetic, and the runtime call an llvm.call. Nothing of nova, scf, func or arith
// is left for mlir-translate.

// CHECK-NOT: nova.
// CHECK-NOT: scf.
// CHECK-NOT: func.
// CHECK-NOT: arith.
// CHECK: llvm.func @nova_print_i32(i32)
// CHECK-LABEL: llvm.func @main() -> i32
// CHECK: llvm.cond_br
// CHECK: llvm.add
// CHECK: llvm.call @nova_print_i32
// CHECK: llvm.return
func.func @main() {
  %c0 = arith.constant 0 : i32
  %c1 = arith.constant 1 : i32
  %c3 = arith.constant 3 : i32
  %r = scf.while (%i = %c0) : (i32) -> i32 {
    %go = arith.cmpi slt, %i, %c3 : i32
    scf.condition(%go) %i : i32
  } do {
  ^bb0(%i: i32):
    %next = arith.addi %i, %c1 : i32
    scf.yield %next : i32
  }
  nova.print %r : i32
  return
}
