// RUN: nova-opt %s --cse --canonicalize | FileCheck %s
// RUN: nova-opt %s --loop-invariant-code-motion | FileCheck %s --check-prefix=LICM

// MLIR's general passes work on nova ops through the memory effects each op declares, with no code of Nova's. A
// Nova tensor can change in place, so an op that makes one allocates: --cse never merges two of them, --canonicalize
// still removes one nothing uses, and LICM never hoists one out of a loop. print writes, so it stays.

// CHECK-LABEL: func.func @twice
func.func @twice(%n: index) {
  // CHECK-COUNT-1: nova.zeros
  // CHECK-NOT: nova.zeros
  %a = nova.zeros %n, %n : !nova.tensor<f64>
  %unused = nova.zeros %n, %n : !nova.tensor<f32>
  // Two products are two tensors: a write to one must not show in the other
  // CHECK: %[[C:.*]] = nova.matmul
  // CHECK: %[[D:.*]] = nova.matmul
  %c = nova.matmul %a, %a : !nova.tensor<f64>
  %d = nova.matmul %a, %a : !nova.tensor<f64>
  // CHECK: nova.print %[[C]]
  // CHECK: nova.print %[[D]]
  nova.print %c : !nova.tensor<f64>
  nova.print %d : !nova.tensor<f64>
  return
}

// Two reads of one element merge when nothing writes between them; a read after a write does not
// CHECK-LABEL: func.func @reads
func.func @reads(%t: !nova.tensor<f64>, %i: index, %x: f64) -> (f64, f64, f64) {
  // CHECK: %[[R:.*]] = nova.get %{{.*}}[%{{.*}}] : !nova.tensor<f64>
  // CHECK-NEXT: nova.set
  // CHECK-NEXT: %[[S:.*]] = nova.get %{{.*}}[%{{.*}}] : !nova.tensor<f64>
  // CHECK-NEXT: return %[[R]], %[[R]], %[[S]]
  %r1 = nova.get %t[%i] : !nova.tensor<f64>
  %r2 = nova.get %t[%i] : !nova.tensor<f64>
  nova.set %x, %t[%i] : !nova.tensor<f64>
  %r3 = nova.get %t[%i] : !nova.tensor<f64>
  return %r1, %r2, %r3 : f64, f64, f64
}

// A zeros in a loop is a new tensor each iteration, so LICM leaves it there; the constant moves out
// LICM-LABEL: func.func @fresh
// LICM: arith.constant 1.000000e+00 : f64
// LICM: scf.for
// LICM-NEXT: nova.zeros
func.func @fresh(%n: index) {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  scf.for %i = %c0 to %n step %c1 {
    %one = arith.constant 1.0 : f64
    %t = nova.zeros %n, %n : !nova.tensor<f64>
    nova.set %one, %t[%i, %i] : !nova.tensor<f64>
    nova.print %t : !nova.tensor<f64>
  }
  return
}
