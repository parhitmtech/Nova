# Nova's grammar

This is the complete grammar of Nova: the tokens the scanner makes ([lexer.cc](../compiler/src/frontend/lexer.cc))
and the context-free grammar the parser accepts ([parse.cc](../compiler/src/frontend/parse.cc)). The parser is
written by hand, one function per rule below, so the grammar is read off it rule for rule; where the two could ever
disagree, the parser is right and this page is a bug.

The grammar says which programs are **well-formed**. Some well-formed programs are still rejected by the check pass,
for rules that a context-free grammar cannot state (a name must be declared before it is used, an argument must fit
its parameter's type, ...); [those rules](#rules-beyond-the-grammar) are listed at the end. Every error either stage
reports is in [ERRORS.md](ERRORS.md).

## Notation

The grammar is in EBNF:

| Notation | Means |
|---|---|
| `rule = ... ;` | a rule |
| `"def"` | that token, as written |
| `NAME`, `INT`, `NEWLINE`, ... | a token class (in capitals): see [Tokens](#tokens) |
| `a b` | a, then b |
| `a \| b` | a or b |
| `[ a ]` | a, or nothing |
| `{ a }` | a, any number of times (none included) |
| `( a )` | grouping |

## Tokens

### Layout: NEWLINE, INDENT and DEDENT

Nova's blocks are made by indentation, as Python's are. Indentation is not context-free (a line's meaning depends
on how far earlier lines were indented), so the scanner turns it into three tokens, and the grammar is context-free
over the scanner's tokens:

- **`NEWLINE`** ends a logical line. A line inside an open `(` or `[` continues the logical line, however it is
  indented; a blank line, or one that holds only a comment, is no line at all.
- **`INDENT`** comes before the first token of a line indented further than the line before it.
- **`DEDENT`** comes before the first token of a line indented less than the line before it, once for each block
  that line closes. The line must line up with one of the blocks still open. At the end of the file, every block
  still open is closed.

Indentation is spaces only (a tab is error E0101). A block is opened by a logical line whose last token is `:` and
whose only `:` outside brackets that is; the block's lines are indented further than that line, by any amount.

### Names, numbers and strings

```ebnf
NAME   = letter { letter | digit | "_" } | "_" { letter | digit | "_" } ;   (* and not a keyword *)
letter = "a" ... "z" | "A" ... "Z" ;
digit  = "0" ... "9" ;

INT    = digit { digit } ;
FLOAT  = digit { digit } "." { digit } [ exponent ]
       | digit { digit } exponent ;
exponent = ( "e" | "E" ) [ "+" | "-" ] { digit } ;

STRING = '"' { any character except '"' and a line break } '"' ;
```

- Names are ASCII. Case matters (`Tensor` is a type, `tensor` a name).
- A number has no sign (`-1` is unary minus applied to `1`), no `_` separators and no hexadecimal form. A `.` or an
  exponent makes it a `FLOAT` (`2.`, `1e3`), whose type is `float` (it can also set a `double`).
- A string has no escape sequences: `"a\nb"` is the four characters `a`, `\`, `n`, `b`, and a string cannot hold a
  `"` or span lines.

### Keywords

```
ignite  def     struct  class   extends self    return
if      elif    else    while   do      for     switch  case    default break   continue
and     or      not     true    false
int     float   double  bool    string  Tensor
print   input   array   import
```

`pass` and `none` are not keywords: `pass` is a statement when it is the only word on its line, and `none` is a name
the compiler declares (the object that is no object). `vector`, `map`, `hf` and `sol` are names the compiler
declares too, and `orbit` is read before the scanner runs (see [Modules](#modules)). `import` is reserved so that
it can be reported (E0303).

### Operators and punctuation

```
+   -   *   /   %   @   =   ==  !=  <>  <   >   <=  >=
+=  -=  *=  /=  ->  :   ,   .   (   )   [   ]   ;
```

- `<>` is an older spelling of `!=`.
- `;` appears only between the parts of a `for` header (inside its brackets). Anywhere else it is error E0108.
- `#` starts a comment that runs to the end of the line.

## Programs

```ebnf
program      = { definition | ignite_block } ;          (* then the end of the file *)
definition   = function | struct | class ;
ignite_block = "ignite" block ;
```

A file is a list of definitions and its entry point, the `ignite` block. The order and the count are checked after
parsing: one entry point, after every definition (or a `def main()` with no parameters instead).

### Functions

```ebnf
function     = "def" NAME "(" [ parameter { "," parameter } ] ")" [ "->" type { "," type } ] block ;
parameter    = "self"
             | NAME                                      (* untyped: the function is generic *)
             | type NAME ;
```

A parameter is untyped when it is a lone name, the next token being `,` or `)`. A function with an untyped parameter
is generic: it is compiled once for each combination of argument types it is called with. `-> int, int` declares a
function that returns two values.

### Structs and classes

```ebnf
struct       = "struct" NAME members ;
class        = "class" NAME [ type_params ] [ "extends" type ] members ;
type_params  = "<" NAME { "," NAME } ">" ;
members      = ":" NEWLINE INDENT member { member } DEDENT ;
member       = "pass" NEWLINE
             | field
             | function ;                                (* in a class only *)
field        = type NAME [ "[" expression "]" ] NEWLINE ;  (* int data[256]: an array field *)
```

A struct holds fields; a class holds fields and methods (`def`), and its `init` method is its constructor. A class
with type parameters (`class Box<T>`) is generic.

### Types

```ebnf
type         = type_name [ "<" type { "," type } ">" ] ;
type_name    = "int" | "float" | "double" | "bool" | "string" | "Tensor" | NAME ;
```

`vector<int>`, `map<string, vector<int>>`, `Box<Point>`. `>>` is never one token, so nested type arguments need no
space.

## Statements

```ebnf
block        = ":" NEWLINE INDENT statement { statement } DEDENT
             | ":" simple_statement NEWLINE ;            (* if (x > 0): y = 1 *)

statement    = if_statement
             | while_statement
             | do_while_statement
             | for_statement
             | switch_statement
             | simple_statement NEWLINE ;

simple_statement
             = "return" [ expression { "," expression } ]
             | "break"
             | "continue"
             | "pass"
             | declaration
             | expression [ assign_op expression ] ;

assign_op    = "=" | "+=" | "-=" | "*=" | "/=" ;

declaration  = type NAME [ "=" expression ]
             | type NAME "," type NAME { "," type NAME } "=" expression ;   (* int q, int r = divmod(17, 5) *)
```

- A block on the same line as its `:` holds one simple statement: `if (x): y = 1` (an `if`, a loop or a `switch`
  there is error E0207).
- A statement is a **declaration** when it starts with a type followed by a name (`int x`, `Point p`,
  `vector<int> v`); otherwise it is an expression, an assignment, or an expression statement such as a call.
- `x = 5` assigns to `x`, or declares it when no variable `x` is visible (its type is the value's).
- The left side of `=` and of `+=` is any expression in the grammar; the check pass accepts a name, a field
  (`p.x`), an element (`v[i]`, `t[i, j]`) or `self.field`.

### Control flow

```ebnf
if_statement       = "if" condition block { "elif" condition block } [ "else" block ] ;
while_statement    = "while" condition block ;
do_while_statement = "do" block "while" condition NEWLINE ;
for_statement      = "for" "(" [ simple_statement ] ";" [ expression ] ";" [ simple_statement ] ")" block ;
switch_statement   = "switch" condition ":" NEWLINE INDENT { case } DEDENT ;
case               = "case" expression block
                   | "default" block ;
condition          = "(" expression ")" ;
```

The condition of `if`, `elif`, `while` and `switch` is always in parentheses (error E0205 without them). Any part of
a `for` header can be left out: `for (;;):` loops until a `break`.

## Expressions

```ebnf
expression     = or_expr ;
or_expr        = and_expr { "or" and_expr } ;
and_expr       = not_expr { "and" not_expr } ;
not_expr       = "not" not_expr
               | comparison ;
comparison     = additive [ compare_op additive ] ;        (* at most one: a < b < c is error E0206 *)
compare_op     = "==" | "!=" | "<>" | "<" | ">" | "<=" | ">=" ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" | "%" | "@" ) unary } ;
unary          = "-" unary
               | postfix ;
postfix        = primary { call | index | member } ;
call           = "(" [ argument { "," argument } ] ")" ;
index          = "[" expression { "," expression } "]" ;  (* t[i, j]: one index per dimension *)
member         = "." NAME ;
argument       = NAME "=" expression                       (* a keyword argument: hf.generate(m, p, max_tokens=50) *)
               | expression ;

primary        = INT | FLOAT | STRING
               | "true" | "false"
               | "self"
               | "print" | "input" | "array"               (* built-ins called as functions are *)
               | "(" expression ")"
               | NAME
               | type ;                                    (* only before "(": a conversion or a constructor *)
```

From loosest to tightest:

| Level | Operators | Grouping |
|---|---|---|
| 1 | `or` | left to right |
| 2 | `and` | left to right |
| 3 | `not` (prefix) | `not a == b` is `not (a == b)` |
| 4 | `==` `!=` `<>` `<` `>` `<=` `>=` | not chained |
| 5 | `+` `-` | left to right |
| 6 | `*` `/` `%` `@` | left to right (`@` is matrix multiplication) |
| 7 | `-` (prefix) | `-a * b` is `(-a) * b` |
| 8 | calls `f(x)`, indexes `t[i]`, members `a.b` | left to right, in any order: `make().x`, `l.ws[1][0, 0]` |

A type is an expression only right before `(`: `float(x)` and `int(y)` convert, and `Point(1, 2)`,
`Box<int>(5)` and `vector<int>()` construct.

## Lookahead and ambiguity

The grammar is not LL(1) in two places. The parser decides each by reading ahead over a type, without building
anything (as C# does for generic calls):

- **Declaration or expression?** A statement starting with `Box<int> b` is a declaration, and `a < b` is a
  comparison. The parser checks whether a type starts at the first token and is followed by a name.
- **Type arguments or less-than?** In an expression, `NAME "<" ... ">"` is a type, and so a constructor call, only
  when the `>` that closes it is followed by `(`: `Box<int>(5)` is a call, and `a < b` stays a comparison.

There are no other ambiguities. `else` and `elif` belong to the `if` at their own indentation (indentation removes
the dangling-else problem), and the precedence table fixes how every expression groups.

## Modules

`orbit` lines are read before the scanner runs, by a preprocessor that inlines the modules they name:

```ebnf
orbit_line   = "orbit" NAME { "." NAME } ;                 (* on a line of its own *)
```

`orbit a.b` is `a/b.nova` next to the file, or else `a.b.nova` in the standard library (`$NOVA_HOME/stdlib`). Each
module is inlined once, without its `ignite` block. `orbit hf` and `orbit sol` only say that the program uses those
built-in modules.

## Rules beyond the grammar

The check pass rejects well-formed programs that break these rules (each with its code in
[ERRORS.md](ERRORS.md)):

- **Program shape:** one entry point, `ignite` (or `def main()`), after every definition; no name defined twice.
- **Names:** every name is declared before it is used, in a block that encloses the use; a variable is not declared
  again while it is visible (no shadowing); `return` only in a function, `break` and `continue` only in a loop,
  `self` only in a method.
- **Types:** every expression's type is worked out, and must fit where it is used: an assignment, an argument, a
  return value, a condition (a `bool` or a number, never a `string`), an operator's operands, an index.
- **Generics:** a generic class or container is written with its type arguments; a generic function is checked
  again for each combination of argument types it is called with.
- **Structs** are values: they cannot be passed, returned, compared, or held by a container or a class.
- **Overrides:** a method that overrides its parent class's takes and returns the same types.
