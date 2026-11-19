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
