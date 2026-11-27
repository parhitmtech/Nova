# Nova Compiler

**Nova** is a compiled, statically typed programming language with Python's syntax, built for machine learning:
tensors, reverse-mode autograd and GPU training are part of the language. Every program is type-checked before it
runs, and it compiles through MLIR and LLVM to native code.

This extension brings Nova to VS Code, powered by Nova's own compiler: the errors you see as you type are exactly
the ones the compiler reports.

```nova
orbit novatorch.nn

def square(int x) -> int:
    return x * x

ignite:                           # the entry point
    Linear layer = Linear(2, 3)
    Tensor h = relu(layer.forward(randn(4, 2)))
    print(h.rows + square(3))     # 13
```

> **Preview.** The extension is a pre-release, for Linux (x64) and Windows (x64).

**New to Nova?** [Getting started](https://github.com/parhitmtech/Nova/blob/main/docs/GETTING_STARTED.md) takes
five minutes: a first program, running it, reading its errors, and the REPL.

## Features

- **Errors as you type**, from Nova's compiler: every error in the file at once, underlined exactly, each with a
  code (`E0502`) that links to its explanation. Warnings (unused variables, unreachable code) are faded.
- **Quick fixes** where the compiler knows the fix: a `;` to remove, a `//` comment to write with `#`, `!` to
  write as `not`.
- **Hover** over a name for its type, a function or method for its signature, a class for its fields.
- **Go to definition** (F12), into imported modules too.
- **Outline** of classes, their fields and methods, and functions (and Go to Symbol, Ctrl+Shift+O).
- **Completion** of the names in scope, keywords and built-ins, and after a `.` of the fields and methods of the
  value's class (or a vector's, a map's, a tensor's): even while the line is half written.
- **Signature help** in a call, with the argument being written.
- **Syntax highlighting**, also in Markdown code blocks (```` ```nova ````).
- **Run File** (Ctrl+F5, or ▷ at the top of the editor), **Check File** and the **REPL**, in VS Code's terminal,
  where an error's `file:line:column` is a link.
- **Snippets** for the entry point, functions, classes, loops and a training loop, and a getting-started
  walkthrough.

## Requirements

None on Linux (x64) and Windows (x64): the extension ships Nova's compiler (`nova`), its language server
(`nova-lsp`) and the standard library, built to run with nothing else installed.

To use tools you built yourself instead (a newer compiler, say), build them from
[the repository](https://github.com/parhitmtech/Nova):

```bash
make -C compiler          # compiler/build/nova
make -C compiler lsp      # compiler/build/nova-lsp
```

and set their paths in the settings below. Nova's standard library (`orbit
novatorch.nn`) is found through `nova.home`, or the `NOVA_HOME` environment variable: the `compiler` folder of
the repository, which holds `stdlib/`.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `nova.server.path` | the one shipped | The language server, `nova-lsp` |
| `nova.compiler.path` | the one shipped | The compiler, `nova`, for Run File and the REPL |
| `nova.home` | the one shipped | The folder that holds `stdlib/`, the standard library |
| `nova.warnings.enabled` | `true` | Show warnings as well as errors |
| `nova.trace.server` | `off` | Log the language server's messages in the Nova output |

`.nova` files are indented with four spaces (Nova does not allow tabs in indentation).

## Commands

| Command | Does |
|---|---|
| **Nova: Run File** | Runs the file in the Nova terminal |
| **Nova: Run File with Optimizations** | Runs it with the compiler's optimizations (`--optimize`) |
| **Nova: Check File** | Checks it without running it: every error, and the warnings |
| **Nova: Open REPL** | An interactive Nova session |
| **Nova: Restart Language Server** | Restarts `nova-lsp` (after rebuilding it, say) |
| **Nova: Show Output** | The extension's log |

## Workspace Trust

In a folder you have not trusted, the extension shows errors and completions but runs no program, and the folder's
settings cannot change which Nova tools are used.

## Troubleshooting

| What you see | What to do |
|---|---|
| No colors or errors in a `.nova` file | Check the language mode in the status bar (bottom right) says **Nova**. |
| "Nova's compiler (nova) was not found", or "Nova's language server (nova-lsp) was not found" | `nova.compiler.path` or `nova.server.path` names a file that does not exist: clear the setting, and the extension uses the tools it ships. |
| "Nova runs programs in trusted workspaces only" | Trust the folder to run programs (see Workspace Trust above). |
| Two Nova extensions installed | An early build from a `.vsix` had the ID `parhitmathur.nova`: uninstall it. |

For anything else, **Nova: Show Output** shows the extension's log; please include it when you
[report a problem](https://github.com/parhitmtech/Nova/issues).

## Quantum programming

Qubits, quantum gates, OpenQASM and compiling for neutral-atom hardware come in the companion extension,
**[Nova Quantum Compiler](https://marketplace.visualstudio.com/items?itemName=parhitmathur.nova-quantum)**.

## Links

- [Getting started](https://github.com/parhitmtech/Nova/blob/main/docs/GETTING_STARTED.md)
- The language: [the tour](https://github.com/parhitmtech/Nova/blob/main/docs/SYNTAX.md), [the grammar](https://github.com/parhitmtech/Nova/blob/main/docs/GRAMMAR.md), [every error and
  warning](https://github.com/parhitmtech/Nova/blob/main/docs/ERRORS.md)
- Source: [github.com/parhitmtech/Nova](https://github.com/parhitmtech/Nova)
- Report a bug: [issues](https://github.com/parhitmtech/Nova/issues)
- Security issues: see [SECURITY.md](https://github.com/parhitmtech/Nova/blob/main/SECURITY.md)

## Privacy

The extension collects no telemetry. Your code is read only by the Nova programs on your machine.

## License

[Apache 2.0](LICENSE)
