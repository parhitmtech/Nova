# Nova's tools

The extension uses two programs from Nova's compiler:

- **`nova-lsp`**, the language server: errors as you type, hover, go to definition, completion.
- **`nova`**, the compiler and interpreter: **Run File** and the REPL.

Build them from the repository:

```bash
git clone https://github.com/parhitmtech/Nova.git
make -C Nova/compiler          # compiler/build/nova
make -C Nova/compiler lsp      # compiler/build/nova-lsp
```

Then set, in VS Code's settings:

| Setting | Value |
|---|---|
| `nova.server.path` | `.../Nova/compiler/build/nova-lsp` |
| `nova.compiler.path` | `.../Nova/compiler/build/nova` |
| `nova.home` | `.../Nova/compiler` (it holds `stdlib/`, the standard library) |

If the programs are on your `PATH`, and `NOVA_HOME` is set, no setting is needed.
