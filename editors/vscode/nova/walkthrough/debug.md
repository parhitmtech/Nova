# Debugging a program

Click in the margin to the left of a line number to set a **breakpoint** (a red dot), then press **F5**, or choose
**Debug File** from the ▷ menu at the top of the editor. The program runs until it reaches the breakpoint, and
stops there.

While it is stopped:

| | |
|---|---|
| **Variables** | Every variable in scope and its value. Expand an object to see its fields, a vector or map its elements, a tensor its rows |
| **Call Stack** | The calls that led here, innermost first; click one to see its variables |
| Hover | Hover over a name in the editor for its value; add it to **Watch** to follow it |
| **Debug Console** | What the program printed |

| Button | Key | Does |
|---|---|---|
| Continue | F5 | Runs to the next breakpoint |
| Step Over | F10 | Runs the current line, calls included |
| Step Into | F11 | Goes into the function the line calls |
| Step Out | Shift+F11 | Runs to the end of this function, and stops in its caller |
| Pause | F6 | Stops the running program where it is (a loop that does not end, say) |
| Stop | Shift+F5 | Ends the program |

Breakpoints can be set and removed while the program runs: they apply at once.

If the program calls `input()`, a box at the top of the window asks for the value. If it fails at run time (a
division by zero, an index out of range), it stops on the line that failed, with its variables, before it ends.
