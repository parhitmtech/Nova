#include <stdio.h>
 
int main() {
    int n = 20;
    int arr[20];
    for (int i = 0; i < n; i++)
        arr[i] = n - i;
 
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n - 1; i++)
            if (arr[i] > arr[i + 1]) {
                int tmp = arr[i];
                arr[i] = arr[i + 1];
                arr[i + 1] = tmp;
            }
 
    for (int i = 0; i < n; i++)
        printf("%d\n", arr[i]);
    return 0;
}