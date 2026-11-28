# Nova's tools

Nothing to set up: the extension ships Nova's tools, built to run with nothing else installed on Linux (x64) and
Windows (x64).

- **`nova-lsp`**, the language server: errors as you type, hover, go to definition, completion.
- **`nova`**, the compiler and interpreter: **Run File**, the debugger and the REPL.
- The standard library, which `orbit` imports from.

To use tools you built yourself instead (a newer compiler, say), build them from the repository:

```bash
git clone https://github.com/parhitmtech/Nova.git
make -C Nova/compiler          # compiler/build/nova
make -C Nova/compiler lsp      # compiler/build/nova-lsp
```

and set, in VS Code's settings:

| Setting | Value |
|---|---|
| `nova.server.path` | `.../Nova/compiler/build/nova-lsp` |
| `nova.compiler.path` | `.../Nova/compiler/build/nova` |
| `nova.home` | `.../Nova/compiler` (it holds `stdlib/`, the standard library) |

Clear a setting to go back to the tool the extension ships.
