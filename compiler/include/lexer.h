#ifndef NOVA_LEXER_H
#define NOVA_LEXER_H
// The lexer. A Scanner reads one source buffer (source.h) a pointer at a time and writes compact tokens: a kind,
// a location, a length and, for names and string literals, an interned Symbol (interner.h). Layout is decided while 
// scanning, as Python's tokenizer does: the end of each logical line is a NEWLINE token, and a change of 
// indentation is INDENT / DEDENT. A '(' or '[' that is still open continues the line.
// 
// The current parser still reads the braces-and-semicolons form ("if (x) { y = 1 ; }") as Tokens with a string
// lexeme, so LexicalAnalyzer turns the scanner's tokens into that form for it. The new parser (roadmap phase 2)
// reads the scanner's tokens directly.

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

// The current parser's token: its kind, its text and its line.
class Token {
  public:
    void Print() const;

    std::string lexeme;
    TokenType token_type;
    int line_no;
    SourceLoc loc = 0;
};

class LexicalAnalyzer {
  public: 
    LexicalAnalyzer();
    void ReinitializeFromString(const std::string& s);  // scans s as the whole program
    const Token& GetToken();
    void UngetToken(int);
    const Token& peek(int);
    int GetPosition() const { return index; }  // where the next GetToken() reads ...
    void SetPosition(int i) { index = i; }  // ... and going back there (to compile a generic function again)
    const Token& TokenAt(int i) const { return tokenList[i]; }
    // A generic class's copy is compiled from its tokens with each type parameter replaced, appended after the 
    // program's tokens and removed again once compiled (an argument can be several tokens, e.g. vector<int>).
    int AppendTokens(const std::vector<Token>& tokens)
    {
      int at = (int)tokenList.size();
      tokenList.insert(tokenList.end(), tokens.begin(), tokens.end());
      return at;
    }
    void TruncateTokens(int size) { tokenList.resize(size); }
    const std::vector<LexToken>& Scanned() const { return scanned; }  // the scanner's tokens, for the parse tree

  private:
    std::vector<Token> tokenList;
    std::vector<LexToken> scanned;
    int index = 0;
    Token eof;    // what GetToken() / peek() give past the end
    void ToBracesForm(const std::vector<LexToken>& lexed);  // NEWLINE / INDENT / DEDENT -> ; { }
    void DesugarCompoundAssignments();  // x += e -> x = x + (e)
    void AddMainEntry();  // no ignite block but def main() -> ignite { main() ; }
};

#endif  // NOVA_LEXER_H