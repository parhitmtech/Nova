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

    // keywords 
    FOR, IF, ELIF, WHILE, SWITCH, CASE, DEFAULT,
    INPUT, ARRAY, PRINT,
    DEF, RETURN, ELSE,
    AND, OR, NOT,

    // type keywords (new) 
    INT_TYPE, BOOL_TYPE, STRING_TYPE,
    TRUE, FALSE,

    // operators 
    PLUS, MINUS, DIV, MULT,

    // punctuation 
    EQUAL, COLON, COMMA, SEMICOLON,
    LBRAC, RBRAC, LPAREN, RPAREN, LBRACE, RBRACE,

    // comparison 
    NOTEQUAL, GREATER, LESS,

    // arrow operator (for function return type) 
    ARROW,

    // literals + misc 
    NUM, ID, STRING, ERROR

} TokenType;

class Token {
  public:
    void Print();

    std::string lexeme;
    TokenType token_type;
    int line_no;
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
    bool SkipSpace();
    int FindKeywordIndex(std::string);
    Token ScanIdOrKeyword();
    Token ScanNumber();
};

#endif  //__LEXER__H__