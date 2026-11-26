# Nova's errors and warnings

Every error and warning the Nova compiler reports has a code, like `E0502`. The code never changes meaning, so you
can search for it. This page explains each one and shows how to fix it.

```
program.nova:6:11: error[E0502]: argument 1 of 'square' expects int but got string
   6 |     print(square("four"))
     |           ^~~~~~~~~~~~~~
```

The first line is `file:line:column: severity[code]: message`, the format compilers and editors share, so VS Code and
other tools can jump to it. The lines under it show the source, with the problem underlined and a caret where it
starts. Some errors are followed by a **note** that points at a related place, such as the first declaration of a
name declared twice.

## How the compiler reports

- **Every error in one run.** After a syntax error the parser skips to the next statement and goes on, so you see
  every broken statement at once, not one per run. Only the first syntax error on a line is reported: the rest of
  the line usually follows from it.
- **Names and types are checked once the file parses.** If the file has syntax errors, those are reported and the
  names and types are not checked yet: most of what that check would say ("unknown name", "wrong type") would come
  from the broken statements. Fix the syntax errors, and the next run checks the rest.
- **At most 20 errors** by default. `--error-limit=N` changes the limit; `--error-limit=0` removes it.
- **Warnings** are reported by `nova --check` (which checks a file without running it) and, when running, with
  `--warnings`. They never stop a program from running.
- **For tools:** `--diagnostics=json` prints one JSON document on stderr instead (see [the format](#the-json-format)).
  `--color=auto|always|never` controls colors; `auto` colors a terminal unless `NO_COLOR` is set.

## Layout and characters

### E0101: indentation uses a tab

Nova's blocks are set by indentation, and a tab is as wide as each editor decides, so indentation must be spaces.
Set your editor to insert spaces for Tab. The compiler reports the first tab only.

### E0102: unexpected indentation

A line is indented further than the line before it, but the line before does not end with `:` to open a block.

```python
print(x)
    print(y)      # E0102: move it back to the line above's indentation
```

### E0103: indentation matches no enclosing block

A line was moved left, but not as far as any block it is in: it must line up with one of them.

```python
if (x > 0):
        print(x)
    print(y)      # E0103: the if is at 0 and its block at 8; this line is at neither
```

### E0104: expected an indented block

A line ends with `:`, so the lines after it must be indented further, as its block. Add the block, or `pass` for an
empty one.

### E0105: unterminated string literal

A string has no closing `"` on its line. A string cannot span lines.

### E0106: unclosed bracket

A `(` or `[` is never closed. The compiler finds where it should have closed: at the next line that can only start a
new statement (such as an `if`, a `print`, or a declaration), or at the end of the file.

### E0107: comment written with `//`

Nova's comments start with `#`.

### E0108: `;` at the end of a statement

Nova ends a statement at the end of its line. Remove the `;`, and put a second statement on its own line. (A `;` is
used only between the parts of a `for` header.)

### E0109: block written with braces

Nova's blocks are a line ending in `:` and the indented lines under it, not `{ }`.

```python
while (x < 3):    # not: while (x < 3) {
    x += 1
```

### E0110: unexpected character

A character Nova does not use, such as `$` or `` ` ``, outside a string or a comment.

### E0111: Tensor with an element type

`Tensor<float>` is planned; for now every tensor holds 64-bit floats, and the type is written `Tensor`.

### E0112: `!` used for `not`

Nova writes `not x`, not `!x`. (`!=` is fine.)

### E0113: closing bracket without an opening one

A `)` or `]` that closes nothing: remove it, or add the bracket it was meant to close.

## Syntax

### E0201: expected a token

The parser expected a particular token, such as a `)`, a `:`, or a name, and found something else. The message
says what it expected and what it found.

### E0202: expected an expression

A value was expected, such as after an operator or an `=`, and the line ended or something else came.

```python
int a = 1 +       # E0202: the right side of + is missing
```

### E0203: expected a type

A type was expected (`int`, `float`, `double`, `bool`, `string`, `Tensor`, a class, `vector<T>`, ...), such as after
`extends` or in a field.

### E0204: expected a definition or the `ignite` block

At the top of a file there can only be definitions (`def`, `struct`, `class`), `orbit` lines and the `ignite:`
block. Statements go inside a function or the `ignite` block.

### E0205: condition without parentheses

The condition of `if`, `elif`, `while` and `switch` is written in parentheses: `if (x > 0):`.

### E0206: chained comparison

`a < b < c` is not allowed: write `a < b and b < c`.

### E0207: compound statement after `:` on the same line

After a `:`, the rest of the line can hold one simple statement (`if (x): y = 1`), but not another `if`, `while`,
`do`, `for` or `switch`: start the block on the next line.

## Files and modules

### E0301: `orbit` without a module name

`orbit` brings in a module, and needs its name: `orbit novatorch.nn`.

### E0302: module not found

`orbit a.b` looks for `a/b.nova` next to the file, then for `a.b.nova` in the standard library (`$NOVA_HOME/stdlib`).
Check the name, and where the file is.

### E0303: `import` used for `orbit`

Nova brings in modules with `orbit NAME`.

### E0304: cannot open the file

The file given to the compiler does not exist, or cannot be read.

### E0305: not a Nova file

The compiler reads `.nova` files (and `.csl`, the older name).

### E0306: NOVA_HOME is not set

The standard library is found through the `NOVA_HOME` environment variable, the directory Nova is installed in. Set
it (the VS Code extension sets it for you).

## Names and scopes

### E0401: no entry point

A program starts at its `ignite:` block (or at `def main()`, with no parameters). Add one at the end of the file.

### E0402: `ignite` block before a definition

The `ignite:` block comes last in a file: move the definitions after it above it.

### E0403: more than one `ignite` block

A file has one entry point.

### E0404: duplicate definition

Two functions, structs or classes have the same name. A note points at the first one.

### E0405: variable declared twice

A variable is declared again while the first one is still visible. Nova's variables do not shadow each other: give
the second one another name, or assign to the first (`x = 2`, not `int x = 2`). A note points at the first.

### E0406: unknown name

A name that is not declared where it is used: a misspelling, a variable used outside the block it was declared in,
or a function that does not exist.

### E0407: invalid parent class

A class extends something that is not a class, or a generic class (which cannot be extended).

### E0408: generic type without type arguments

A generic class or container needs its type arguments: `Box<int>`, `vector<string>`, `map<string, int>`.

### E0409: type arguments on a type that takes none

Type arguments on a type that is not generic, such as `int<float>` or `Point<int>`.

### E0410: `return` outside a function

`return` is allowed in a function only. To end the program early, return from `def main()`.

### E0411: `break` or `continue` outside a loop

`break` and `continue` are allowed inside `while`, `do` and `for` only.

### E0412: `self` outside a method

`self` is the object a method was called on, so it exists in a class's methods only.

## Types

### E0501: value of the wrong type

A value of one type where another is expected: assigning a `string` to an `int` variable, storing a `float` in a
`vector<int>`, or an object of one class where another is expected.

### E0502: argument of the wrong type

An argument does not fit its parameter's type. The message says which argument, what it expects and what it got.

### E0503: wrong number of arguments

A call gives more or fewer arguments than the function, constructor or method takes.

### E0504: return value of the wrong type

A `return` gives a value that does not fit the function's declared return type (`-> int`).

### E0505: wrong number of return values

A `return` gives a different number of values than the function returns.

### E0506: return type must be written

A function's return type is worked out from its first `return` when it is not written. It must be written when the
function returns several values (`-> int, int`), or when it calls itself before its first `return`.

### E0507: operator on the wrong types

An arithmetic operator on values it is not defined for: `"a" - 1`, `%` on a `double`, or mixing `int` and `float`
(convert one with `float(x)` or `int(x)`).

### E0508: values that cannot be compared

Two values of types that cannot be compared, or compared with `<`: structs (compare their fields), objects (only
`==` and `!=`), containers (only `==` and `!=`).

### E0509: string used as a condition

A string is not true or false: compare it, `if (s == "yes"):`.

### E0510: no such field or member

A struct, class or tensor does not have the field or member named. A tensor has `.T`, `.rows`, `.cols` and `.rank`.

### E0511: no such method

A class, or the `hf` or `sol` module, has no method of that name.

### E0512: class without a constructor

`Name(...)` creates an object by calling the class's `init` method; the class has none. Add `def init(...)`.

### E0513: invalid conversion

`int(x)`, `float(x)` and `double(x)` convert one number. Other types cannot be converted this way.

### E0514: wrong number of type arguments

A generic type is given the wrong number of type arguments: `vector` takes one, `map` two, and a generic class as many
as it declares.

### E0515: invalid type argument

Something that is not a type is used as a type argument.

### E0516: invalid element type

A container's elements can be `int`, `float`, `double`, `bool`, `string`, `Tensor` or objects of a class, but not
structs (make the struct a class) or other containers (wrap the inner container in a class).

### E0517: invalid map key type

A map's key must be an `int` or a `string`.

### E0518: index of the wrong type

A vector's index must be an `int`; a map's key must be its key type.

### E0519: switch on a value that is not an int

`switch` takes an `int`, and each `case` value must be an `int`.

### E0520: several values from something that does not give them

`int q, int r = f(x)` takes several values from one call: `f` must be a function that returns that many values, of
those types.

### E0521: invalid keyword argument

`name=value` arguments are allowed in `hf.*` and `sol.*` calls only, with the names those functions take, each once.

### E0522: variable needs an initial value

A `string` variable must be given a value when it is declared: `string s = ""`.

### E0523: struct where it cannot go

A struct is a value of fixed size kept where it is declared, so it cannot be passed to a function, returned, or
stored in a container or a class's field. Pass or return its fields, or make it a class.

### E0524: invalid field

A field that a struct or class cannot have: an array field in a struct, an object or container in a struct, or a
struct in a class.

### E0525: method does not match the one it overrides

A method with the same name as its parent class's method replaces it, so it must take and return the same types.

### E0526: invalid assignment

Something that cannot be assigned to as written, such as a whole array field (assign its elements instead).

### E0527: invalid use of a container method

A vector or map method used in a way it does not support: `sum()` on strings, `sort()` on objects, a method that
gives no value used as one, or a method the container does not have.

## Not supported yet

### E0601: not supported by this compiler yet

The program is valid Nova, but this compiler (or this back end, such as `nova-lower` to MLIR) does not cover that
part of the language yet.

## Runtime errors

A runtime error stops a running program. It is reported like a compile error, at the expression that failed, with
the calls that led there as notes ("called from here"), most recent first:

```
program.nova:2:14: runtime error[R0001]: integer division by zero
   2 |     return a / b
     |              ^
program.nova:5:11: note: called from here
   5 |     print(divide(10, 0))
     |           ^~~~~~
```

### R0001: integer division by zero

An `int` divided by zero (or `%` by zero). Check the divisor first. (A `float` or `double` divided by zero gives
infinity instead.)

### R0002: index out of range

An index past the end of an array, a vector or a tensor, or a tensor given the wrong number of indexes. Indexes start
at 0; the last is `size - 1`.

### R0003: tensors of incompatible shapes

An operation on tensors whose shapes do not fit: `+`, `-`, `*`, `/` need the same shape (or one that broadcasts),
and `A @ B` needs `A`'s columns to equal `B`'s rows.

### R0004: invalid size or dimension

A size that cannot be: a negative or zero array size, a negative tensor dimension, or a `tensor_reshape` whose new
shape does not hold the same number of elements.

### R0005: empty container

`min()`, `max()` or `pop()` on a vector with no elements.

### R0006: key not in the map

Reading `m[key]` for a key the map does not have. Check with `m.has(key)` first.

### R0007: object is none

A method or field of an object variable that was never set to an object, or was set to `none`.

### R0008: value used before it was set or after it was freed

A tensor, object or container used before it was given a value, or after the compiler freed it. If this happens in
code that looks correct, it is a compiler bug: please report it.

### R0009: object's class has no such method

A method called on an object whose actual class does not have it (with inheritance, a parent's variable can hold a
child's object, and the method is looked up on the child's class).

### R0010: execution limit reached

The program ran about 500 million simple instructions, which almost always means an infinite loop. (Tensor work
raises the limit, so long training runs are not stopped.)

### R0011: file could not be read

A file the program reads, such as a CSV with `tensor_load_csv`, does not exist or is not in the expected format.

## Warnings

### W0001: unused variable

A variable is declared but never used. Remove it, or start its name with `_` to say it is unused on purpose.

### W0002: unreachable code

A statement after `return`, `break` or `continue` in the same block never runs.

## The JSON format

With `--diagnostics=json` the compiler prints one JSON document on stderr, whether or not it found anything:

```json
{"version":1,
 "diagnostics":[
   {"severity":"error","code":"E0405","title":"variable declared twice",
    "message":"redeclaration of 'y'","file":"program.nova",
    "range":{"start":{"line":8,"column":9},"end":{"line":8,"column":10}},
    "caret":{"line":8,"column":9},
    "notes":[{"severity":"note","message":"first declared here","file":"program.nova",
              "range":{"start":{"line":7,"column":9},"end":{"line":7,"column":10}},
              "caret":{"line":7,"column":9}}],
    "fixes":[]}],
 "errors":1,"warnings":0,"truncated":false}
```

- Lines and columns start at 1, and columns count bytes. A range's `end` is just past its last character.
- `range` and `caret` are `null` for a diagnostic about no place in a file (a file that cannot be opened); a
  diagnostic about a whole line (a module that cannot be found) has the line and column 1.
- `fixes` are edits that correct the error (replace `range` with `replacement`), for an editor's quick fix.
- `truncated` is `true` when the error limit stopped the compiler from reporting more.
- `version` changes only if a field is removed or changes meaning; new fields can be added at any time.
