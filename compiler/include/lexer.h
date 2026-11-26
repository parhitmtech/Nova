#ifndef NOVA_LEXER_H
#define NOVA_LEXER_H
// The lexer. A Scanner reads one source buffer (source.h) a pointer at a time and writes compact tokens: a kind,
// a location, a length and, for names and string literals, an interned Symbol (interner.h). Layout is decided while 
// scanning, as Python's tokenizer does: the end of each logical line is a NEWLINE token, and a change of 
// indentation is INDENT / DEDENT. A '(' or '[' that is still open continues the line. The parser (parse_tree.h)
// reads these tokens.

#include <string>
#include <vector>
#include "interner.h"
#include "diagnostics.h"
#include "source.h"

using namespace std;

typedef enum { END_OF_FILE = 0,

  // entry point: ignite { ... }
  IGNITE,

  // keywords
  FOR, IF, ELIF, WHILE, DO, SWITCH, CASE, DEFAULT,
  INPUT, ARRAY, PRINT, 
  DEF, RETURN, ELSE,
  AND, OR, NOT,

  // type keywords
  INT_TYPE, BOOL_TYPE, STRING_TYPE,
  FLOAT_TYPE, DOUBLE_TYPE, TENSOR_TYPE,
  TRUE, FALSE,
  STRUCT, CLASS, EXTENDS, SELF, IMPORT,
  BREAK, CONTINUE,

  // operators
  PLUS, MINUS, DIV, MULT,

  // punctuation
  EQUAL, COLON, COMMA, SEMICOLON,
  LBRAC, RBRAC, LPAREN, RPAREN, LBRACE, RBRACE,

  // comparison
  NOTEQUAL, GREATER, LESS, EQEQ,

  // arrow operator (for function return type)
  ARROW, 
  DOT,

  // more comparisons, remainder and compound assignment
  GREATEREQ, LESSEQ, MOD, AT,
  PLUSEQ, MINUSEQ, MULTEQ, DIVEQ,

  // literals + misc
  NUM, FLOAT_LITERAL, ID, STRING, ERROR,

  // layout, made by the scanner: the end of a logical line, and a block's start and end
  NEWLINE, INDENT, DEDENT

} TokenType;

// A token as the scanner writes it: no strings. A name's or string literal's text is names.text(symbol); any
// token's spelling is its length characters at loc (sourceManager.text_at(loc)).
struct LexToken
{
  TokenType kind;
  SourceLoc loc;
  uint32_t length;
  Symbol symbol;  // ID, keywords and STRING (the text between the quotes); 0 otherwise
  int line;       // the line as a person reads it (after "#@line" directives)
};

// Scans one buffer of size bytes (followed by a '\0') into tokens. Errors are reported to the diagnostics engine
// (diagnostics.h) and scanning goes on, so one run finds every lexical error: a character Nova does not use is
// skipped, '!' reads as 'not', an unterminated string ends at the end of its line, and a bracket left open is closed
// where a line plainly starts a new statement. The parser reports what is wrong with the layout itself (a missing
// or unexpected indented block), where it can say what it expected.
class Scanner
{
  public:
    Scanner(SourceLoc start, const char* text, size_t size) : base(start), begin(text), end(text + size), p(text) {}
    void scan(std::vector<LexToken>& out);

  private:
    struct Bracket
    {
      const char* at;
      char c;
      int indent;  // the indentation of the line it opened on
    };
    void begin_line(std::vector<LexToken>& out);  // at a line start: indentation and blank lines
    void end_line(std::vector<LexToken>& out);    // the end of a logical line: NEWLINE, and a block if it ended with ':'
    void scan_token(std::vector<LexToken>& out);
    void push(std::vector<LexToken>& out, TokenType kind, const char* start, Symbol symbol = 0);
    void comment();                               // p at '#': to the end of the line, reading a "#@line" directive
    bool starts_statement(const char* q) const;   // a keyword that only starts a statement or a definition is at q
    void error(DiagCode code, const char* at, uint32_t length, const std::string& message);
    SourceLoc loc_of(const char* q) const { return base + (SourceLoc)(q - begin); }

    SourceLoc base;
    const char* begin;
    const char* end;
    const char* p;
    int line = 1;
    std::vector<int> indents = {0};     // indentation of each open block, outermost first
    std::vector<Bracket> openBrackets;  // each '(' / '[' still open
    bool needBody = false;              // the last logical line ended with ':', so the next one must be indented
    bool tabReported = false;           // a tab in indentation is reported once
    size_t lineStart = 0;               // the first token of the current logical line
    int colons = 0;                     // ':' outside brackets on the current logical line
};

// Scans a whole program into tokens: the source becomes one buffer of sourceManager, named name (the file name
// diagnostics print).
std::vector<LexToken> scan_program(const std::string& source, const std::string& name = "<input>");

#endif  // NOVA_LEXER_H