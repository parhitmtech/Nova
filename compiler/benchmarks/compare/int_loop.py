i = 0
s = 0
while i < 3000000:
    s = s + i
    s = s - i // 2
    i = i + 1
print(s)
