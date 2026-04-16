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
string reserved[] = { 
    "MAIN",    
    "END_OF_FILE",
    // control flow
    "FOR", "IF", "ELIF", "WHILE", "DO", "SWITCH", "CASE", "DEFAULT",
    // I/O
    "INPUT", "ARRAY", "PRINT",
    // function keywords
    "DEF", "RETURN", "ELSE",
    // logical
    "AND", "OR", "NOT",
    // type keywords (new)
    "INT_TYPE", "BOOL_TYPE", "STRING_TYPE",
    // float/double type keywords
    "FLOAT_TYPE", "DOUBLE_TYPE",
    // bool literals (new)
    "TRUE", "FALSE",
    // struct and class keywords
    "STRUCT", "CLASS", "EXTENDS", "SELF", "IMPORT",
    // punctuation 
    "DOT",
    // arithmetic
    "PLUS", "MINUS", "DIV", "MULT",
    // punctuation
    "EQUAL", "COLON", "COMMA", "SEMICOLON",
    "LBRAC", "RBRAC", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
    // comparison
    "NOTEQUAL", "GREATER", "LESS",
    // arrow (new)
    "ARROW",
    // literals + misc
    "NUM", "FLOAT_LITERAL", "ID", "STRING", "ERROR"
};

// FOR IF ELIF WHILE SWITCH CASE DEFAULT      (7)
// INPUT ARRAY PRINT                          (3)
// DEF RETURN ELSE                            (3)
// AND OR NOT                                 (3)
// INT_TYPE BOOL_TYPE STRING_TYPE             (3)  ← new
// TRUE FALSE                                 (2)  ← new
#define KEYWORDS_COUNT 29

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
    tokenList.clear();
    index = 0;
    line_no = 1;
    initialized = false;

    tmp.lexeme = "";
    tmp.line_no = 1;
    tmp.token_type = ERROR;

    input.InitFromString(s);

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
    // starting at index 1 (END_OF_FILE = 0)
    string keyword[] = {
        "main",
        // 1–7
        "for", "if", "elif", "while", "do", "switch", "case", "default",
        // 8–10
        "input", "array", "print",
        // 11–13
        "def", "return", "else",
        // 14–16
        "and", "or", "not",
        // 17–19  ← new type keywords
        "int", "bool", "string",
        // 20-21 float/double type keywords
        "float", "double",
        // 22–23  ← new bool literals
        "true", "false",
        // 24 - struct keyword
        "struct", "class", "extends", "self", "import"
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
        tmp.lexeme = "";
        // scan integer part
        while (!input.EndOfInput() && isdigit(c)) {
            tmp.lexeme += c;
            input.GetChar(c);
        }
        // check for decimal point or exponent → float literal
        bool isFloat = false;
        if (c == '.') {
            isFloat = true;
            tmp.lexeme += c;
            input.GetChar(c);
            while (!input.EndOfInput() && isdigit(c)) {
                tmp.lexeme += c;
                input.GetChar(c);
            }
        }
        if (c == 'e' || c == 'E') {
            isFloat = true;
            tmp.lexeme += c;
            input.GetChar(c);
            if (c == '+' || c == '-') {
                tmp.lexeme += c;
                input.GetChar(c);
            }
            while (!input.EndOfInput() && isdigit(c)) {
                tmp.lexeme += c;
                input.GetChar(c);
            }
        }
        if (!input.EndOfInput()) input.UngetChar(c);
        tmp.token_type = isFloat ? FLOAT_LITERAL : NUM;
        tmp.line_no = line_no;
        return tmp;
    } else {
        if (!input.EndOfInput()) input.UngetChar(c);
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

    if (isalpha(c) || c == '_') {
        tmp.lexeme = "";
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
    } else {
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

        // ── '-' or '->' ───────────────────────────────────────────────────────
        case '-': {
            char next;
            input.GetChar(next);
            if (next == '>') {
                tmp.lexeme = "->";
                tmp.token_type = ARROW;
            } else {
                if (!input.EndOfInput()) input.UngetChar(next);
                tmp.token_type = MINUS;
            }
            return tmp;
        }

        case '/': {
            char next;
            input.GetChar(next);
            if (next == '/') {
                // line comment — skip to end of line, then re-lex
                char ch;
                while (!input.EndOfInput()) {
                    input.GetChar(ch);
                    if (ch == '\n') { line_no++; break; }
                }
                return GetTokenMain();  // recurse to get the real next token
            } else {
                if (!input.EndOfInput()) input.UngetChar(next);
                tmp.token_type = DIV;
                return tmp;
            }
        }
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
        case '.':   tmp.token_type = DOT;       return tmp;
        case '>':   tmp.token_type = GREATER;   return tmp;
        case '<':
            input.GetChar(c);
            if (c == '>') {
                tmp.token_type = NOTEQUAL;
            } else {
                if (!input.EndOfInput()) input.UngetChar(c);
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

        // ── Python-style comments: # ... ──────────────────────────────────────
        case '#': {
            char ch;
            input.GetChar(ch);
            while (!input.EndOfInput() && ch != '\n')
                input.GetChar(ch);
            line_no++;
            return GetTokenMain();
        }

        default:
            if (isdigit(c)) {
                input.UngetChar(c);
                return ScanNumber();
            } else if (isalpha(c) || c == '_') {
                input.UngetChar(c);
                return ScanIdOrKeyword();
            } else if (input.EndOfInput())
                tmp.token_type = END_OF_FILE;
            else
                tmp.token_type = ERROR;

            return tmp;
    }
}