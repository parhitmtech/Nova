// RUN: nova-opt %s | nova-opt | FileCheck %s

// The dialect's types and ops print as they are read: nova-opt parses the file and prints it, a second nova-opt
// parses that output, and FileCheck checks the result.

// CHECK-LABEL: func.func @product
func.func @product(%rows: index, %cols: index) -> !nova.tensor<f64> {
  // CHECK: %[[A:.*]] = nova.zeros %{{.*}}, %{{.*}} : !nova.tensor<f64>
  %a = nova.zeros %rows, %cols : !nova.tensor<f64>
  // CHECK: %[[B:.*]] = nova.zeros
  %b = nova.zeros %cols, %rows : !nova.tensor<f64>
  // CHECK: %[[C:.*]] = nova.matmul %[[A]], %[[B]] : !nova.tensor<f64>
  %c = nova.matmul %a, %b : !nova.tensor<f64>
  // CHECK: nova.print %[[C]] : !nova.tensor<f64>
  nova.print %c : !nova.tensor<f64>
  return %c : !nova.tensor<f64>
}

// get and set name the tensor's type once: the element's type follows from it (TypesMatchWith)
// CHECK-LABEL: func.func @elements
func.func @elements(%i: index, %j: index) -> f64 {
  // CHECK: %[[T:.*]] = nova.ones %{{.*}}, %{{.*}} : !nova.tensor<f64>
  %t = nova.ones %i, %j : !nova.tensor<f64>
  // CHECK: %[[X:.*]] = nova.get %[[T]][%{{.*}}, %{{.*}}] : !nova.tensor<f64>
  %x = nova.get %t[%i, %j] : !nova.tensor<f64>
  // CHECK: nova.set %[[X]], %[[T]][%{{.*}}, %{{.*}}] : !nova.tensor<f64>
  nova.set %x, %t[%j, %i] : !nova.tensor<f64>
  return %x : f64
}
