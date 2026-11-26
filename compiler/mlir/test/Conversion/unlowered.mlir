// RUN: not nova-opt %s --convert-nova-to-runtime 2>&1 | FileCheck %s

// Every nova op must be lowered: one there is no pattern for yet stops the pass, and the error names it, instead of
// a binary being made without it. (Tensors are next.)

func.func @main(%n: index) {
  // CHECK: error: failed to legalize operation 'nova.zeros'
  %t = nova.zeros %n, %n : !nova.tensor<f64>
  nova.print %t : !nova.tensor<f64>
  return
}
