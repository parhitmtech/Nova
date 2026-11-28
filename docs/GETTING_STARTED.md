# Getting started with Nova

From nothing to a running Nova program in VS Code, in about five minutes. You need VS Code (or VSCodium, Cursor,
or any editor that installs from Open VSX) on Linux (x64) or Windows (x64). Nothing else: the extension ships
Nova's compiler, its language server and the standard library.

## 1. Install the extension

In VS Code, open the Extensions view (**Ctrl+Shift+X**), search for **Nova Compiler**, and click **Install**. Or,
in a terminal:

```bash
code --install-extension parhitmathur.nova-programming-language --pre-release
```

It is on the [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=parhitmathur.nova-programming-language)
and on [Open VSX](https://open-vsx.org/extension/parhitmathur/nova-programming-language). The current release is a
pre-release: if VS Code asks, choose the pre-release version.

> Installed an earlier build from a `.vsix` file, with the ID `parhitmathur.nova`? Uninstall it first, so that only
> one Nova extension is active.

## 2. Write a program

Create a file `hello.nova` and type:

```nova
def square(int x) -> int:
    return x * x

ignite:
    int total = 0
    for (int i = 1; i <= 4; i += 1):
        total += square(i)
    print(total)
```

Nova reads like Python, with types: blocks are indented with four spaces, `int x` declares a variable with its
type, and a function states its parameter and return types. `ignite:` is where the program starts, like `main`.

As you type, the extension completes names, shows a function's signature as you call it, and shows a name's type
when you hover over it.

## 3. Run it

Press **Ctrl+F5**, or click **▷** at the top of the editor. The program runs in the **Nova** terminal:

```
30
```

**Nova: Check File** in the command palette (**Ctrl+Shift+P**) checks the file without running it.

### Step through it

Click left of line 2's number to set a breakpoint (a red dot), then press **F5**. The program stops in `square`,
before line 2 runs:

- **Variables** shows `x = 1`, and **Call Stack** shows `square` called from `ignite` on line 7; click `ignite` to
  see its `total = 0` and `i = 1`.
- **F5** runs to the breakpoint again: `x` is 2, then 3, then 4, while `ignite`'s `total` grows from 0 to 1, 5
  and 14.
- **F10** (Step Over) runs a line; **F11** (Step Into) goes into a call; **Shift+F11** (Step Out) returns to the
  caller. Hover over a variable in the editor to see its value.

A program that fails at run time stops on the line that failed, with its variables, so you can see why.

## 4. Read an error

Add a semicolon to the loop's body, `total += square(i);`. A red underline appears under it. Hover over it, or
open the **Problems** panel (**Ctrl+Shift+M**):

```
hello.nova:7:27: error[E0108]: Nova does not use ';' to end statements: start a new line instead
```

Every error has a code. Click it (`E0108`) for a page that explains the error and how to fix it; [ERRORS.md](ERRORS.md)
lists them all.

Where the compiler knows the fix, it offers it: put the cursor on the `;` and press **Ctrl+.**, then choose
**Remove the ';'**.

Now misspell the variable in the last line, `print(totl)`:

```
hello.nova:8:11: error[E0406]: undeclared variable 'totl'
```

Mistakes in the syntax, like the `;`, are reported first, every one of them in the file at once. Names and types
are checked once the file's syntax is right, so an error like `totl` shows up after the syntax errors are fixed.

If you run a program with errors, the terminal shows the same errors with the source line, and each
`file:line:column` is a link to the place.

## 5. Try things in the REPL

**Nova: Open REPL** starts an interactive session in a terminal. Type a statement and press Enter:

```
>>> int x = 6
>>> print(x * 7)
42
>>> Tensor t = ones(2, 3)
>>> print(t.rows)
2
```

Type `quit` to leave.

## 6. Tensors and training

Tensors, automatic differentiation and neural-network layers are part of the language and its standard library:

```nova
orbit novatorch.nn

ignite:
    Linear layer = Linear(2, 3)
    Tensor h = relu(layer.forward(randn(4, 2)))
    print(h.rows)                 # 4

    Tensor W = randn(3, 2)
    requires_grad(W)
    Tensor loss = mse_loss(ones(4, 3) @ W, zeros(4, 2))
    backward(loss)                # reverse-mode autograd
    print(tensor_grad(W).rows)    # 3
```

`orbit` imports a module: from the standard library, as here, or one of your own files. Type `train` and accept
the snippet for a complete training loop.

## Where next

- **The language:** [SYNTAX.md](SYNTAX.md) is the tour; [GRAMMAR.md](GRAMMAR.md) is the complete grammar.
- **Errors:** [ERRORS.md](ERRORS.md) explains every error and warning by its code.
- **Quantum programs:** the companion extension **Nova Quantum Compiler** adds qubits, quantum gates and OpenQASM,
  compiled for neutral-atom hardware ([NovaQuantum](https://github.com/parhitmtech/NovaQuantum)).
- **Building from source:** [SETUP.md](SETUP.md).

## If something does not work

| What you see | What to do |
|---|---|
| No colors or errors in a `.nova` file | Check the language mode in the status bar (bottom right) says **Nova**. |
| "Nova's compiler (nova) was not found", or "Nova's language server (nova-lsp) was not found" | `nova.compiler.path` or `nova.server.path` names a file that does not exist: clear the setting, and the extension uses the tools it ships. |
| "Nova runs programs in trusted workspaces only" | Nova runs no program in a folder you have not trusted (**Workspace Trust**): trust the folder to run programs. Errors and completion work either way. |
| "not available for your platform" | The extension supports Linux (x64) and Windows (x64) for now. |
| The debugger does not stop at a breakpoint set while the program runs | It takes effect at the program's next stop. Set breakpoints before you press F5, or step to reach them. |
| Anything else | **Nova: Show Output** shows the extension's log. Please [report it](https://github.com/parhitmtech/Nova/issues) with that log. |
