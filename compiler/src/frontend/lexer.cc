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
    "END_OF_FILE",
    "IGNITE",
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
    // arithmetic
    "PLUS", "MINUS", "DIV", "MULT",
    // punctuation
    "EQUAL", "COLON", "COMMA", "SEMICOLON",
    "LBRAC", "RBRAC", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
    // comparison
    "NOTEQUAL", "GREATER", "LESS", "EQEQ",
    // arrow (new)
    "ARROW", "DOT",
    // more comparisons, remainder and compound assignment
    "GREATEREQ", "LESSEQ", "MOD",
    "PLUSEQ", "MINUSEQ", "MULTEQ", "DIVEQ",
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
    DesugarCompoundAssignments();
    AddMainEntry();
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
    DesugarCompoundAssignments();
    AddMainEntry();
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
    DesugarCompoundAssignments();
    AddMainEntry();
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

// "target op= expr" becomes "target = target op (expr)" in the token list, before parsing. Every
// kind of target the parser accepts (x, a[i], obj.f, self.f) then gets += -= *= /= without code of
// its own. The target is the run of tokens since the start of the statement, and it is evaluated
// twice (once to read, once to write).
void LexicalAnalyzer::DesugarCompoundAssignments()
{
    vector<Token> out;
    out.reserve(tokenList.size());
    for (size_t i = 0; i < tokenList.size(); i++)
    {
        const Token& t = tokenList[i];
        TokenType base;
        switch (t.token_type)
        {
            case PLUSEQ:  base = PLUS;  break;
            case MINUSEQ: base = MINUS; break;
            case MULTEQ:  base = MULT;  break;
            case DIVEQ:   base = DIV;   break;
            default:      out.push_back(t); continue;
        }

        // the target: tokens since the last statement boundary
        size_t start = out.size();
        while (start > 0)
        {
            TokenType p = out[start - 1].token_type;
            if (p == SEMICOLON || p == LBRACE || p == RBRACE || p == COLON) break;
            start--;
        }
        vector<Token> target(out.begin() + start, out.end());

        auto make = [&](TokenType type, const char* lexeme) {
            Token k = t;
            k.token_type = type;
            k.lexeme = lexeme;
            return k;
        };
        out.push_back(make(EQUAL, "="));
        out.insert(out.end(), target.begin(), target.end());
        out.push_back(make(base, ""));
        out.push_back(make(LPAREN, "("));

        // the right side runs to the end of the statement (';', or the ')' closing a for header)
        int depth = 0;
        for (i = i + 1; i < tokenList.size(); i++)
        {
            TokenType k = tokenList[i].token_type;
            if (depth == 0 && (k == SEMICOLON || k == RPAREN || k == RBRACE)) break;
            if (k == LPAREN || k == LBRAC) depth++;
            if (k == RPAREN || k == RBRAC) depth--;
            out.push_back(tokenList[i]);
        }
        out.push_back(make(RPAREN, ")"));
        i--;  // the terminator is copied by the outer loop
    }
    tokenList.swap(out);
}

// The entry point is the 'ignite { ... }' block. A file without one may define 'def main()' instead:
// then "ignite { main() ; }" is appended, so the parser always sees a single entry block.
void LexicalAnalyzer::AddMainEntry()
{
    bool hasMain = false;
    for (size_t i = 0; i < tokenList.size(); i++)
    {
        if (tokenList[i].token_type == IGNITE) return;
        if (tokenList[i].token_type == DEF && i + 3 < tokenList.size() &&
            tokenList[i + 1].token_type == ID && tokenList[i + 1].lexeme == "main" &&
            tokenList[i + 2].token_type == LPAREN && tokenList[i + 3].token_type == RPAREN)
            hasMain = true;
    }
    if (!hasMain) return;

    int line = tokenList.back().line_no;
    auto add = [&](TokenType type, const char* lexeme) {
        Token k;
        k.token_type = type;
        k.lexeme = lexeme;
        k.line_no = line;
        tokenList.push_back(k);
    };
    add(IGNITE, "ignite");
    add(LBRACE, "{");
    add(ID, "main");
    add(LPAREN, "(");
    add(RPAREN, ")");
    add(SEMICOLON, ";");
    add(RBRACE, "}");
}

// Two-character operators ("<=", "+=", "!=", ...): if the next character is c, consume it and
// return true; otherwise leave the input untouched.
bool LexicalAnalyzer::NextIs(char c)
{
    if (input.EndOfInput()) return false;
    char next;
    input.GetChar(next);
    if (next == c) return true;
    input.UngetChar(next);
    return false;
}

int LexicalAnalyzer::FindKeywordIndex(string s)
{
    // keywords must match TokenType enum order exactly
    // starting at index 1 (END_OF_FILE = 0)
    string keyword[] = {
        "ignite",
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
    if (index == (int)tokenList.size()) {
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
        case '+':   tmp.token_type = NextIs('=') ? PLUSEQ : PLUS;   return tmp;

        // ── '-', '->' or '-=' ─────────────────────────────────────────────────
        case '-': {
            if (NextIs('>')) {
                tmp.lexeme = "->";
                tmp.token_type = ARROW;
            } else {
                tmp.token_type = NextIs('=') ? MINUSEQ : MINUS;
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
                tmp.token_type = NextIs('=') ? DIVEQ : DIV;
                return tmp;
            }
        }
        case '*':   tmp.token_type = NextIs('=') ? MULTEQ : MULT;   return tmp;
        case '%':   tmp.token_type = MOD;       return tmp;
        case '=':
        {   
            // "==" is equality in a condition; a single "=" is assignment
            input.GetChar(c);
            if (c == '=') {
                tmp.token_type = EQEQ;
            } else {
                if (!input.EndOfInput()) input.UngetChar(c);
                tmp.token_type = EQUAL;
            }
            return tmp;
        }
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
        case '>':   tmp.token_type = NextIs('=') ? GREATEREQ : GREATER;   return tmp;
        case '<':
            if (NextIs('>'))      tmp.token_type = NOTEQUAL;  // "<>" (older spelling of "!=")
            else if (NextIs('=')) tmp.token_type = LESSEQ;
            else                  tmp.token_type = LESS;
            return tmp;
        case '!':
            // "!=" only; a lone '!' is not an operator (use "not")
            tmp.token_type = NextIs('=') ? NOTEQUAL : ERROR;
            return tmp;

        // ── string literals ───────────────────────────────────────────────────
        case '"': {
            tmp.lexeme = "";
            char ch;
            input.GetChar(ch);
            while (!input.EndOfInput() && ch != '"' && ch != '\n')
            {
                tmp.lexeme += ch;
                input.GetChar(ch);
            }
            if (ch != '"')
            {
                // Without this the rest of the line (or file) silently became part of the string.
                fprintf(stderr, "Error at line %d: unterminated string literal\n", line_no);
                exit(1);
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