import type * as MonacoType from 'monaco-editor'

// Registers the Nova language definition with Monaco exactly once.
// Called via the editor's beforeMount prop so it runs before the first render.
export function registerNovaLanguage(monaco: typeof MonacoType) {
  // Guard against double-registration if the component remounts (e.g. HMR).
  if (monaco.languages.getLanguages().some(l => l.id === 'nova')) return

  monaco.languages.register({ id: 'nova', extensions: ['.nova'] })

  // Monarch tokeniser — defines how the lexer classifies each token type.
  // Token names map to the colour rules in the theme defined below.
  monaco.languages.setMonarchTokensProvider('nova', {
    // Control flow, type names, and built-in literals
    keywords: [
      'for', 'if', 'elif', 'while', 'do', 'switch', 'case', 'default',
      'def', 'return', 'else', 'int', 'bool', 'string', 'float', 'double',
      'true', 'false', 'struct', 'class', 'extends', 'self', 'import',
      'and', 'or', 'not', 'void', 'null',
    ],
    // Built-in functions and ML primitives available without importing
    builtins: [
      'print', 'println', 'len', 'append', 'push', 'pop', 'main',
      'requires_grad', 'backward', 'tensor_load_csv', 'cross_entropy_loss',
      'mse_loss', 'normalize', 'one_hot', 'zeros', 'ones', 'rand',
    ],
    // ML layer and optimiser types (highlighted differently from plain identifiers)
    mlTypes: [
      'Linear', 'ReLU', 'Sigmoid', 'Tanh', 'SGD', 'DataLoader', 'Tensor',
      'Conv2d', 'MaxPool2d', 'BatchNorm',
    ],

    tokenizer: {
      root: [
        // Compiler directives like // @ml that enable GPU / ML execution paths
        [/\/\/\s*@\w+/, 'annotation'],
        // Single-line comments
        [/\/\/.*$/, 'comment'],
        // Import angle-bracket paths: import <stdlib/something>
        [/<[^>]+>/, 'string'],
        // Double-quoted string literals
        [/"[^"]*"/, 'string'],
        // Floating-point numbers (must come before integer rule)
        [/\d+\.\d*([eE][-+]?\d+)?/, 'number.float'],
        // Integer literals
        [/\d+/, 'number'],
        // Identifiers — matched against keyword/builtin/mlType lists first
        [/[a-zA-Z_]\w*/, {
          cases: {
            '@keywords': 'keyword',
            '@builtins': 'support.function',
            '@mlTypes':  'type.identifier',
            '@default':  'identifier',
          },
        }],
        // Brackets, semicolons, commas, dots
        [/[{}()\[\];,.]/, 'delimiter'],
        // Arithmetic, comparison, bitwise, and assignment operators
        [/[=<>!+\-*/%&|^~]/, 'operator'],
        // Whitespace (ignored)
        [/\s+/, 'white'],
      ],
    },
  } as MonacoType.languages.IMonarchLanguage)

  // Language configuration — enables bracket matching, auto-close, and
  // correct indent/dedent when the user types { or }.
  monaco.languages.setLanguageConfiguration('nova', {
    comments: { lineComment: '//' },
    brackets: [['(', ')'], ['{', '}'], ['[', ']']],
    autoClosingPairs: [
      { open: '(',  close: ')' },
      { open: '{',  close: '}' },
      { open: '[',  close: ']' },
      { open: '"',  close: '"' },
    ],
    indentationRules: {
      increaseIndentPattern: /\{[^}]*$/,  // indent after an opening brace
      decreaseIndentPattern: /^\s*\}/,    // dedent on a closing brace
    },
  })

  // Custom dark theme that matches the rest of the NovaComp UI palette.
  // Extends vs-dark so we only need to override the tokens we care about.
  monaco.editor.defineTheme('nova-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'keyword',          foreground: '00d4ff', fontStyle: 'bold' },
      { token: 'type.identifier',  foreground: 'ffb347' },   // ML types — orange
      { token: 'support.function', foreground: 'ffd700' },   // built-ins — gold
      { token: 'string',           foreground: '39ff8f' },   // strings — neon green
      { token: 'number',           foreground: 'ff9980' },
      { token: 'number.float',     foreground: 'ff9980' },
      { token: 'comment',          foreground: '3d4259', fontStyle: 'italic' },
      { token: 'annotation',       foreground: '7a8099', fontStyle: 'italic' }, // @ml etc.
      { token: 'delimiter',        foreground: 'c9d1d9' },
      { token: 'operator',         foreground: '58a6ff' },
      { token: 'identifier',       foreground: 'e8ecf5' },
    ],
    colors: {
      'editor.background':                 '#0d1117',
      'editor.foreground':                 '#e8ecf5',
      'editor.lineHighlightBackground':    '#161b2260',
      'editorCursor.foreground':           '#00d4ff',
      'editor.selectionBackground':        '#264f7880',
      'editorLineNumber.foreground':       '#3d4259',
      'editorLineNumber.activeForeground': '#7a8099',
      'editorIndentGuide.background1':     '#21262d',
      'editorBracketMatch.background':     '#264f7840',
      'editorBracketMatch.border':         '#00d4ff80',
    },
  })
}
