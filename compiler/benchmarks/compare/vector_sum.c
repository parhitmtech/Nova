#include <stdio.h>
#include <stdlib.h>

int main(void) {
    volatile int n = 1000000;
    int *v = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) v[i] = i;
    long long s = 0;
    for (int i = 0; i < n; i++) s += v[i] % 7;
    printf("%lld\n", s);
    free(v);
    return 0;
}
