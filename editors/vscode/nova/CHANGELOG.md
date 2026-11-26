# Changelog

All notable changes to the Nova Programming Language extension. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).
Odd minor versions (0.1, 0.3) are pre-releases.

## [0.3.0] (pre-release)

### Added

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

## [0.1.0] (pre-release)

### Added

- `.nova` files are recognized as the Nova language, with its icon.
- Language configuration: `#` comments, bracket matching and auto-closing, folding by indentation, and indentation
  after a line that ends in `:`.
