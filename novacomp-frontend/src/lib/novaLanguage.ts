import type * as MonacoType from 'monaco-editor'

export function registerNovaLanguage(monaco: typeof MonacoType) {
  if (monaco.languages.getLanguages().some(l => l.id === 'nova')) return

  monaco.languages.register({ id: 'nova', extensions: ['.nova'] })

  monaco.languages.setMonarchTokensProvider('nova', {
    keywords: [
      'for', 'if', 'elif', 'while', 'do', 'switch', 'case', 'default',
      'def', 'return', 'else', 'int', 'bool', 'string', 'float', 'double',
      'true', 'false', 'struct', 'class', 'extends', 'self', 'import',
      'and', 'or', 'not', 'void', 'null',
    ],
    builtins: [
      'print', 'println', 'len', 'append', 'push', 'pop', 'main',
      'requires_grad', 'backward', 'tensor_load_csv', 'cross_entropy_loss',
      'mse_loss', 'normalize', 'one_hot', 'zeros', 'ones', 'rand',
    ],
    mlTypes: [
      'Linear', 'ReLU', 'Sigmoid', 'Tanh', 'SGD', 'DataLoader', 'Tensor',
      'Conv2d', 'MaxPool2d', 'BatchNorm',
    ],

    tokenizer: {
      root: [
        // ML annotations (// @ml)
        [/\/\/\s*@\w+/, 'annotation'],
        // Line comments
        [/\/\/.*$/, 'comment'],
        // Import angle-bracket paths
        [/<[^>]+>/, 'string'],
        // Strings
        [/"[^"]*"/, 'string'],
        // Float numbers
        [/\d+\.\d*([eE][-+]?\d+)?/, 'number.float'],
        // Integer numbers
        [/\d+/, 'number'],
        // Identifiers + keyword matching
        [/[a-zA-Z_]\w*/, {
          cases: {
            '@keywords': 'keyword',
            '@builtins': 'support.function',
            '@mlTypes': 'type.identifier',
            '@default': 'identifier',
          },
        }],
        // Delimiters
        [/[{}()\[\];,.]/, 'delimiter'],
        // Operators
        [/[=<>!+\-*/%&|^~]/, 'operator'],
        // Whitespace
        [/\s+/, 'white'],
      ],
    },
  } as MonacoType.languages.IMonarchLanguage)

  monaco.languages.setLanguageConfiguration('nova', {
    comments: { lineComment: '//' },
    brackets: [['(', ')'], ['{', '}'], ['[', ']']],
    autoClosingPairs: [
      { open: '(', close: ')' },
      { open: '{', close: '}' },
      { open: '[', close: ']' },
      { open: '"', close: '"' },
    ],
    indentationRules: {
      increaseIndentPattern: /\{[^}]*$/,
      decreaseIndentPattern: /^\s*\}/,
    },
  })

  monaco.editor.defineTheme('nova-dark', {
    base: 'vs-dark',
    inherit: true,
    rules: [
      { token: 'keyword',          foreground: '00d4ff', fontStyle: 'bold' },
      { token: 'type.identifier',  foreground: 'ffb347' },
      { token: 'support.function', foreground: 'ffd700' },
      { token: 'string',           foreground: '39ff8f' },
      { token: 'number',           foreground: 'ff9980' },
      { token: 'number.float',     foreground: 'ff9980' },
      { token: 'comment',          foreground: '3d4259', fontStyle: 'italic' },
      { token: 'annotation',       foreground: '7a8099', fontStyle: 'italic' },
      { token: 'delimiter',        foreground: 'c9d1d9' },
      { token: 'operator',         foreground: '58a6ff' },
      { token: 'identifier',       foreground: 'e8ecf5' },
    ],
    colors: {
      'editor.background':                '#0d1117',
      'editor.foreground':                '#e8ecf5',
      'editor.lineHighlightBackground':   '#161b2260',
      'editorCursor.foreground':          '#00d4ff',
      'editor.selectionBackground':       '#264f7880',
      'editorLineNumber.foreground':      '#3d4259',
      'editorLineNumber.activeForeground':'#7a8099',
      'editorIndentGuide.background1':    '#21262d',
      'editorBracketMatch.background':    '#264f7840',
      'editorBracketMatch.border':        '#00d4ff80',
    },
  })
}
