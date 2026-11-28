# Changelog

All notable changes to the Nova Compiler extension. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).
Odd minor versions (0.1, 0.3, 0.5) are pre-releases.

## [0.5.1] (pre-release)

### Added

- **Pause** (F6) stops a running program where it is, even in a loop on one line.
- Breakpoints set or removed while the program runs apply at once (they applied at its next stop).
- Stopping the debugger ends a running program at once, where it is.

## [0.5.0] (pre-release)

### Added

- **Debugging.** Press F5 on a `.nova` file (no `launch.json` needed), or use **Debug File**: breakpoints, Step
  Over, Into and Out, the call stack with each frame's variables (a recursive function's too), and every value,
  with an object's fields, a vector's or a map's elements and a tensor's rows to expand. Hover over a name, or
  watch an expression such as `p.x`, for its value.
- The program's output in the Debug Console, and `input()` asked for in a box.
- A runtime error stops the program on the line that failed, with its message, before it ends.
- A `nova` configuration type for `launch.json`, with `program`, `stopOnEntry` and `cwd`.
- A **Debug it** step in the getting-started walkthrough.

### Changed

- The walkthrough no longer starts by asking to build Nova's tools: the extension ships them. Setting your own
  builds is its last, optional step.

## [0.3.1] (pre-release)

### Fixed

- `print` works as Python's: `print("total:", total)` prints every value, with a space between them (only the first
  was printed), and `print()` prints an empty line (it stopped the compiler).
- `print(t)` of a tensor prints its rows again, not a number.
- The messages for a tool that is not found say to check or clear its setting: the extension ships its tools.

### Added

- A link to [Getting started](https://github.com/parhitmtech/Nova/blob/main/docs/GETTING_STARTED.md), and a
  Troubleshooting section, on the extension's page.

## [0.3.0] (pre-release)

### Added

- Packages for Linux (x64) and Windows (x64) that ship `nova`, `nova-lsp` and the standard library, linked to run
  with nothing else installed.
- The language server, `nova-lsp`, built on Nova's own compiler: errors and warnings as you type with their codes
  and links to their explanations, quick fixes, hover, go to definition, the outline, completion (also of members
  after `.`) and signature help.
- Syntax highlighting, also in Markdown code blocks.
- Commands: Run File (Ctrl+F5, and ▷ in the editor's title bar), Run File with Optimizations, Check File, Open
  REPL, Restart Language Server, Show Output.
- Settings for the paths of `nova-lsp`, `nova` and the standard library, and to hide warnings.
- Snippets, and a getting-started walkthrough.
- Workspace Trust: in an untrusted folder, no program runs and the tools' paths cannot be changed by the folder.
- `.nova` files are indented with four spaces.

### Changed

- Needs VS Code 1.91 or later.

### Fixed

- Activation no longer waits for the "nova-lsp was not found" message to be dismissed (an extension that depends on
  this one, such as Nova Quantum, waited until it was).

## [0.1.0] (pre-release)

### Added

- `.nova` files are recognized as the Nova language, with its icon.
- Language configuration: `#` comments, bracket matching and auto-closing, folding by indentation, and indentation
  after a line that ends in `:`.
