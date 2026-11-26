# Running a program

**Run File** (Ctrl+F5, or the ▷ button at the top right of the editor) saves the file and runs it with `nova` in
the **Nova** terminal. The program's output, and any error, appear there; an error's `file:line:column` is a link
to the place.

| Command | Does |
|---|---|
| **Nova: Run File** | Runs the file |
| **Nova: Run File with Optimizations** | Runs it with `--optimize` |
| **Nova: Check File** | Checks it without running it: every error, and the warnings |
| **Nova: Open REPL** | An interactive session: type statements and see their results |

A runtime error (a division by zero, an index out of range) points at the expression that failed, with the calls
that led there.
