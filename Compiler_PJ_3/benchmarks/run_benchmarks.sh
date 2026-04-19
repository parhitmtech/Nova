#!/bin/bash
# NovaComp Benchmark Suite — run on AWS Linux
NOVA=../compiler
RUNS=5

echo "======================================"
echo "  NovaComp Benchmark Suite"
echo "  $(date)"
echo "======================================"

benchmark() {
    local label="$1"
    local cmd="$2"
    local total=0
    for i in $(seq 1 $RUNS); do
        ms=$({ time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | sed 's/.*m//;s/s//' | awk '{printf "%d", $1*1000}')
        total=$((total + ms))
    done
    avg=$((total / RUNS))
    echo "  $label: ${avg}ms (avg of $RUNS runs)"
}

echo ""
echo "── Fibonacci(35) ──────────────────────"
gcc -O2 -o fib_c fibonacci.c
benchmark "Nova (interpreted)" "$NOVA fibonacci.nova"
benchmark "Nova (native x86)"  "./fibonacci_native"
benchmark "Python 3"           "python3 fibonacci.py"
benchmark "C (gcc -O2)"        "./fib_c"

echo ""
echo "── Bubble Sort (n=20) ─────────────────"
gcc -O2 -o sort_c bubble_sort.c
benchmark "Nova (interpreted)" "$NOVA bubble_sort.nova"
benchmark "Nova (native x86)"  "./bubble_sort_native"
benchmark "Python 3"           "python3 bubble_sort.py"
benchmark "C (gcc -O2)"        "./sort_c"

echo ""
echo "── Matrix Multiply (32x32) ────────────"
gcc -O2 -o matmul_c matrix_multiply.c
benchmark "Nova (interpreted)" "$NOVA matrix_multiply.nova"
benchmark "Python 3"           "python3 matrix_multiply.py"
benchmark "C (gcc -O2)"        "./matmul_c"

echo ""
echo "======================================"
echo "  Done."
echo "======================================"