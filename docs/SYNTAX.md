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

ignite:
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
  indented further (any consistent amount; 4 spaces by convention). Indent with spaces: tabs are an
  error, to avoid mixed widths. A block whose body is one statement can stay on its line:
  `if (x > 0): print(x)`. An empty block is written `pass`.
- **Newlines end statements.** No semicolons, except as separators inside a `for (...)` header. A
  newline inside `( )` or `[ ]` does not end a statement, so long calls can wrap freely, and the
  continuation lines may be indented however you like.
- **Comments:** `# ...` to end of line. A comment's indentation does not matter. (`//` is an error.)
- **How it is implemented:** a layout pass in the lexer turns `:` into `{`, a return to a smaller
  indentation into `}`, and each line end into `;`, so the parser reads the same token stream as
  before. A `case` / `default` label keeps its `:`.
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

Variables are block-scoped: a variable declared inside a block (an `if` / `elif` / `else`, loop or `case`
body) is not visible after it, and the same name can be declared again in a later block, even with
another type. A name that is already visible (an outer variable or a parameter) cannot be declared again
in an inner block: there is no shadowing. A declaration runs every time its block does, so `int n` in a
loop body starts at 0 on every pass.

## 5. Functions

```python
def add(int a, int b) -> int:      # typed: parameter types first, return type after ->
    return a + b

def forward(Tensor<float> x) -> Tensor<float>:
    return relu(x)

def scale(x, k):                   # loose: types are inferred from each call
    return x * k

ignite:                            # entry point: runs when this file is the program
    print(add(1, 2))
    print(scale(2.0, 3.0))         # compiles a double version of scale
```

- Every function is a `def`. Parameter types and the `-> type` return type may each be written or
  left out. Nova is compiled, so leaving them out is not dynamic: the compiler makes one specialized
  copy per distinct combination of argument types a function is called with (like C++ templates),
  and type errors are still compile errors.
- **Entry point:** the `ignite` block. It comes after the definitions (Nova is one-pass, so a
  function must be defined before it is called), and a file has at most one. A file without an
  `ignite` block may define `def main()` instead. When a file is pulled in with `orbit`, its
  `ignite` block does not run, so a library file can carry its own demo or quick test, like Python's
  `if __name__ == "__main__":`.

## 6. Structs and classes

```python
struct Point:
    int x
    int y

class Linear:
    Tensor W
    Tensor b

    def init(self, int in_f, int out_f):       # constructor: Linear(2, 4) calls init
        self.W = xavier(in_f, out_f)
        self.b = zeros(1, out_f)

    def forward(self, Tensor x) -> Tensor:
        return x @ self.W + self.b

class Tanh extends Activation:
    def forward(self, Tensor x) -> Tensor:
        return tanh(x)

class Box<T>:                                 # generic class: one compiled copy per T used
    T value

    def init(self, T v):
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

On tensors: `+ - * /` are element-wise on tensors of the same shape (an MxN matrix `+` or `-` a 1xN row
broadcasts the row, e.g. a bias; `+` takes the row on either side), `@` is matmul, `t[i, j, k]` reads or
writes one element, `t.T` is the transpose, `t.shape` and `t.rank` give the shape. These work on fields
too: `self.W.T`, `layer.W[0, 1] = v`. A tensor and a number combine with `+ - *` in either order
(`lr * dW`, `t + 1.0`, `1.0 - t`) and with `/` when the number is on the right.

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
| `main() { ... }` | `ignite:` + indented block (or `def main():`) |
| `int x = 5 ;` | `int x = 5` |
| `int t = tensor_zeros(2, 3) ;` | `Tensor t = zeros(2, 3)` |
| `def f(double x) -> double {` | `def f(double x) -> double:` |
| `def forward(int x) -> int {` (tensor as `int`) | `def forward(Tensor x) -> Tensor:` |
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
2. Entry point (done): `ignite { ... }` replaces `main() { ... }`; `def main()` is also accepted; an
   imported file's `ignite` block does not run.
3. Indentation (done): `:` blocks with indented bodies, one-line bodies, `pass`; no braces, no
   semicolons, `#` comments only; the REPL reads a block until a blank line.
4. `break` and `continue` (done): in `for`, `while` and `do`-`while`; inside a `switch` they act on the
   enclosing loop (cases never fall through, so a switch needs no `break` of its own).
5. `Tensor` as a real type (done): variables, parameters, returns, fields and multiple returns; tensor ops
   check their arguments; a tensor is no longer an `int`; `Tensor h` declares one to assign later.
   `Tensor<dtype>` waits for fp32 storage (M1 stage 4). Also: `nova --check` (type-check only) and
   correct line numbers after an import.
6. Tensor operators (done): `a @ b`, element-wise `a + b`, `a - b`, `a * b` (+ and - broadcast a 1xN
   row), `k * a` / `a * k` / `a / k` scale, `-a`; `t[i, j]` reads and `t[i, j] = v` writes one element;
   `t.T`, `t.rows`, `t.cols`, `t.rank`; short names `zeros`, `ones`, `randn`, `xavier`, `reshape`.
   Follow-up (done): `t + k`, `k + t`, `t - k`, `k - t`; element-wise `a / b` (and `tensor_div`); postfixes
   and element stores on fields (`self.W.T`, `self.W[i, j] = v`, `obj.W[0, 0]`); shape mismatches are a
   runtime error instead of reading past the end. Not yet: `k / t`, methods like `t.sum()`.
7. Constructors and modules (done): `Linear l = Linear(2, 4)` runs init (`Linear l` runs a no-argument
   init); `Point p = Point(3, 4)`; `orbit a.b` loads a/b.nova next to the file, else a.b.nova from the
   stdlib; `import` is an error. Field syntax (`int x` under `struct P:`) already came with step 3.
8. Block scoping (done): a name declared in a block ends with it; a later block can declare it again;
   no shadowing (redeclaring a visible name is an error, for every type, struct and object); a
   declaration without a value resets on every pass. The debugger lists only the top-level names.
9. Generics: loose `def` and `class Box<T>` (one compiled copy per type used), then the generic
   `std.collections` (`vector<T>`, `map<K, V>`, ...) and `std.algo` written in Nova.
10. Later, with the Tensor IR: shape types `Tensor<float, [B, T, C]>`.
