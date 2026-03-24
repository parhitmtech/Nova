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

// must match TokenType enum order exactly
string reserved[] = { "END_OF_FILE",
    "FOR", "IF", "ELIF", "WHILE", "SWITCH", "CASE", "DEFAULT",
    "INPUT", "ARRAY", "PRINT",
    "DEF", "RETURN", "ELSE",
    "AND", "OR", "NOT",
    "PLUS", "MINUS", "DIV", "MULT",
    "EQUAL", "COLON", "COMMA", "SEMICOLON",
    "LBRAC", "RBRAC", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
    "NOTEQUAL", "GREATER", "LESS",
    "NUM", "ID", "STRING", "ERROR"
};

// must match the number of keywords in TokenType enum
// FOR, IF, ELIF, WHILE, SWITCH, CASE, DEFAULT,
// INPUT, ARRAY, PRINT,
// DEF, RETURN, ELSE, AND, OR, NOT
#define KEYWORDS_COUNT 16

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
    index = 0;
    initialized = false;
}

void LexicalAnalyzer::Initialize()
{
    if (initialized) return;
    initialized = true;

    Token token = GetTokenMain();
    while (token.token_type != END_OF_FILE)
    {
        tokenList.push_back(token);
        token = GetTokenMain();
    }
}

void LexicalAnalyzer::InitializeFromFile(const std::string& filename)
{
    if (initialized) return;

    // load file into InputBuffer directly — stdin stays untouched
    if (!input.InitFromFile(filename))
    {
        cerr << "Error: could not open file '" << filename << "'\n";
        exit(1);
    }

    initialized = true;

    Token token = GetTokenMain();
    while (token.token_type != END_OF_FILE)
    {
        tokenList.push_back(token);
        token = GetTokenMain();
    }
}

void LexicalAnalyzer::ReinitializeFromString(const std::string& s)
{
    // reset all the states
    tokenList.clear();
    index = 0;
    line_no = 1;
    initialized = false;

    tmp.lexeme = "";
    tmp.line_no = 1;
    tmp.token_type = ERROR;

    // load string into input buffer
    input.InitFromString(s);

    //tokenize
    initialized = true;
    Token token = GetTokenMain();
    while (token.token_type != END_OF_FILE)
    {
        tokenList.push_back(token);
        token = GetTokenMain();
    }
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

    if (c != '\0' && !isspace(c)) {
        input.UngetChar(c);
    }
    return space_encountered;
}

int LexicalAnalyzer::FindKeywordIndex(string s)
{
    // keywords must match TokenType enum order exactly
    // starting from index 1 (END_OF_FILE = 0)
    string keyword[] = {
        "for", "if", "elif", "while", "switch", "case", "default",
        "input", "array", "print",
        "def", "return", "else",
        "and", "or", "not"
    };

    for (int i = 0; i < KEYWORDS_COUNT; i++) {
        if (s == keyword[i]) {
            return i + 1;  // +1 because END_OF_FILE = 0
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

    if (isalpha(c) || c == '_') {  // ← allow leading underscore (Python style)
        tmp.lexeme = "";
        // allow letters, digits, underscores — standard Python identifier rules
        while (!input.EndOfInput() && (isalnum(c) || c == '_')) {
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

Token LexicalAnalyzer::GetToken()
{
    Token token;
    if (index == tokenList.size()) {
        token.lexeme = "";
        token.line_no = line_no;
        token.token_type = END_OF_FILE;
    }
    else {
        token = tokenList[index];
        index = index + 1;
    }
    return token;
}

void LexicalAnalyzer::UngetToken(int howMany)
{
    if (howMany <= 0)
    {
        cout << "LexicalAnalyzer:UngetToken:Error: non positive argument\n";
        exit(-1);
    }
    index = index - howMany;
    if (index < 0)
    {
        cout << "LexicalAnalyzer:UngetToken:Error: large argument\n";
        exit(-1);
    }
}

Token LexicalAnalyzer::peek(int howFar)
{
    if (howFar <= 0) {
        cout << "LexicalAnalyzer:peek:Error: non positive argument\n";
        exit(-1);
    }

    int peekIndex = index + howFar - 1;
    if (peekIndex > (int)(tokenList.size() - 1)) {
        Token token;
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

        // ── string literals ───────────────────────────────────────────────────
        case '"': {
            tmp.lexeme = "";
            char ch;
            input.GetChar(ch);
            while (!input.EndOfInput() && ch != '"')
            {
                tmp.lexeme += ch;
                input.GetChar(ch);
            }
            tmp.token_type = STRING;
            return tmp;
        }

        // ── single line comments  # comment ──────────────────────────────────
        // Python-style comments — skip everything after # to end of line
        case '#': {
            char ch;
            input.GetChar(ch);
            while (!input.EndOfInput() && ch != '\n')
                input.GetChar(ch);
            line_no++;
            // after skipping comment, get the next real token
            return GetTokenMain();
        }

        default:
            if (isdigit(c)) {
                input.UngetChar(c);
                return ScanNumber();
            } else if (isalpha(c) || c == '_') {  // ← allow leading underscore
                input.UngetChar(c);
                return ScanIdOrKeyword();
            } else if (input.EndOfInput())
                tmp.token_type = END_OF_FILE;
            else
                tmp.token_type = ERROR;

            return tmp;
    }
}