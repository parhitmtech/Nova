n = 20
arr = list(range(n, 0, -1))
 
for j in range(n):
    for i in range(n - 1):
        if arr[i] > arr[i + 1]:
            arr[i], arr[i + 1] = arr[i + 1], arr[i]
 
for x in arr:
    print(x)