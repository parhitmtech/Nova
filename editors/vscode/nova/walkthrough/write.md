# A Nova program

Nova reads like Python, with types written before names. A program starts at its `ignite:` block, at the end of
the file:

```nova
def square(int x) -> int:
    return x * x

ignite:
    int n = 7
    print(square(n))
```

- Blocks are a line ending in `:` and the indented lines under it (four spaces).
- Conditions are in parentheses: `if (n > 0):`.
- `orbit novatorch.nn` brings in a module of the standard library.

As you type, errors are underlined; hover over one to read it, and follow its code (`E0502`) for an explanation.
Hover over a name to see its type, and press F12 to go to where it is defined.
