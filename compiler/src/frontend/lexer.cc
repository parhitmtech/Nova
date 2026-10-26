// The lexer: a pointer-based Scanner that decides layout as it scans (NEWLINE / INDENT / DEDENT), and 
// LexicalAnalyzer, which turns the scanner's tokens into the braces-and-semicolons form the current parser reads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "lexer.h"

using namespace std;

// must match TokenType enum order exactly
static const char* const kind_names[] = {
    "END_OF_FILE", "IGNITE",
    "FOR", "IF", "ELIF", "WHILE", "DO", "SWITCH", "CASE", "DEFAULT",
    "INPUT", "ARRAY", "PRINT",
    "DEF", "RETURN", "ELSE",
    "AND", "OR", "NOT",
    "INT_TYPE", "BOOL_TYPE", "STRING_TYPE",
    "FLOAT_TYPE", "DOUBLE_TYPE", "TENSOR_TYPE",
    "TRUE", "FALSE",
    "STRUCT", "CLASS", "EXTENDS", "SELF", "IMPORT",
    "BREAK", "CONTINUE",
    "PLUS", "MINUS", "DIV", "MULT",
    "EQUAL", "COLON", "COMMA", "SEMICOLON",
    "LBRAC", "RBRAC", "LPAREN", "RPAREN", "LBRACE", "RBRACE",
    "NOTEQUAL", "GREATER", "LESS", "EQEQ",
    "ARROW", "DOT",
    "GREATEREQ", "LESSEQ", "MOD", "AT",
    "PLUSEQ", "MINUSEQ", "MULTEQ", "DIVEQ",
    "NUM", "FLOAT_LITERAL", "ID", "STRING", "ERROR",
    "NEWLINE", "INDENT", "DEDENT"
};
static_assert(sizeof(kind_names) / sizeof(kind_names[0]) == DEDENT + 1, "kind_names must list every TokenType");

// Layout and lexical errors stop compilation with the line, like the parser's errors.
[[noreturn]] static void layout_error(int line, const string& msg)
{
    fprintf(stderr, "Error at line %d: %s\n", line, msg.c_str());
    exit(1);
}

void Token::Print() const
{
    cout << "{" << lexeme << ", " << kind_names[(int)token_type] << " , " << line_no << "}\n";
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
// LexicalAnalyzer: the scanner's tokens in the current parser's form
// ─────────────────────────────────────────────────────────────────────────────

LexicalAnalyzer::LexicalAnalyzer()
{
    eof.token_type = END_OF_FILE;
    eof.line_no = 1;
}

void LexicalAnalyzer::ReinitializeFromString(const string& s)
{
    tokenList.clear();
    index = 0;
    SourceLoc start = sourceManager.add_buffer("<input>", s);
    vector<LexToken> lexed;
    lexed.reserve(s.size() / 3 + 16);
    Scanner(start, sourceManager.text_at(start)).scan(lexed);
    // An error at the end of the file points at the last line with code on it
    eof.line_no = 1;
    for (size_t i = lexed.size(); i-- > 0;)
    {
        if (lexed[i].kind != NEWLINE && lexed[i].kind != INDENT && lexed[i].kind != DEDENT && lexed[i].kind != END_OF_FILE)
        {
            eof.line_no = lexed[i].line;
            break;
        }
    }
    eof.loc = lexed.back().loc;
    ToBracesForm(lexed);
    DesugarCompoundAssignments();
    AddMainEntry();
}

// The form the current parser reads: a block's ':' becomes '{' and its DEDENT '}', and each statement ends with ';'.
// 
//          if (x > 0):             if (x > 0) {
//              y = 1     becomes       y = 1 ;
//          else: y = 2             } else { y = 2 ; }
//
// A 'case' / 'default' label keeps its ':' (the parser expects "case 1: {"), and a line that is just 'pass' is an
// empty statement. A token's lexeme is its text for names, keywords and numbers, the text between the quotes for a 
// string, "->" for an arrow, and empty for other operators (as before).
void LexicalAnalyzer::ToBracesForm(const vector<LexToken>& lexed)
{
    tokenList.reserve(lexed.size() + lexed.size() / 4);
    auto add = [&](TokenType type, const char* lexeme, int line, SourceLoc loc) {
        Token k;
        k.token_type = type;
        k.lexeme = lexeme;
        k.line_no = line;
        k.loc = loc;
        tokenList.push_back(k);
    };
    auto copy = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; i++)
        {
            const LexToken& t = lexed[i];
            Token k;
            k.token_type = t.kind;
            k.line_no = t.line;
            k.loc = t.loc;
            if (t.symbol) k.lexeme = string(names.text(t.symbol));
            else if (t.kind == NUM || t.kind == FLOAT_LITERAL) k.lexeme.assign(sourceManager.text_at(t.loc), t.length);
            else if (t.kind == ARROW) k.lexeme = "->";
            tokenList.push_back(move(k));
        }
    };
    auto is_pass = [&](size_t a, size_t b) {
        return b - a == 1 && lexed[a].kind == ID && names.text(lexed[a].symbol) == "pass";
    };

    size_t i = 0, n = lexed.size();
    while (i < n && lexed[i].kind != END_OF_FILE)
    {
        const LexToken& t = lexed[i];
        if (t.kind == INDENT) { i++; continue; }
        if (t.kind == DEDENT) { add(RBRACE, "}", t.line, t.loc); i++; continue; }

        // one logical line: up to its NEWLINE
        size_t start = i, end = i;
        int depth = 0, colon = -1;
        for (; lexed[end].kind != NEWLINE; end++)
        {
            TokenType k = lexed[end].kind;
            if (k == LPAREN || k == LBRAC) depth++;
            else if (k == RPAREN || k == RBRAC) depth--;
            else if (depth == 0 && k == COLON && colon < 0) colon = (int)end;
        }
        int line = lexed[start].line;
        SourceLoc loc = lexed[start].loc;
        if (is_pass(start, end))
        {
            // empty statement
        }
        else if (colon < 0)
        {
            copy(start, end);
            add(SEMICOLON, ";", lexed[end - 1].line, lexed[end].loc);
        }
        else
        {
            bool label = (lexed[start].kind == CASE || lexed[start].kind == DEFAULT);
            copy(start, colon + (label ? 1 : 0));
            add(LBRACE, "{", line, lexed[colon].loc);
            if (colon + 1 == (int)end)
            {
                // the body is on the following, indented lines; its DEDENT closes it
            }
            else if (!is_pass(colon + 1, end))
            {
                copy(colon + 1, end);  // a one-line body: if (x): y = 1
                add(SEMICOLON, ";", line, loc);
                add(RBRACE, "}", line, loc);
            }
            else
            {
                add(RBRACE, "}", line, loc);
            }
        }
        i = end + 1;
    }
}

// "target op= expr" becomes "target = target op (expr)" in the token list, before parsing. Every
// kind of target the parser accepts (x, a[i], obj.f, self.f) then gets += -= *= /= without code of
// its own. The target is the run of tokens since the start of the statement, and it is evaluated
// twice (once to read, once to write): the new parser (roadmap phase 2) makes these real operators.
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
            TokenType k= tokenList[i].token_type;
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
        {
            hasMain = true;
        }
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

const Token& LexicalAnalyzer::GetToken()
{
    if (index >= (int)tokenList.size()) return eof;
    return tokenList[index++];
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

const Token& LexicalAnalyzer::peek(int howFar)
{
    if (howFar <= 0)
    {
        cout << "LexicalAnalyzer:peek:Error: non positive argument\n";
        exit(-1);
    }
    int at = index + howFar - 1;
    return at < (int)tokenList.size() ? tokenList[at] : eof;
}
