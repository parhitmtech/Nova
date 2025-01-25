/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
#include <iostream>
#include <istream>
#include <vector>
#include <string>
#include <cctype>

#include "lexer.h"
#include "inputbuf.h"

using namespace std;

string reserved[] = { "END_OF_FILE",
    "VAR", "FOR", "IF", "WHILE", "SWITCH", "CASE", "DEFAULT", "INPUT", "OUTPUT", "ARRAY",
    "PLUS", "MINUS", "DIV", "MULT",
    "EQUAL", "COLON", "COMMA", "SEMICOLON",
    "LBRAC", "RBRAC", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
    "NOTEQUAL", "GREATER", "LESS",
    "NUM", "ID", "ERROR"
};

#define KEYWORDS_COUNT 9

void Token::Print()
{
    cout << "{" << this->lexeme << " , "
         << reserved[(int) this->token_type] << " , "
         << this->line_no << "}\n";
}

LexicalAnalyzer::LexicalAnalyzer()
{
    this->line_no = 1;
    tmp.lexeme = "";
    tmp.line_no = 1;
    tmp.token_type = ERROR;

    Token token = GetTokenMain();
    index = 0;

    while (token.token_type != END_OF_FILE)
    {
        tokenList.push_back(token);     // push token into internal list
        token = GetTokenMain();        // and get next token from standatd input
    }
    // pushes END_OF_FILE is not pushed on the token list
}

bool LexicalAnalyzer::SkipSpace()
{
    char c;
    bool space_encountered = false;

    input.GetChar(c);
    line_no += (c == '\n');

    while (!input.EndOfInput() && isspace(c)) {
        space_encountered = true;
        input.GetChar(c);
        line_no += (c == '\n');
    }

    if (!input.EndOfInput()) {
        input.UngetChar(c);
    }
    return space_encountered;
}

int LexicalAnalyzer::FindKeywordIndex(string s)
{
    string keyword[] = { "VAR", "FOR", "IF", "WHILE", "SWITCH", "CASE", "DEFAULT", "input", "output", "ARRAY" };
    for (int i = 0; i < KEYWORDS_COUNT; i++) {
        if (s == keyword[i]) {
            return i + 1;
        }
    }
    return -1;
}

Token LexicalAnalyzer::ScanNumber()
{
    char c;

    input.GetChar(c);
    if (isdigit(c)) {
        if (c == '0') {
            tmp.lexeme = "0";
        } else {
            tmp.lexeme = "";
            while (!input.EndOfInput() && isdigit(c)) {
                tmp.lexeme += c;
                input.GetChar(c);
            }
            if (!input.EndOfInput()) {
                input.UngetChar(c);
            }
        }
        tmp.token_type = NUM;
        tmp.line_no = line_no;
        return tmp;
    } else {
        if (!input.EndOfInput()) {
            input.UngetChar(c);
        }
        tmp.lexeme = "";
        tmp.token_type = ERROR;
        tmp.line_no = line_no;
        return tmp;
    }
}

Token LexicalAnalyzer::ScanIdOrKeyword()
{
    char c;
    input.GetChar(c);

    if (isalpha(c)) {
        tmp.lexeme = "";
        while (!input.EndOfInput() && isalnum(c)) {
            tmp.lexeme += c;
            input.GetChar(c);
        }
        if (!input.EndOfInput()) {
            input.UngetChar(c);
        }
        tmp.line_no = line_no;
        int keywordIndex = FindKeywordIndex(tmp.lexeme);
        if (keywordIndex != -1)
            tmp.token_type = (TokenType) keywordIndex;
        else
            tmp.token_type = ID;
    } else {
        if (!input.EndOfInput()) {
            input.UngetChar(c);
        }
        tmp.lexeme = "";
        tmp.token_type = ERROR;
    }
    return tmp;
}

// GetToken() accesses tokens from the tokenList that is populated when a 
// lexer object is instantiated
Token LexicalAnalyzer::GetToken()
{
    Token token;
    if (index == tokenList.size()){       // return end of file if
        token.lexeme = "";                // index is too large
        token.line_no = line_no;
        token.token_type = END_OF_FILE;
    }
    else{
        token = tokenList[index];
        index = index + 1;
    }
    return token;
}

// UngetToken() resets the index back by a amount equal to its argument 
// "howMany". "howMany" should be positive and not larger than the 
// actual number of valid tokens that were obtained using GetToken()
//
// NOTE 1: UngetToken() unget actual tokens. So, if you call GetToken() twice
// and for both call you get END_OF_FILE UngetToken(2) will return the last 
// two actual tokens (not END_OF_FILE). This might make the use of UngetToken()
// awkward and potentially error-prone (see NOTE 2)
//
// NOTE 2: UngetToken() will not be needed if you use GetToken() and peek() 
// judiciously
void LexicalAnalyzer::UngetToken(int howMany)
{
    if (howMany <= 0)
    {
        cout << "LexicalAnalyzer:UngetToken:Error: non positive argument\n";
        exit(-1);
    }

    index = index - howMany; // update index
    if (index < 0)           // and panic if resulting index is negative
    {
        cout << "LexicalAnalyzer:UngetToken:Error: large  argument\n";
        exit(-1);
    }
}

// peek requires that the argument "howFar" be positive.
Token LexicalAnalyzer::peek(int howFar)
{
    if (howFar <= 0) {      // peeking backward or in place is not allowed
        cout << "LexicalAnalyzer:peek:Error: non positive argument\n";
        exit(-1);
    }

    int peekIndex = index + howFar - 1;
    if (peekIndex > (tokenList.size()-1)) { // if peeking too far
        Token token;                        // return END_OF_FILE
        token.lexeme = "";
        token.line_no = line_no;
        token.token_type = END_OF_FILE;
        return token;
    } else
        return tokenList[peekIndex];
}

Token LexicalAnalyzer::GetTokenMain()
{
    char c;

    SkipSpace();
    tmp.lexeme = "";
    tmp.line_no = line_no;
    input.GetChar(c);
    switch (c) {
        case '+':   tmp.token_type = PLUS;      return tmp;
        case '-':   tmp.token_type = MINUS;     return tmp;
        case '/':   tmp.token_type = DIV;       return tmp;
        case '*':   tmp.token_type = MULT;      return tmp;
        case '=':   tmp.token_type = EQUAL;     return tmp;
        case ':':   tmp.token_type = COLON;     return tmp;
        case ',':   tmp.token_type = COMMA;     return tmp;
        case ';':   tmp.token_type = SEMICOLON; return tmp;
        case '[':   tmp.token_type = LBRAC;     return tmp;
        case ']':   tmp.token_type = RBRAC;     return tmp;
        case '(':   tmp.token_type = LPAREN;    return tmp;
        case ')':   tmp.token_type = RPAREN;    return tmp;
        case '{':   tmp.token_type = LBRACE;    return tmp;
        case '}':   tmp.token_type = RBRACE;    return tmp;
        case '>':   tmp.token_type = GREATER;   return tmp;
        case '<':
            input.GetChar(c);
            if (c == '>') {
                tmp.token_type = NOTEQUAL;
            } else {
                if (!input.EndOfInput()) {
                    input.UngetChar(c);
                }
                tmp.token_type = LESS;
            }
            return tmp;
        default:
            if (isdigit(c)) {
                input.UngetChar(c);
                return ScanNumber();
            } else if (isalpha(c)) {
                input.UngetChar(c);
                return ScanIdOrKeyword();
            } else if (input.EndOfInput())
                tmp.token_type = END_OF_FILE;
            else
                tmp.token_type = ERROR;

            return tmp;
    }
}
