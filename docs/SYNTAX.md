# Nova syntax (target design, draft 3)

Status: draft for review. This is where the syntax is heading. The current parser gets there in
small steps (see "Migration order" at the end); each step converts every test, example and stdlib
file, so there is only ever one syntax.

Design rule: keep every keyword and control structure Nova has today (`for`, `while`, `do`-`while`,
`switch` / `case` / `default`, `struct`, `class`, `extends`, `self`, ...), replace braces and
semicolons with indentation, and make tensors a real type with operators. Types always come first
(`int x`, `Tensor<float> W`), in variables, parameters and fields alike, with C++-style type
arguments in angle brackets.

## 1. Example

```python
orbit novatorch.nn

fn main():
    Tensor X = zeros(4, 2)
    X[1, 1] = 1.0
    X[2, 0] = 1.0
    X[3, 0] = 1.0
    X[3, 1] = 1.0
    Tensor Y = zeros(4, 1)
    Y[1, 0] = 1.0
    Y[2, 0] = 1.0

    Linear layer1 = Linear(2, 4)
    Linear layer2 = Linear(4, 1)
    double lr = 0.5
    int epoch = 0

    while (epoch < 10_000):
        Tensor pred = sigmoid(layer2.forward(sigmoid(layer1.forward(X))))
        Tensor loss = mse_loss(pred, Y)
        backward(loss)
        layer1.W -= lr * layer1.W.grad
        layer2.W -= lr * layer2.W.grad
        epoch += 1

    print(pred)
```

## 2. Lexical structure

- **Blocks by indentation.** A line ending in `:` opens a block; the block is the following lines
  indented further. The lexer turns indentation changes into `INDENT` / `DEDENT` tokens. Indent with
  spaces (tabs are an error, to avoid mixed widths).
- **Newlines end statements.** No semicolons, except as separators inside a `for (...)` header. A
  newline inside `( )` or `[ ]` does not end a statement, so long calls can wrap freely.
- **Comments:** `# ...` to end of line.
- **Literals:** integers `42`, `10_000`; floats `0.5`, `1e-3`; strings `"..."` with `\n`, `\t`,
  `\"`, `\\`; booleans `true`, `false`.
- **Identifiers:** letters, digits, `_`; not starting with a digit.

## 3. Types

| Type | Meaning |
|---|---|
| `int` | integers |
| `float`, `double` | floating point |
| `bool`, `string` | booleans, strings |
| `Tensor<float>` | N-D tensor (1 to 6 dimensions) of fp32 elements |
| `Tensor<double>`, `Tensor<int>` | fp64 tensor (e.g. gradient checks); integer tensor (e.g. token ids) |
| `Tensor` | short for `Tensor<float>`, the default dtype |
| `Tensor<float, [B, T, C]>` | later: tensor with a symbolic shape, checked at compile time |
| `vector<int>`, `map<string, int>`, ... | generic containers from `std.collections` (section 10) |
| `array` | fixed-size int array (as today) |
| user `struct` / `class` names | records and classes; `class Box<T>` for generic classes |

`Tensor` is built into the language, not a library type: the compiler has to understand tensor ops
for autodiff, fusion and GPU code generation. Containers and algorithms live in `std`.

After a known type name (`Tensor`, `vector`, `map`, a generic class), `<` starts type arguments;
anywhere else it is less-than. This is how C++ resolves the same ambiguity.

## 4. Variables

Declarations stay as today, type first, without the semicolon:

```python
int x = 5
double y = 0.0
Tensor w = zeros(2, 3)              # Tensor<float>
Tensor<int> ids = zeros(16)
Point p = Point(1, 2)
vector<int> v = vector<int>()
```

Variables are block-scoped: a variable declared inside a loop body is not visible after the loop, and
the same name can be declared again in a later block.

## 5. Functions

```python
fn add(int a, int b) -> int:       # typed: every parameter and the return type are declared
    return a + b

fn forward(Tensor<float> x) -> Tensor<float>:
    return relu(x)

def scale(x, k):                   # loose: types are inferred from each call
    return x * k

fn main():                         # entry point
    print(add(1, 2))
    print(scale(2.0, 3.0))         # compiles a double version of scale
```

- `fn`: all parameter types required; return type required unless the function returns nothing.
- `def`: parameter and return types may be omitted. Nova is compiled, so `def` is not dynamic: the
  compiler makes one specialized copy per distinct combination of argument types it is called with
  (like C++ templates). Type errors are still compile errors.

## 6. Structs and classes

```python
struct Point:
    int x
    int y

class Linear:
    Tensor W
    Tensor b

    fn init(self, int in_f, int out_f):       # constructor: Linear(2, 4) calls init
        self.W = xavier(in_f, out_f)
        self.b = zeros(1, out_f)

    fn forward(self, Tensor x) -> Tensor:
        return x @ self.W + self.b

class Tanh extends Activation:
    fn forward(self, Tensor x) -> Tensor:
        return tanh(x)

class Box<T>:                                 # generic class: one compiled copy per T used
    T value

    fn init(self, T v):
        self.value = v
```

`Point(1, 2)` builds a struct from its fields in order. `self` is always the first parameter of a
method and has no type written.

## 7. Statements

All of today's control structures stay, with indentation instead of braces:

```python
if (x > 0):
    ...
elif (x == 0):
    ...
else:
    ...

while (i < n):
    i += 1

for (i = 0; i < n; i += 1):
    if (i == 3):
        continue              # new: skip to the next iteration
    if (i == 7):
        break                 # new: leave the loop
    ...

do:
    i += 1
while (i < n)

switch (x):
    case 1:
        print(10)
    case 2:
        print(20)
    default:
        print(0)
```

Also: `return`, assignment `=`, compound `+= -= *= /=` (new), and `print(...)`, `input()` as today.
`break` and `continue` work in `for`, `while` and `do`-`while`, like Python.

## 8. Expressions

Precedence, lowest to highest:

| Level | Operators |
|---|---|
| 1 | `or` |
| 2 | `and` |
| 3 | `not` |
| 4 | `==  !=  <  >  <=  >=` |
| 5 | `+  -` |
| 6 | `*  /  %  @` |
| 7 | unary `-` |
| 8 | call `f(x)`, index `t[i, j]`, attribute `a.b`, `t.T` |

On tensors: `+ - * /` are element-wise (with the existing row broadcasting), `@` is matmul,
`t[i, j, k]` reads or writes one element, `t.T` is the transpose, `t.shape` and `t.rank` give the
shape. A tensor and a number can be combined: `lr * dW`.

## 9. Built-ins

Tensor built-ins lose the `tensor_` prefix: `zeros`, `ones`, `randn`, `xavier`, `reshape`, `relu`,
`sigmoid`, `tanh`, `softmax`, `matmul`, `mse_loss`, `cross_entropy_loss`, `requires_grad`,
`backward`, ... Method forms are also available: `t.sum(0)`, `t.reshape(6, -1)`, `t.grad`.
`hf.*` and `sol.*` stay as they are.

## 10. Modules

One keyword, `orbit`, for both the standard library and your own files:

```python
orbit novatorch.nn      # stdlib: $NOVA_HOME/stdlib/novatorch.nn.nova
orbit model             # your own model.nova, next to this file
orbit utils.metrics     # your own utils/metrics.nova
```

The compiler looks next to the current file first, then in the stdlib. Everything the orbited file
defines becomes visible (as with today's `import`). A file is only loaded once. If two orbited
modules define the same name, write the full path for that name: `std.collections.vector<int>`.

The standard library lives under `std`:

```python
orbit std.collections       # vector, map, set, stack, queue (generic)
orbit std.algo              # sort, binary_search, min, max, ...
orbit std.math              # abs, pow, clamp, ...

vector<int> ids = vector<int>()
ids.push(3)
map<string, int> vocab = map<string, int>()
vocab["a"] = 0
sort(ids)
```

ML libraries stay under `novatorch` (`orbit novatorch.nn`, `orbit novatorch.optim`).

## 11. Today -> target

| Today | Target |
|---|---|
| `main() { ... }` | `fn main():` + indented block |
| `int x = 5 ;` | `int x = 5` |
| `int t = tensor_zeros(2, 3) ;` | `Tensor t = zeros(2, 3)` |
| `def f(double x) -> double {` | `fn f(double x) -> double:` |
| `def forward(int x) -> int {` (tensor as `int`) | `fn forward(Tensor x) -> Tensor:` |
| `for(i = 1 ; i < n ; i = i + 1) {` | `for (i = 1; i < n; i += 1):` |
| `do { ... } while c ;` | `do:` + block, then `while (c)` |
| `switch(x) { case 1: {...} default: {...} }` | `switch (x):` / `case 1:` / `default:` |
| `struct P { int x ; }` | `struct P:` / `int x` |
| `class A extends B {` | `class A extends B:` |
| `Linear l ; l.init(2, 4) ;` | `Linear l = Linear(2, 4)` |
| `a <> b` | `a != b` |
| `(0.0 - 2.0)` | `-2.0` |
| `tensor_get(t, i, j)`, `tensor_set(t, i, j, v)` | `t[i, j]`, `t[i, j] = v` |
| `tensor_add(a, b)`, `matmul(a, b)` | `a + b`, `a @ b` |
| `import <novatorch.nn> ;`, `import "model.nova" ;` | `orbit novatorch.nn`, `orbit model` |
| `// comment` | `# comment` |

## Migration order

Each step keeps `make test` green and converts all tests, examples and stdlib files in the same
commit.

1. Operators and conditions (done): `!=`, `<=`, `>=`, `%`, unary minus, `+= -= *= /=`;
   conditions of `if` / `elif` / `while` / `do`-`while` / `switch` in mandatory parentheses;
   comparisons and `and` / `or` / `not` are expressions, so any expression is a condition.
2. Functions: `fn` and `fn main()`.
3. Indentation: `INDENT` / `DEDENT`, `:` blocks, no braces, no semicolons, `#` comments only.
4. `break` and `continue`.
5. `Tensor` / `Tensor<dtype>` as a real type (no more `int` tensors).
6. Tensor operators and methods: `+ - * / @` on tensors, `t[i, j]`, `t.T`, `t.shape`, short names.
7. Constructors, `struct` / `class` field syntax, `orbit`.
8. Block scoping.
9. Generics: loose `def` and `class Box<T>` (one compiled copy per type used), then the generic
   `std.collections` (`vector<T>`, `map<K, V>`, ...) and `std.algo` written in Nova.
10. Later, with the Tensor IR: shape types `Tensor<float, [B, T, C]>`.
