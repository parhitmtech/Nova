/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#ifndef __LEXER__H__
#define __LEXER__H__

#include <vector>
#include <string>

#include "inputbuf.h"

using namespace std;

// token types 

typedef enum { END_OF_FILE = 0,

    // entry point: ignite { ... }
    IGNITE,

    // keywords 
    FOR, IF, ELIF, WHILE, DO, SWITCH, CASE, DEFAULT,
    INPUT, ARRAY, PRINT,
    DEF, RETURN, ELSE,
    AND, OR, NOT,

    // type keywords (new) 
    INT_TYPE, BOOL_TYPE, STRING_TYPE,
    FLOAT_TYPE, DOUBLE_TYPE,
    TRUE, FALSE,
    STRUCT, CLASS, EXTENDS, SELF, IMPORT,

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
    GREATEREQ, LESSEQ, MOD,
    PLUSEQ, MINUSEQ, MULTEQ, DIVEQ,

    // literals + misc 
    NUM, FLOAT_LITERAL, ID, STRING, ERROR

} TokenType;

class Token {
  public:
    void Print();

    std::string lexeme;
    TokenType token_type;
    int line_no;
    int indent = -1;  // first token on its line: the line's indentation in spaces; otherwise -1
};

class LexicalAnalyzer {
  public:
    Token GetToken();
    void UngetToken(int);
    Token peek(int);
    LexicalAnalyzer();
    void Initialize();
    void InitializeFromFile(const std::string& filename);
    void ReinitializeFromString(const std::string& s);

  private:
    std::vector<Token> tokenList;
    Token GetTokenMain();
    int line_no;
    int index;
    Token tmp;
    InputBuffer input;
    bool initialized = false;
    bool atLineStart = true;   // no token read yet on the current line
    int column = 0;            // spaces since the start of the current line
    bool tabInIndent = false;  // a tab in the current line's indentation (not allowed)
    bool SkipSpace();
    void ApplyLayout();        // indentation, ':' and line ends  ->  { } and ;
    bool NextIs(char c);  // consume the next character if it is c
    void DesugarCompoundAssignments();  // x += e  ->  x = x + (e)
    void AddMainEntry();                // no ignite block but def main()  ->  ignite { main() ; }
    int FindKeywordIndex(std::string);
    Token ScanIdOrKeyword();
    Token ScanNumber();
};

#endif  //__LEXER__H__