#include <stdio.h>

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main(void) {
    volatile int n = 30; /* keep the compiler from folding the call */
    printf("%d\n", fib(n));
    return 0;
}
