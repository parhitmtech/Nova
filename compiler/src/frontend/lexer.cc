// The lexer: a pointer-based Scanner that decides layout as it scans (NEWLINE / INDENT / DEDENT).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "lexer.h"

using namespace std;

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

void Scanner::error(DiagCode code, const char* at, uint32_t length, const string& message)
{
    diagnostics.report(code, loc_of(at), length, message);
}

void Scanner::push(vector<LexToken>& out, TokenType kind, const char* start, Symbol symbol)
{
    out.push_back({kind, loc_of(start), (uint32_t)(p - start), symbol, line});
}

// p at '#': skips the comment. "#@line N" or "#@line N "file"" (written by the import preprocessor) says the next
// line is line N (of that file).
void Scanner::comment()
{
    const char* start = p;
    while (p < end && *p != '\n') p++;
    if (p - start > 7 && strncmp(start, "#@line ", 7) == 0)
    {
        int n = atoi(start + 7);
        string file;
        const char* q = (const char*)memchr(start + 7, '"', p - start - 7);
        if (q)
        {
            const char* close = (const char*)memchr(q + 1, '"', p - q - 1);
            if (close) file.assign(q + 1, close);
        }
        sourceManager.add_line_directive(loc_of(start), n, file);
        line = n - 1;
    }
}

// Whether the line at q can only be the start of a statement, never the rest of an expression: a line like that,
// at or left of an open bracket's line, is not that bracket's continuation, so the bracket was left open by
// mistake. It is: a keyword that only starts a statement or a definition; print(...), which gives no value; or a
// declaration, a type and then a name (int b, Point p), since two names in a row are not an expression. A line
// that only might be a statement (x = 1 could be a keyword argument's name=value) is read as a continuation.
bool Scanner::starts_statement(const char* q) const
{
    auto word_at = [&](const char* w) -> size_t {  // the length of the name at w, or 0
        if (w >= end || !is_id_start(*w)) return 0;
        const char* e = w;
        while (e < end && is_id(*e)) e++;
        return (size_t)(e - w);
    };
    auto is = [](const char* w, size_t n, const char* const* list, size_t count) {
        for (size_t i = 0; i < count; i++) if (strlen(list[i]) == n && strncmp(w, list[i], n) == 0) return true;
        return false;
    };
    static const char* const statements[] = {"def", "class", "struct", "ignite", "if", "elif", "else", "while", "for",
                                             "do", "switch", "case", "default", "return", "break", "continue", "print"};
    static const char* const operators[] = {"and", "or", "not"};  // between two names in an expression
    static const char* const values[] = {"and", "or", "not", "true", "false", "self", "input", "array"};
    size_t n = word_at(q);
    if (n == 0) return false;
    if (is(q, n, statements, sizeof statements / sizeof *statements)) return true;
    if (is(q, n, values, sizeof values / sizeof *values)) return false;
    const char* r = q + n;
    if (r >= end || (*r != ' ' && *r != '\t')) return false;
    while (r < end && (*r == ' ' || *r == '\t')) r++;
    size_t m = word_at(r);
    return m > 0 && !is(r, m, operators, sizeof operators / sizeof *operators);
}

// The end of a logical line: a NEWLINE, and a ':' at its end (the only ':' outside brackets) opens a block.
void Scanner::end_line(vector<LexToken>& out)
{
    if (out.size() == lineStart) return;
    const LexToken& last = out.back();
    if (last.kind == COLON && colons == 1) needBody = true;
    out.push_back({NEWLINE, loc_of(p), 0, 0, last.line});
    lineStart = out.size();
    colons = 0;
}

// p is at the start of a physical line. Blank and comment-only lines are skipped. Outside brackets, the first line
// with a token sets the layout: INDENT after a ':' line (or wherever the line is indented further: the parser says
// whether that was expected), DEDENT for each block it closes.
void Scanner::begin_line(vector<LexToken>& out)
{
    while (true)
    {
        int column = 0;
        const char* lineBegin = p;
        const char* tab = nullptr;
        while (*p == ' ' || *p == '\t' || *p == '\r')
        {
            if (*p == '\t' && !tab) tab = p;
            if (*p != '\r') column++;
            p++;
        }
        if (p >= end) return;
        if (*p == '\n') { p++; line++; continue; }  // a blank line
        if (*p == '#')
        {
            comment();
            if (p < end && *p == '\n') { p++; line++; }
            continue;
        }
        if (tab && !tabReported && openBrackets.empty())
        {
            error(DiagCode::TabIndentation, tab, 1, "indent with spaces, not tabs");
            tabReported = true;
        }
        if (!openBrackets.empty())
        {
            // A continuation line inside ( ) or [ ], unless it plainly starts a new statement: then the bracket was
            // never closed, and the logical line ends before this line.
            if (column > openBrackets.front().indent || !starts_statement(p)) return;
            const Bracket& b = openBrackets.front();
            error(DiagCode::UnclosedBracket, b.at, 1, string("'") + b.c + "' was never closed");
            diagnostics.add_quiet_range({loc_of(b.at), loc_of(lineBegin) + 1});
            openBrackets.clear();
            const char* here = p;
            p = lineBegin;  // the NEWLINE that ends the bracket's line, before this line
            end_line(out);
            p = here;
        }

        if (needBody)
        {
            needBody = false;
            if (column > indents.back())
            {
                indents.push_back(column);
                out.push_back({INDENT, loc_of(p), 0, 0, line});
                return;
            }
            // no indented block: the parser reports it, after the NEWLINE that ended the ':' line
        }
        else if (column > indents.back())
        {
            // indented further with no ':' before it: the parser reports the unexpected block and skips it
            indents.push_back(column);
            out.push_back({INDENT, loc_of(p), 0, 0, line});
            return;
        }
        while (column < indents.back())
        {
            indents.pop_back();
            out.push_back({DEDENT, loc_of(p), 0, 0, line});
        }
        if (column != indents.back())
        {
            // between two blocks' indentation: read it as the enclosing block's
            error(DiagCode::InconsistentIndentation, lineBegin, (uint32_t)(p - lineBegin),
                  "this line's indentation does not match any enclosing block");
        }
        return;
    }
}

void Scanner::scan(vector<LexToken>& out)
{
    register_keywords();
    begin_line(out);
    lineStart = out.size();
    while (true)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (p >= end) break;
        char c = *p;
        if (c == '\n')
        {
            if (openBrackets.empty()) end_line(out);
            p++;
            line++;
            size_t before = out.size();
            begin_line(out);
            if (out.size() != before && out.back().kind != NEWLINE) lineStart = out.size();  // after INDENT / DEDENTs
            continue;
        }
        if (c == '#')
        {
            comment();
            continue;
        }
        if (c == ':' && openBrackets.empty()) colons++;
        scan_token(out);
    }

    if (!openBrackets.empty())  // the file ended inside ( ) or [ ]
    {
        const Bracket& b = openBrackets.front();
        error(DiagCode::UnclosedBracket, b.at, 1, string("'") + b.c + "' was never closed");
        diagnostics.add_quiet_range({loc_of(b.at), loc_of(end) + 1});
        openBrackets.clear();
    }
    end_line(out);
    needBody = false;  // a ':' on the last line: the parser reports the missing block
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
            while (q < end && *q != '"' && *q != '\n') q++;
            if (q >= end || *q != '"')
            {
                // the string ends at the end of the line, so the lines after it are read as they are written
                while (q > p && q[-1] == '\r') q--;
                error(DiagCode::UnterminatedString, start, (uint32_t)(q - start), "unterminated string literal");
                Symbol s = names.intern(p, (size_t)(q - p));
                p = q;
                push(out, STRING, start, s);
                return;
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
            if (*p == '/')
            {
                Diagnostic& d = diagnostics.report(DiagCode::SlashComment, loc_of(start), 2,
                                                   "comments start with '#', not '//'");
                d.fixes.push_back({{loc_of(start), loc_of(start + 2)}, "#"});
                p = start;
                comment();  // the rest of the line is the comment it was meant to be
                return;
            }
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
                Diagnostic& d = diagnostics.report(DiagCode::Semicolon, loc_of(start), 1,
                                                   "Nova does not use ';' to end statements: start a new line instead");
                d.fixes.push_back({{loc_of(start), loc_of(start + 1)}, ""});
                return;
            }
            push(out, SEMICOLON, start);
            return;
        case '(': case '[':
            openBrackets.push_back({start, c, indents.back()});
            push(out, c == '(' ? LPAREN : LBRAC, start);
            return;
        case ')': case ']':
            if (openBrackets.empty())
            {
                error(DiagCode::UnmatchedBracket, start, 1, string("'") + c + "' has no matching '" +
                                                            (c == ')' ? "(" : "[") + "'");
                return;
            }
            openBrackets.pop_back();
            push(out, c == ')' ? RPAREN : RBRAC, start);
            return;
        case '{': case '}':
            error(DiagCode::BraceBlock, start, 1, "blocks are written with ':' and an indented body, not braces");
            return;
        case '>': push(out, next_is('=') ? GREATEREQ : GREATER, start); return;
        case '<':
        {
            if (!out.empty() && out.back().kind == TENSOR_TYPE)
            {
                error(DiagCode::TensorDtype, start, 1,
                      "Tensor<dtype> comes with fp32 storage (M1 stage 4); write Tensor for now");
            }
            if (next_is('>')) push(out, NOTEQUAL, start);  // "<>": an older spelling of "!="
            else push(out, next_is('=') ? LESSEQ : LESS, start);
            return;
        }
        case '!':
        {
            if (next_is('='))
            {
                push(out, NOTEQUAL, start);
                return;
            }
            Diagnostic& d = diagnostics.report(DiagCode::BangOperator, loc_of(start), 1, "Nova writes 'not', not '!'");
            d.fixes.push_back({{loc_of(start), loc_of(start + 1)}, "not "});
            push(out, NOT, start);  // read as the 'not' it means
            return;
        }
        default:
        {
            // a character Nova does not use: show it as written (a whole UTF-8 character), and skip it
            unsigned char u = (unsigned char)c;
            int len = (u >> 5) == 0x6 ? 2 : (u >> 4) == 0xE ? 3 : (u >> 3) == 0x1E ? 4 : 1;
            bool valid = u >= 0x80 ? len > 1 : (u >= 0x20 && u < 0x7F);
            for (int k = 1; valid && k < len; k++) valid = start + k < end && ((unsigned char)start[k] & 0xC0) == 0x80;
            string shown;
            if (valid)
            {
                shown = string(start, len);
                p = start + len;
            }
            else
            {
                char buf[8];
                snprintf(buf, sizeof buf, "\\x%02X", u);
                shown = buf;
            }
            error(DiagCode::UnexpectedCharacter, start, (uint32_t)(p - start), "unexpected character '" + shown + "'");
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A whole program
// ─────────────────────────────────────────────────────────────────────────────

vector<LexToken> scan_program(const string& source, const string& name)
{
    SourceLoc start = sourceManager.add_buffer(name, source);
    vector<LexToken> tokens;
    tokens.reserve(source.size() / 3 + 16);
    Scanner(start, sourceManager.text_at(start), source.size()).scan(tokens);
    return tokens;
}
