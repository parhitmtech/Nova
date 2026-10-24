const n = 1000000, v = [];
for (let i = 0; i < n; i++) v.push(i);
let s = 0;
for (let i = 0; i < n; i++) s = s + v[i] % 7;
console.log(s);
