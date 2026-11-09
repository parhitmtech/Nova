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

// Scans one buffer, start.. '\0', into tokens. Layout and lexical errors stop compilation with the line, as the 
// parser's errors do.
class Scanner 
{
  public:
    Scanner(SourceLoc start, const char* text) : base(start), begin(text), p(text) {}
    void scan(std::vector<LexToken>& out);

  private:
    void begin_line(std::vector<LexToken>& out);  // at a line start: indentation and blank lines
    void scan_token(std::vector<LexToken>& out);
    void push(std::vector<LexToken>& out, TokenType kind, const char* start, Symbol symbol = 0);
    SourceLoc loc_of(const char* q) const { return base + (SourceLoc)(q - begin); }

    SourceLoc base;
    const char* begin;
    const char* p;
    int line = 1;
    std::vector<int> indents = {0};  // indentation of each open block, outermost first
    std::vector<int> openBrackets;   // line of each '(' / '[' still open
    bool needBody = false;           // the last logical line ended with ':', so the next one must be indented
    int headerLine = 0;
};

// Scans a whole program into tokens: the source becomes one buffer of sourceManager ("<input>").
std::vector<LexToken> scan_program(const std::string& source);

#endif  // NOVA_LEXER_H