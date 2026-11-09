#include <stdio.h>

int main(void) {
    volatile int n = 3000000;
    long long s = 0;
    for (int i = 0; i < n; i++) { s += i; s -= i / 2; }
    printf("%lld\n", s);
    return 0;
}
