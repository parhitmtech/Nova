// The parser: the scanner's tokens in, a ParseTree (parse_tree.h) out. Recursive descent for definitions and 
// statements, precedence climbing for expressions. Every construct is decided from the tokens alone; where that 
// takes more than one token of lookahead (is "Box<int>" a type or a comparison?), the parser reads ahead over a
// type without building anything, as C# does for generic calls.
#include "parse_tree.h"

using namespace std;

namespace {
    bool is_type_keyword(TokenType k)
    {
        return k == INT_TYPE || k == FLOAT_TYPE || k == DOUBLE_TYPE || k == BOOL_TYPE || k == STRING_TYPE || k == TENSOR_TYPE;
    }

    // How tightly a binary operator binds, loosest first; 0 if the token is not one. Level 3 is 'not', a prefix.
    int precedence(TokenType k)
    {
        switch (k)
        {
            case OR:  return 1;
            case AND: return 2;
            case EQEQ: case NOTEQUAL: case LESS: case GREATER: case LESSEQ: case GREATEREQ: return 4;
            case PLUS: case MINUS: return 5;
            case MULT: case DIV: case MOD: case AT: return 6;
            default: return 0;
        }
    }

    class Parser
    {
        public:
            Parser(const vector<LexToken>& tokens, ParseTree& tree) : toks(tokens), tree(tree), pass(names.intern("pass")) {}
            void parse_file();
            
        private: 
            const vector<LexToken>& toks;
            ParseTree& tree;
            Symbol pass;
            uint32_t pos = 0;
            bool failed = false;

            // After an error every token reads as the end of the file, so each loop ends and the parser returns.
            TokenType kind_at(uint32_t i) const { return failed || i >= toks.size() ? END_OF_FILE : toks[i].kind; }
            TokenType kind() const { return kind_at(pos); }
            TokenType peek() const { return kind_at(pos + 1); }
            bool at(TokenType k) const { return kind() == k; }
            uint32_t take()  // the current token's index, moving on to the next token (never past the end)
            {
                uint32_t t = pos;
                if (!failed && pos + 1 < toks.size()) pos++;
                return t;
            }
            bool consume(TokenType k)
            {
                if (!at(k)) return false;
                take();
                return true;
            }
            uint32_t expect(TokenType k, const char* message)
            {
                if (!at(k)) error_got(message);
                return take();
            }
            void error(const string& message);
            void error_got(const char* message) { error(string(message) + ", got " + describe(pos)); }
            string describe(uint32_t i) const;

            // A node over everything pushed since start: those nodes are its children's subtrees.
            size_t mark() const { return tree.nodes.size(); }
            void add(NodeKind kind, uint32_t token, size_t start)
            {
                tree.nodes.push_back({kind, token, (uint32_t)(tree.nodes.size() - start + 1)});
            }

            uint32_t type_end(uint32_t i) const;
            bool at_declaration() const;
            bool at_pass() const { return at(ID) && toks[pos].symbol == pass && peek() == NEWLINE; }

            void parse_function();
            void parse_param();
            void parse_struct();
            void parse_class();
            void parse_members(bool methods);
            void parse_field();
            void parse_type();
            void parse_block();
            void parse_statement();
            void parse_simple_statement();
            void parse_declaration();
            void parse_condition(const char* keyword);
            void parse_if();
            void parse_while();
            void parse_do_while();
            void parse_for();
            void parse_switch();
            void parse_expression() { parse_binary(1); }
            void parse_binary(int min);
            void parse_unary();
            void parse_postfix();
            void parse_primary();
    };

    void Parser::error(const string& message)
    {
        if (failed) return;
        tree.errors.push_back("Error at line " + to_string(toks[pos].line) + ": " + message);
        failed = true;
    }

    string Parser::describe(uint32_t i) const 
    {
        switch (toks[i].kind)
        {
            case NEWLINE:       return "the end of the line";
            case INDENT:        return "an indented line";
            case DEDENT:        return "the end of the block";
            case END_OF_FILE: return "the end of the file";
            default:            return "'" + string(sourceManager.text_at(toks[i].loc), toks[i].length) + "'";
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Definitions
    // ─────────────────────────────────────────────────────────────────────────────

    void Parser::parse_file()
    {
        size_t start = mark();
        while (!at(END_OF_FILE))
        {
            switch(kind())
            {
                case DEF:       parse_function(); break;
                case STRUCT:    parse_struct(); break;
                case CLASS:     parse_class(); break;
                case IGNITE:
                {
                    size_t s = mark();
                    uint32_t t = take();
                    parse_block();
                    add(NodeKind::Ignite, t, s);
                    break;
                }
                default: error_got("expected a definition (def, struct or class) or the 'ignite' block"); break;
            }
        }
        add(NodeKind::File, pos, start);
    }

    // def name(params) -> types: block
    void Parser::parse_function()
    {
        size_t start = mark();
        take();  // 'def'
        uint32_t name = expect(ID, "expected the function's name after 'def'");
        size_t params = mark();
        uint32_t open = expect(LPAREN, "expected '(' after the function's name");
        if (!at(RPAREN))
        {
            do parse_param(); while (consume(COMMA));
        }
        expect(RPAREN, "expected ',' or ')' in the parameter list");
        add(NodeKind::ParamList, open, params);
        if (at(ARROW))
        {
            size_t s = mark();
            uint32_t arrow = take();
            do parse_type(); while (consume(COMMA));  // -> Tensor, Tensor
            add(NodeKind::ReturnTypes, arrow, s);
        }
        parse_block();
        add(NodeKind::FunctionDecl, name, start);
    }

    // self; a name alone (its type comes from each call); or a type and a name
    void Parser::parse_param()
    {
        size_t start = mark();
        if (!at(SELF) && !(at(ID) && (peek() == COMMA || peek() == RPAREN))) parse_type();
        uint32_t name = at(SELF) ? take() : expect(ID, "expected a parameter's name");
        add(NodeKind::Param, name, start);
    }

    void Parser::parse_struct()
    {
        size_t start = mark();
        take();  // 'struct'
        uint32_t name = expect(ID, "expected the struct's name after 'struct'");
        parse_members(false);
        add(NodeKind::StructDecl, name, start);
    }

    // class Name<T, U> extends Parent: then the fields and methods
    void Parser::parse_class()
    {
        size_t start = mark();
        take();  // 'class'
        uint32_t name = expect(ID, "expected the class's name after 'class'");
        if (at(LESS))
        {
            size_t s = mark();
            uint32_t open = take();
            do 
            {
                size_t p = mark();
                add(NodeKind::TypeParam, expect(ID, "expected a type parameter's name"), p);
            } while (consume(COMMA));
            expect(GREATER, "expected ',' or '>' after the type parameters");
            add(NodeKind::TypeParams, open, s);
        }
        if (at(EXTENDS))
        {
            size_t s = mark();
            uint32_t ext = take();
            parse_type();
            add(NodeKind::Extends, ext, s);
        }
        parse_members(true);
        add(NodeKind::ClassDecl, name, start);
    }

    // The indented body of a struct (fields) or a class (fields and methods); 'pass' for an empty one
    void Parser::parse_members(bool methods)
    {
        expect(COLON, "expected ':' to start the body");
        expect(NEWLINE, "expected the body on the lines after ':'");
        expect(INDENT, "expected an indented body");
        while (!at(DEDENT) && !at(END_OF_FILE))
        {
            if (at_pass())
            {
                size_t s = mark();
                add(NodeKind::Pass, take(), s);
                take();  // the end of the line
            }
            else if(methods && at(DEF))
            {
                parse_function();
            }
            else 
            {
                parse_field();
            }
        }
        expect(DEDENT, "expected the end of the body");
    }

    // int top, Node next, or an array field: int data[256]
    void Parser::parse_field()
    {
        size_t start = mark();
        parse_type();
        uint32_t name = expect(ID, "expected the field's name after its type");
        if (consume(LBRAC))
        {
            parse_expression();
            expect(RBRAC, "expected ']' after the array's size");
        }
        expect(NEWLINE, "expected the end of the line after the field");
        add(NodeKind::FieldDecl, name, start);
    }

    // int, Point, T, or a name with type arguments: vector<int>, map<string, vector<int>>, Tensor<float>, Box<Point>
    void Parser::parse_type()
    {
        size_t start = mark();
        if (!is_type_keyword(kind()) && !at(ID))
        {
            error_got("expected a type");
            return;
        }
        uint32_t name = take();
        if (!at(LESS))
        {
            add(NodeKind::TypeName, name, start);
            return;
        }
        take();  // '<'
        do parse_type(); while (consume(COMMA));
        expect(GREATER, "expected ',' or '>' after the type arguments");
        add(NodeKind::GenericType, name, start);
    }

    // Where a type that starts at token i ends (the index after it), or 0 if no type starts there. It builds nothing:
    // it is the lookahead that tells a declaration (Box<int> b) from an expression, and a call (Box<int>(5)) from a 
    // comparison.
    uint32_t Parser::type_end(uint32_t i) const
    {
        if (!is_type_keyword(kind_at(i)) && kind_at(i) != ID) return 0;
        i++;
        if (kind_at(i) != LESS) return i;
        do 
        {
            i = type_end(i + 1);
            if (i == 0) return 0;
        } while (kind_at(i) == COMMA);
        return kind_at(i) == GREATER ? i + 1 : 0;
    }

    // A statement is a declaration when it starts with a type followed by a name: int x, Point p, vector<int> v.
    bool Parser::at_declaration() const 
    {
        uint32_t end = type_end(pos);
        return end != 0 && kind_at(end) == ID;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Statements
    // ─────────────────────────────────────────────────────────────────────────────

    // ':' then the indented lines after it, or one simple statement on the same line: if (x > 0): y = 1
    void Parser::parse_block()
    {
        size_t start = mark();
        uint32_t colon = expect(COLON, "expected ':' to start the block");
        if (consume(NEWLINE))
        {
            expect(INDENT, "expected an indented block");
            while (!at(DEDENT) && !at(END_OF_FILE)) parse_statement();
            expect(DEDENT, "expected the end of the block");
        }
        else
        {
            if (at(IF) || at(WHILE) || at(DO) || at(FOR) || at(SWITCH))
            {
                error_got("only a simple statement can follow ':' on the same line; start the block on the next line");
            }
            parse_simple_statement();
            expect(NEWLINE, "expected the end of the line");
        }
        add(NodeKind::Block, colon, start);
    }

    void Parser::parse_statement()
    {
        switch (kind())
        {
            case IF:        parse_if(); return;
            case WHILE:     parse_while(); return;
            case DO:        parse_do_while(); return;
            case FOR:       parse_for(); return;
            case SWITCH:    parse_switch(); return;
            default:
            {
                parse_simple_statement();
                expect(NEWLINE, "expected the end of the statement");
                return;
            }
        }
    }

    // A statement that fits on one line: a declaration, an assignment, a call, return, break, continue or pass
    void Parser::parse_simple_statement()
    {
        size_t start = mark();
        switch (kind())
        {
            case RETURN:
            {
                uint32_t t = take();
                if (!at(NEWLINE))
                {
                    do parse_expression(); while (consume(COMMA));  // return a, b
                }
                add(NodeKind::Return, t, start);
                return;
            }
            case BREAK:     add(NodeKind::Break, take(), start); return;
            case CONTINUE:  add(NodeKind::Continue, take(), start); return;
            default:        break;
        }
        if (at_pass())
        {
            add(NodeKind::Pass, take(), start);
            return;
        }
        if (at_declaration())
        {
            parse_declaration();
            return;
        }
        uint32_t first = pos;
        parse_expression();
        switch (kind())
        {
            case EQUAL:
            {
                uint32_t op = take();
                parse_expression();
                add(NodeKind::Assign, op, start);
                return;
            }
            case PLUSEQ: case MINUSEQ: case MULTEQ: case DIVEQ:
            {
                uint32_t op = take();
                parse_expression();
                add(NodeKind::CompoundAssign, op, start);
                return;
            }
            default:
            {
                add(NodeKind::ExprStatement, first, start);
                return;
            }
        }
    }

    // int x, int x = 5, Point p = Point(1, 2), vector<int> v; or typed names set from one call: Tensor a, Tensor b = f(t)
    void Parser::parse_declaration()
    {
        size_t start = mark();
        parse_type();
        uint32_t name = take();  // at_declaration() saw the name
        if (!at(COMMA))
        {
            if (consume(EQUAL)) parse_expression();
            add(NodeKind::VarDecl, name, start);
            return;
        }
        add(NodeKind::VarDecl, name, start);
        while (consume(COMMA))
        {
            size_t s = mark();
            parse_type();
            add(NodeKind::VarDecl, expect(ID, "expected a type and a name for each variable (int a, double b = f())"), s);
        }
        uint32_t eq = expect(EQUAL, "expected '=' and the call that sets these variables");
        parse_expression();
        add(NodeKind::MultiDecl, eq, start);
    }

    // The condition of if / elif / while / switch, in parentheses
    void Parser::parse_condition(const char* keyword)
    {
        if (!at(LPAREN))
        {
            error(string("'") + keyword + "' needs its condition in parentheses: " + keyword + " (...)");
            return;
        }
        take();
        parse_expression();
        if (!at(RPAREN))
        {
            error(string("missing ')' after the condition of '") + keyword + "'");
            return;
        }
        take();
    }

    void Parser::parse_if()
    {
        size_t start = mark();
        uint32_t t = take();
        parse_condition("if");
        parse_block();
        while (at(ELIF))
        {
            size_t s = mark();
            uint32_t e = take();
            parse_condition("elif");
            parse_block();
            add(NodeKind::Elif, e, s);
        }
        if (at(ELSE))
        {
            size_t s = mark();
            uint32_t e = take();
            parse_block();
            add(NodeKind::Else, e, s);
        }
        add(NodeKind::If, t, start);
    }

    void Parser::parse_while()
    {
        size_t start = mark();
        uint32_t t = take();
        parse_condition("while");
        parse_block();
        add(NodeKind::While, t, start);
    }

    // do: then the body, and while (condition) on the line after it
    void Parser::parse_do_while()
    {
        size_t start = mark();
        uint32_t t = take();
        parse_block();
        expect(WHILE, "expected 'while (...)' after the body of 'do'");
        parse_condition("while");
        expect(NEWLINE, "expected the end of the line after the condition");
        add(NodeKind::DoWhile, t, start);
    }

    // for (start; condition; step): any part can be left out
    void Parser::parse_for()
    {
        size_t start = mark();
        uint32_t t = take();
        expect(LPAREN, "expected '(' after 'for'");
        for (int part = 0; part < 3; part++)
        {
            TokenType end = part < 2 ? SEMICOLON : RPAREN;
            if (at(end))
            {
                size_t s = mark();
                add(NodeKind::Empty, pos, s);
            }
            else if (part == 1)
            {
                parse_expression();
            }
            else
            {
                parse_simple_statement();
            }
            expect(end, part < 2 ? "expected ';' in the 'for' header" : "expected ')' to close the 'for' header");
        }
        parse_block();
        add(NodeKind::For, t, start);
    }

    // switch (x): then the indented case / default labels, each with its block
    void Parser::parse_switch()
    {
        size_t start = mark();
        uint32_t t = take();
        parse_condition("switch");
        expect(COLON, "expected ':' after the switch's value");
        expect(NEWLINE, "expected the cases on the lines after 'switch (...):'");
        expect(INDENT, "expected indented 'case' labels");
        while (!at(DEDENT) && !at(END_OF_FILE))
        {
            size_t s = mark();
            if (at(CASE))
            {
                uint32_t c = take();
                parse_expression();
                parse_block();
                add(NodeKind::Case, c, s);
            }
            else if (at(DEFAULT))
            {
                uint32_t d = take();
                parse_block();
                add(NodeKind::Default, d, s);
            }
            else
            {
                error_got("expected 'case' or 'default'");
            }
        }
        expect(DEDENT, "expected the end of the switch");
        add(NodeKind::Switch, t, start);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Expressions
    // ─────────────────────────────────────────────────────────────────────────────

    // Binary operators that bind at least as tightly as min, left to right (precedence climbing). 'not' sits between
    // 'and' and the comparisons, so "not a == b" is "not (a == b)"; unary '-' binds tighter than any binary operator.
    void Parser::parse_binary(int min)
    {
        size_t start = mark();
        if (min <= 3 && at(NOT))
        {
            uint32_t op = take();
            parse_binary(3);
            add(NodeKind::Unary, op, start);
        }
        else
        {
            parse_unary();
        }
        while (precedence(kind()) >= min)  // min is at least 1, so this is a binary operator
        {
            int level = precedence(kind());
            uint32_t op = take();
            parse_binary(level + 1);
            add(NodeKind::Binary, op, start);
            if (level == 4 && precedence(kind()) == 4)
            {
                error("comparisons cannot be chained; use 'and', e.g. (a < b and b < c)");
            }
        }
    }

    void Parser::parse_unary()
    {
        if (!at(MINUS))
        {
            parse_postfix();
            return;
        }
        size_t start = mark();
        uint32_t op = take();
        parse_unary();
        add(NodeKind::Unary, op, start);
    }

    // A primary and what follows it, in any order: calls f(x), indexes t[i, j], members a.b (make().x, l.ws[1][0, 0])
    void Parser::parse_postfix()
    {
        size_t start = mark();
        parse_primary();
        while (true)
        {
            if (at(LPAREN))
            {
                uint32_t open = take();
                if (!at(RPAREN))
                {
                    do parse_expression(); while (consume(COMMA));
                }
                expect(RPAREN, "expected ',' or ')' in the call's arguments");
                add(NodeKind::Call, open, start);
            }
            else if (at(LBRAC))
            {
                uint32_t open = take();
                do parse_expression(); while (consume(COMMA));
                expect(RBRAC, "expected ',' or ']' after the index");
                add(NodeKind::Index, open, start);
            }
            else if (at(DOT))
            {
                take();
                add(NodeKind::Member, expect(ID, "expected a name after '.'"), start);
            }
            else
            {
                return;
            }
        }
    }

    void Parser::parse_primary()
    {
        size_t start = mark();
        switch (kind())
        {
            case NUM:           add(NodeKind::IntLiteral, take(), start); return;
            case FLOAT_LITERAL: add(NodeKind::FloatLiteral, take(), start); return;
            case STRING:        add(NodeKind::StringLiteral, take(), start); return;
            case TRUE:
            case FALSE:         add(NodeKind::BoolLiteral, take(), start); return;
            case SELF:          add(NodeKind::Self, take(), start); return;
            case PRINT:
            case INPUT:
            case ARRAY:         add(NodeKind::Name, take(), start); return;
            case LPAREN:        
            {
                uint32_t open = take();
                parse_expression();
                expect(RPAREN, "expected ')'");
                add(NodeKind::Paren, open, start);
                return;
            }
            case ID:
            {
                // Box<int>(5), vector<int>(): type arguments and then '(' make a call; otherwise '<' is less-than
                uint32_t end = peek() == LESS ? type_end(pos) : 0;
                if (end != 0 && kind_at(end) == LPAREN) parse_type();
                else add(NodeKind::Name, take(), start);
                return;
            }
            default:
            {
                if (is_type_keyword(kind())) parse_type();  // float(x), int(y): a conversion
                else error_got("expected an expression");
                return;
            }
        }
    }
}  // namespace

ParseTree build_parse_tree(const vector<LexToken>& tokens)
{
    ParseTree tree(tokens);
    tree.nodes.reserve(tokens.size());
    Parser(tokens, tree).parse_file();
    return tree;
}