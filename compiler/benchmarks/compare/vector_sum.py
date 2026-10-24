n = 1000000
v = []
for i in range(n):
    v.append(i)
s = 0
for i in range(n):
    s = s + v[i] % 7
print(s)
