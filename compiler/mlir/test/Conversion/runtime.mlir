// RUN: nova-opt %s --convert-nova-to-runtime | FileCheck %s

// nova.print becomes a call to the runtime function for its type, declared once at the start of the module; a bool
// prints as an int. @main returns an i32 0, as C's main does.

// CHECK-DAG: func.func private @nova_print_i32(i32)
// CHECK-DAG: func.func private @nova_print_f32(f32)
// CHECK-DAG: func.func private @nova_print_f64(f64)

// CHECK-LABEL: func.func @show(%arg0: i32, %arg1: f32, %arg2: f64, %arg3: i1) {
func.func @show(%i: i32, %f: f32, %d: f64, %b: i1) {
  // CHECK-NEXT: call @nova_print_i32(%arg0) : (i32) -> ()
  // CHECK-NEXT: call @nova_print_f32(%arg1) : (f32) -> ()
  // CHECK-NEXT: call @nova_print_f64(%arg2) : (f64) -> ()
  // CHECK-NEXT: %[[B:.*]] = arith.extui %arg3 : i1 to i32
  // CHECK-NEXT: call @nova_print_i32(%[[B]]) : (i32) -> ()
  // CHECK-NEXT: call @nova_print_i32(%arg0) : (i32) -> ()
  // CHECK-NEXT: return
  nova.print %i : i32
  nova.print %f : f32
  nova.print %d : f64
  nova.print %b : i1
  nova.print %i : i32
  return
}

// CHECK-LABEL: func.func @main() -> i32 {
// CHECK: %[[ZERO:.*]] = arith.constant 0 : i32
// CHECK-NEXT: return %[[ZERO]] : i32
func.func @main() {
  %x = arith.constant 7 : i32
  nova.print %x : i32
  return
}
