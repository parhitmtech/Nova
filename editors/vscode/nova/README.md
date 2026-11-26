# Nova Programming Language

> **Preview.** This is an early release that sets up the language in VS Code. Errors as you type, hover,
> go-to-definition, completion and running programs arrive in the next releases (see the roadmap below).

**Nova** is a compiled, statically typed programming language with Python's syntax, built for machine learning:
tensors, reverse-mode autograd and GPU training are part of the language. Every program is type-checked before it
runs, and it compiles through MLIR and LLVM to native code.

```python
orbit novatorch.nn

ignite:                           # the entry point
    Linear layer = Linear(2, 3)
    Tensor h = relu(layer.forward(randn(4, 2)))
    print(h.rows)                 # 4
```

## In this release

- `.nova` files are recognized as Nova.
- Comments (`#`), bracket matching and auto-closing, folding by indentation, and Python-style indentation after a
  line that ends in `:`.

## Roadmap

| | What it adds |
|---|---|
| Next | Syntax highlighting, errors as you type (from Nova's own compiler), hover with types, go to definition, outline, completion |
| Then | Run, REPL and step debugger from the editor; snippets; a getting-started walkthrough |
| 1.0 | Stable release on Linux and Windows |

Quantum programming (qubits, OpenQASM, compiling for neutral-atom hardware) comes in the companion extension,
**Nova Quantum**.

## Links

- Source and language docs: [github.com/parhitmtech/Nova](https://github.com/parhitmtech/Nova)
- Report a bug: [issues](https://github.com/parhitmtech/Nova/issues)
- Security issues: see [SECURITY.md](https://github.com/parhitmtech/Nova/blob/main/SECURITY.md)

## Privacy

This extension collects no telemetry.

## License

[Apache 2.0](LICENSE)
