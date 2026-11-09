// The lexer: a pointer-based Scanner that decides layout as it scans (NEWLINE / INDENT / DEDENT).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "lexer.h"

using namespace std;

// Layout and lexical errors stop compilation with the line, like the parser's errors.
[[noreturn]] static void layout_error(int line, const string& msg)
{
    fprintf(stderr, "Error at line %d: %s\n", line, msg.c_str());
    exit(1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Keywords and character classes
// ─────────────────────────────────────────────────────────────────────────────

// Keywords are interned once and tagged with their kind: scanning a name then costs one lookup either way.
static void register_keywords()
{
    static bool done = false;
    if (done) return;
    done = true;
    static const struct { const char* text; TokenType kind; } keywords[] = {
        {"ignite", IGNITE},
        {"for", FOR}, {"if", IF}, {"elif", ELIF}, {"while", WHILE}, {"do", DO}, {"switch", SWITCH},
        {"case", CASE}, {"default", DEFAULT},
        {"input", INPUT}, {"array", ARRAY}, {"print", PRINT},
        {"def", DEF}, {"return", RETURN}, {"else", ELSE},
        {"and", AND}, {"or", OR}, {"not", NOT},
        {"int", INT_TYPE}, {"bool", BOOL_TYPE}, {"string", STRING_TYPE},
        {"float", FLOAT_TYPE}, {"double", DOUBLE_TYPE}, {"Tensor", TENSOR_TYPE},
        {"true", TRUE}, {"false", FALSE},
        {"struct", STRUCT}, {"class", CLASS}, {"extends", EXTENDS}, {"self", SELF}, {"import", IMPORT},
        {"break", BREAK}, {"continue", CONTINUE},
    };
    for (const auto& k : keywords) names.set_tag(names.intern(k.text, strlen(k.text)), k.kind);
}

enum : unsigned char { C_ID_START = 1, C_ID = 2, C_DIGIT = 4 };

static const struct CharClasses
{
    unsigned char of[256] = {};
    CharClasses()
    {
        for (int c = 'a'; c <= 'z'; c++) of[c] = C_ID_START | C_ID;
        for (int c = 'A'; c <= 'Z'; c++) of[c] = C_ID_START | C_ID;
        of[(unsigned char)'_'] = C_ID_START | C_ID;
        for (int c = '0'; c <= '9'; c++) of[c] = C_ID | C_DIGIT;
    }
} chars;

static inline bool is_id_start(char c) { return chars.of[(unsigned char)c] & C_ID_START; }
static inline bool is_id(char c) { return chars.of[(unsigned char)c] & C_ID; }
static inline bool is_digit(char c) { return chars.of[(unsigned char)c] & C_DIGIT; }

// ─────────────────────────────────────────────────────────────────────────────
// Scanner
// ─────────────────────────────────────────────────────────────────────────────

void Scanner::push(vector<LexToken>& out, TokenType kind, const char* start, Symbol symbol)
{
    out.push_back({kind, loc_of(start), (uint32_t)(p - start), symbol, line});
}

// p is at the start of a physical line. Blank and comment-only lines are skipped. Outside brackets, the first line
// with a token sets the layout: INDENT after a ':' line, DEDENT for each block it closes.
void Scanner::begin_line(vector<LexToken>& out)
{
    while (true)
    {
        int column = 0;
        bool tab = false;
        while (*p == ' ' || *p == '\t' || *p == '\r')
        {
            if (*p == '\t') tab = true;
            if (*p != '\r') column++;
            p++;
        }
        if (*p == '\0') return;
        if (*p == '\n') { p++; line++; continue; }  // a blank line
        if (tab) layout_error(line, "indent with spaces, not tabs");
        if (*p == '#')
        {
            // a comment line: "#@line N" (written by the import preprocessor) says the next line is line N
            const char* start = p;
            while (*p && *p != '\n') p++;
            if (p - start > 7 && strncmp(start, "#@line ", 7) == 0)
            {
                int n = atoi(start + 7);
                sourceManager.add_line_directive(loc_of(start), n);
                line = n - 1;
            }
            if (*p == '\n') { p++; line++; }
            continue;
        }
        if (!openBrackets.empty()) return;  // a continuation line inside ( ) or [ ]: no layout

        if (needBody)
        {
            if (column <= indents.back())
            {
                layout_error(line, "expected an indented block after the ':' on line " + to_string(headerLine));
            }
            indents.push_back(column);
            out.push_back({INDENT, loc_of(p), 0, 0, line});
            needBody = false;
        }
        else if (column > indents.back())
        {
            layout_error(line, "unexpected indentation");
        }
        while (column < indents.back())
        {
            indents.pop_back();
            out.push_back({DEDENT, loc_of(p), 0, 0, line});
        }
        if (column != indents.back())
        {
            layout_error(line, "this line's indentation does not match any enclosing block");
        }
        return;
    }
}

void Scanner::scan(vector<LexToken>& out)
{
    register_keywords();
    size_t lineStart = out.size();  // the first token of the current logical line
    int colons = 0;                 // ':' outside brackets on the current logical line
    
    // The end of a logical line: a NEWLINE, and a ':' at its end (the only ':' outside brackets) opens a block.
    auto end_line = [&]() {
        if (out.size() == lineStart) return;
        const LexToken& last = out.back();
        if (last.kind == COLON && colons == 1)
        {
            needBody = true;
            headerLine = out[lineStart].line;
        }
        out.push_back({NEWLINE, loc_of(p), 0, 0, last.line});
        lineStart = out.size();
        colons = 0;
    };

    begin_line(out);
    lineStart = out.size();
    while (true)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        char c = *p;
        if (c == '\0') break;
        if (c == '\n')
        {
            if (openBrackets.empty()) end_line();
            p++;
            line++;
            size_t before = out.size();
            begin_line(out);
            if (out.size() != before) lineStart = out.size();  // after the INDENT / DEDENTs
            continue;
        }
        if (c == '#')
        {
            const char* start = p;
            while (*p && *p != '\n') p++;
            if (p - start > 7 && strncmp(start, "#@line", 7) == 0)
            {
                int n = atoi(start + 7);
                sourceManager.add_line_directive(loc_of(start), n);
                line = n - 1;
            }
            continue;
        }
        if (c == ':' && openBrackets.empty()) colons++;
        scan_token(out);
    }

    if (!openBrackets.empty())  // the file ended inside ( ) or [ ]
    {
        layout_error(openBrackets.back() >> 1, string("unclosed '") + ((openBrackets.back() & 1) ? "[" : "(") + 
                                               "' - missing closing bracket");
    }
    end_line();
    if (needBody)
    {
        layout_error(headerLine, "expected an indented block after the ':' on line " + to_string(headerLine));
    }
    int lastLine = out.empty() ? line : out.back().line;
    while (indents.size() > 1)
    {
        indents.pop_back();
        out.push_back({DEDENT, loc_of(p), 0, 0, lastLine});
    }
    out.push_back({END_OF_FILE, loc_of(p), 0, 0, line});
}

void Scanner::scan_token(vector<LexToken>& out)
{
    const char* start = p;
    char c = *p++;

    if (is_id_start(c))
    {
        while (is_id(*p)) p++;
        Symbol s = names.intern(start, (size_t)(p - start));
        int keyword = names.tag(s);
        push(out, keyword ? (TokenType)keyword : ID, start, s);
        return;
    }
    if (is_digit(c))
    {
        // 12, 1.5, 1e-3, 2.5E4: a '.' or an exponent makes a float literal
        bool isFloat = false;
        while (is_digit(*p)) p++;
        if (*p == '.')
        {
            isFloat = true;
            p++;
            while (is_digit(*p)) p++;
        }
        if (*p == 'e' || *p == 'E')
        {
            isFloat = true;
            p++;
            if (*p == '+' || *p == '-') p++;
            while (is_digit(*p)) p++;
        }
        push(out, isFloat ? FLOAT_LITERAL : NUM, start);
        return;
    }

    auto next_is = [&](char want) {
        if (*p != want) return false;
        p++;
        return true;
    };
    switch (c)
    {
        case '"':
        {
            const char* q = p;
            while (*q && *q != '"' && *q != '\n') q++;
            if (*q != '"')
            {
                // Without this the rest of the line(or file) silently became part of the string.
                fprintf(stderr, "Error at line %d: unterminated string literal\n", line);
                exit(1);
            }
            Symbol s = names.intern(p, (size_t)(q - p));
            p = q + 1;
            push(out, STRING, start, s);
            return;
        }
        case '+': push(out, next_is('=') ? PLUSEQ : PLUS, start); return;
        case '-':
            if (next_is('>')) push(out, ARROW, start);
            else push(out, next_is('=') ? MINUSEQ : MINUS, start);
            return;
        case '*': push(out, next_is('=') ? MULTEQ : MULT, start); return;
        case '/':
            if (*p == '/') layout_error(line, "comments start with '#', not '//'");
            push(out, next_is('=') ? DIVEQ : DIV, start);
            return;
        case '%': push(out, MOD, start); return;
        case '@': push(out, AT, start); return;             // matmul
        case '=': push(out, next_is('=') ? EQEQ : EQUAL, start); return;
        case ':': push(out, COLON, start); return;
        case ',': push(out, COMMA, start); return;
        case '.': push(out, DOT, start); return;
        case ';': 
            if (openBrackets.empty())
            {
                layout_error(line, "Nova does not use ';' to end statements: start a new line instead");
            }
            push(out, SEMICOLON, start);
            return;
        case '(': case '[':
            openBrackets.push_back(line << 1 | (c == '[' ? 1 : 0));
            push(out, c == '(' ? LPAREN : LBRAC, start);
            return;
        case ')': case ']':
            if (!openBrackets.empty()) openBrackets.pop_back();
            push(out, c == ')' ? RPAREN : RBRAC, start);
            return;
        case '{': case '}':
        {
            layout_error(line, "blocks are written with ':' and an indented body, not braces");
        }
        case '>': push(out, next_is('=') ? GREATEREQ : GREATER, start); return;
        case '<':
        {
            if (!out.empty() && out.back().kind == TENSOR_TYPE)
            {
                layout_error(line, "Tensor<dtype> comes with fp32 storage (M1 stage 4); write Tensor for now");
            }
            if (next_is('>')) push(out, NOTEQUAL, start);  // "<>": an older spelling of "!="
            else push(out, next_is('=') ? LESSEQ : LESS, start);
            return;
        }
        case '!':
        {
            push(out, next_is('=') ? NOTEQUAL : ERROR, start);  // a lone '!' is not an operator (use "not")
            return;
        }
        default:
        {
            push(out, ERROR, start);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A whole program
// ─────────────────────────────────────────────────────────────────────────────

vector<LexToken> scan_program(const string& source)
{
    SourceLoc start = sourceManager.add_buffer("<input>", source);
    vector<LexToken> tokens;
    tokens.reserve(source.size() / 3 + 16);
    Scanner(start, sourceManager.text_at(start)).scan(tokens);
    return tokens;
}
