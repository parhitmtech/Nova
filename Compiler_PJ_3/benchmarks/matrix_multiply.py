import random
import time
 
n = 32
A = [[random.gauss(0,1) for _ in range(n)] for _ in range(n)]
B = [[random.gauss(0,1) for _ in range(n)] for _ in range(n)]
 
C = [[0.0]*n for _ in range(n)]
for i in range(n):
    for k in range(n):
        for j in range(n):
            C[i][j] += A[i][k] * B[k][j]
 
print("done")