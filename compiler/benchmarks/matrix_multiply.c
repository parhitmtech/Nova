#include <stdio.h>
#include <stdlib.h>
 
int main() {
    int n = 32;
    double A[32][32], B[32][32], C[32][32];
 
    srand(42);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            A[i][j] = (double)rand()/RAND_MAX;
            B[i][j] = (double)rand()/RAND_MAX;
            C[i][j] = 0.0;
        }
 
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
            for (int j = 0; j < n; j++)
                C[i][j] += A[i][k] * B[k][j];
 
    printf("done\n");
    return 0;
}