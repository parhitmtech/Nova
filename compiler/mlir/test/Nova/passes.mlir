// RUN: nova-opt %s --cse --canonicalize | FileCheck %s

// MLIR's general passes work on nova ops through their traits, with no code of Nova's: zeros and matmul are Pure,
// so --cse merges the two identical matmuls and --canonicalize removes the zeros nothing uses. print writes, so
// it stays.

// CHECK-LABEL: func.func @twice
func.func @twice(%n: index) {
  // CHECK-COUNT-1: nova.zeros
  %a = nova.zeros %n, %n : !nova.tensor<f64>
  %unused = nova.zeros %n, %n : !nova.tensor<f32>
  // CHECK: %[[C:.*]] = nova.matmul
  // CHECK-NOT: nova.matmul
  %c = nova.matmul %a, %a : !nova.tensor<f64>
  %d = nova.matmul %a, %a : !nova.tensor<f64>
  // CHECK: nova.print %[[C]]
  // CHECK: nova.print %[[C]]
  nova.print %c : !nova.tensor<f64>
  nova.print %d : !nova.tensor<f64>
  return
}
