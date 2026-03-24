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

// ------- token types -------------------

typedef enum { END_OF_FILE = 0,

    // ── keywords ──────────────────────────────────────────────────────────────
    // old: VAR, FOR, IF, WHILE, SWITCH, CASE, DEFAULT, INPUT, OUTPUT, ARRAY,
    //      PRINT, PRINTLN, FUNC, RETURN, ELSE, AND, OR, NOT
    // new: lowercase python-like keywords, VAR removed, OUTPUT/PRINTLN removed,
    //      FUNC → DEF, ELSE IF → ELIF added

    FOR, IF, ELIF, WHILE, SWITCH, CASE, DEFAULT,
    INPUT, ARRAY, PRINT,
    DEF, RETURN, ELSE,
    AND, OR, NOT,

    // ── operators ─────────────────────────────────────────────────────────────
    PLUS, MINUS, DIV, MULT,

    // ── punctuation ───────────────────────────────────────────────────────────
    EQUAL, COLON, COMMA, SEMICOLON,
    LBRAC, RBRAC, LPAREN, RPAREN, LBRACE, RBRACE,

    // ── comparison ────────────────────────────────────────────────────────────
    NOTEQUAL, GREATER, LESS,

    // ── literals + misc ───────────────────────────────────────────────────────
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
    void Initialize();  // reads all tokens after freopen (CLI fix)
    void InitializeFromFile(const string& filename);  // to handle input from file
    void ReinitializeFromString(const std::string& s);  // to handle REPL mode

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