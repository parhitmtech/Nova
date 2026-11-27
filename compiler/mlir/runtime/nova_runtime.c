// Nova's C runtime: what a Nova program compiled through MLIR (--nova-to-llvm) calls for what LLVM IR has no
// instruction for. --convert-nova-to-runtime turns each nova op into a call to one of these. Each prints exactly as
// the interpreter does (src/runtime/interp_core.cc, exec_out), so a program's output is the same either way.
#include <stdint.h>
#include <stdio.h>

void nova_print_i32(int32_t x) { printf("%d\n", x); }

void nova_print_f32(float x) { printf("%f\n", x); }  // the float is passed to printf as a double, as in C

void nova_print_f64(double x) { printf("%lf\n", x); }

// The values of print(a, b, ...) before the last: each followed by a space, not a newline, as the interpreter's OUT
// without its newline. print() prints an empty line.
void nova_print_i32_sp(int32_t x) { printf("%d ", x); }

void nova_print_f32_sp(float x) { printf("%f ", x); }

void nova_print_f64_sp(double x) { printf("%lf ", x); }

void nova_print_newline(void) { printf("\n"); }
