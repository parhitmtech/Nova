// RUN: nova-opt %s -split-input-file -verify-diagnostics

// The verifier's errors: -split-input-file checks each case on its own, and expected-error names the message an
// op must report (expected-note, a note that goes with it).

func.func @ints(%n: index) {
  %a = nova.zeros %n, %n : !nova.tensor<i32>
  // expected-error @+1 {{'nova.matmul' op needs tensors of floats, not of 'i32'}}
  %c = nova.matmul %a, %a : !nova.tensor<i32>
  return
}

// -----

func.func @mixed(%n: index) {
  %a = nova.zeros %n, %n : !nova.tensor<f64>
  // expected-note @+1 {{prior use here}}
  %b = nova.zeros %n, %n : !nova.tensor<f32>
  // expected-error @+1 {{use of value '%b' expects different type than prior uses}}
  %c = nova.matmul %a, %b : !nova.tensor<f64>
  return
}

// -----

// In the generic form, where every type is written, a get whose result is not the tensor's element type
func.func @element(%t: !nova.tensor<f64>, %i: index) {
  // expected-error @+1 {{'nova.get' op failed to verify that the result is the tensor's element type}}
  %x = "nova.get"(%t, %i) : (!nova.tensor<f64>, index) -> f32
  return
}

// -----

// Element-wise ops take two tensors of one type
func.func @mixed(%a: !nova.tensor<f64>, %b: !nova.tensor<f32>) {
  // expected-error @+1 {{'nova.add' op failed to verify that all of {lhs, rhs, result} have same type}}
  %c = "nova.add"(%a, %b) : (!nova.tensor<f64>, !nova.tensor<f32>) -> !nova.tensor<f64>
  return
}

// -----

// The number a tensor is scaled by is the tensor's element type
func.func @scale(%t: !nova.tensor<f64>, %k: f32) {
  // expected-error @+1 {{'nova.scale' op failed to verify that the number is the tensor's element type}}
  %s = "nova.scale"(%t, %k) : (!nova.tensor<f64>, f32) -> !nova.tensor<f64>
  return
}
