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
