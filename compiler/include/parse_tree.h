#ifndef NOVA_PARSE_TREE_H
#define NOVA_PARSE_TREE_H
// The parse tree. The parser (parse.cc) reads the scanner's tokens (lexer.h) and writes the tree as one flat array
// of nodes in postorder, as Carbon's parser does: a node's children come right before it, so a node holds no
// pointers, only its kind, its token and the size of its subtree. Building the tree is one push_back per node.
//
// The tree records what the source says and nothing more. Names, types and what a call calls are the check pass's
// job, so the parser keeps no tables: "Point p" is a declaration because a name follows a type, not because Point 
// is a known class, and "Box<int>(5)" is a call because a '(' follows the type arguments.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>
#include "lexer.h"

enum class NodeKind : uint8_t
{
    // the file and its definitions
    File,               // children: the definitions and the ignite block, in order 
    FunctionDecl,       // token: the name. children: ParamList, ReturnTypes if written, Block
    ParamList,          // token: '('. children: Param...
    Param,              // token: the name, or 'self'. children: its type, if written
    ReturnTypes,        // token: '->'. children: one type per value returned
    StructDecl,         // token: the name. children: FieldDecl...
    ClassDecl,          // token: the name. children: TypeParams and Extends if written, then FieldDecl / FunctionDecl
    TypeParams,         // token: '<'. children: TypeParam...
    TypeParam,          // token: the name
    Extends,            // token: 'extends'. children: the parent's type
    FieldDecl,          // token: the name. children: its type, then the size of an array field (int data[256])
    Ignite,             // token: 'ignite'. children: Block

    // types
    TypeName,           // token: a type keyword or a name (int, Tensor, Point, T)
    GenericType,        // token: the name (vector, map, Tensor, Box). children: the type arguments

    // statements
    Block,              // token: the ':' that opens it. children: the statements
    VarDecl,            // token: the name. children: its type, then its initial value if written
    MultiDecl,          // token: '='. children: VarDecl... (without values), then the value they are set from
    ExprStatement,      // token: the expression's first token. children: the expression
    Assign,             // token: '='. children: the target, the value
    CompoundAssign,     // token: '+=', '-=', '*=', or '/='. children: the target, the value
    If,                 // token: 'if'. children: the condition, Block, then Elif... and Else if written
    Elif,               // token: 'elif'. children: the condition, Block
    Else,               // token: 'else'. children: Block
    While,              // token: 'while'. children: the condition, Block
    DoWhile,            // token: 'do'. children: Block, the condition
    For,                // token: 'for'. children: the start, the condition, the step (each may be Empty), Block
    Switch,             // token: 'switch'. children: the value, then Case... and Default
    Case,               // token: 'case'. children: the value, Block
    Default,            // token: 'default'. children: Block
    Return,             // token: 'return'. children: the values
    Break,
    Continue,
    Pass,
    Empty,              // a part of a for header left out. token: the ';' or ')' where it would be

    // expressions
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    BoolLiteral,
    Name,               // token: a name, or 'print' / 'input' / 'array' (they are called as functions are)
    Self,
    Paren,              // token: '('. children: the expression
    Unary,              // token: '-' or 'not'. children: the operand
    Binary,             // token: the operator. children: the left side, the right side
    Call,               // token: '('. children: what is called (a Name, Member, TypeName, or GenericType), the arguments
    Index,              // token: '['. children: the value, the indexes
    Member,             // token: the member's name. children: the value
};

struct ParseNode
{
    NodeKind kind;
    uint32_t token;  // index into the tokens
    uint32_t size;  // the nodes in its subtree, itself included
};

class ParseTree 
{
    public:
        explicit ParseTree(const std::vector<LexToken>& tokens) : tokens(&tokens) {}

        const std::vector<LexToken>* tokens;
        std::vector<ParseNode> nodes;       // postorder: a node's children come right before it, and the root (File) is last
        std::vector<std::string> errors;    // "Error at line N: ..."; when there are any, the tree is incomplete
        
        uint32_t root() const { return (uint32_t)nodes.size() - 1; }
        NodeKind kind(uint32_t n) const { return nodes[n].kind; }
        const LexToken& token(uint32_t n) const { return (*tokens)[nodes[n].token]; }
        std::string_view spelling(uint32_t n) const;        // its token's text as written
        std::vector<uint32_t> children(uint32_t n) const;   // in source order
        void print(std::ostream& out) const;                // as nested lists, one statement per line (--dump-tree)
};

// Parses a whole file's tokens. The parser stops at the first error (error recovery is roadmap phase 3).
ParseTree build_parse_tree(const std::vector<LexToken>& tokens);

#endif  // NOVA_PARSE_TREE_H