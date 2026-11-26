#ifndef NOVA_LSP_ANALYSIS_H
#define NOVA_LSP_ANALYSIS_H
// One run of the front end over a document, for the language server: the scanner, the parser and (when the file
// parses) the check pass, exactly as the compiler runs them, plus the indexes that answer an editor's questions
// (what is at this position, where is it defined, what can come next).
//
// Positions here are a document's: 0-based lines, and 0-based columns counted in bytes. The server converts columns
// to and from the UTF-16 code units the Language Server Protocol counts in.

#include <memory>
#include <string>
#include <vector>
#include "check.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parse_tree.h"

struct Position
{
    int line = 0;
    int column = 0;  // bytes
};

struct Range
{
    Position start, end;
};

// A place in a file: the document itself, or an imported module
struct Location
{
    std::string file;
    Range range;
};

struct HoverInfo
{
    std::string code;     // a declaration, shown as Nova code ("int x", "def f(int a) -> int")
    std::string detail;   // a line of explanation, or ""
    Range range;
};

enum class CompletionKind { Keyword, Variable, Function, Method, Field, Class, Struct, Module, TypeParameter, Type };

struct CompletionItem
{
    std::string label;
    CompletionKind kind;
    std::string detail;   // its type or signature
};

struct SignatureInfo
{
    std::string label;                  // def f(int a, int b) -> int
    std::vector<std::string> params;    // "int a", "int b": each a substring of label
    int active = 0;
};

struct DocumentSymbol
{
    std::string name;
    std::string detail;
    enum Kind { Function, Method, Class, Struct, Field, Entry } kind;
    Range range;          // the whole definition
    Range selection;      // its name
    std::vector<DocumentSymbol> children;
};

class Analysis
{
    public:
        // Runs the front end on text, the content of the file at path (its directory is where 'orbit' looks)
        Analysis(const std::string& path, const std::string& text);
        ~Analysis();
        Analysis(const Analysis&) = delete;
        Analysis& operator=(const Analysis&) = delete;

        const std::string& path() const { return file; }
        const std::string& text() const { return source; }
        bool checked() const { return checkRan; }       // the check pass ran (the file parsed)
        const std::vector<Diagnostic>& diagnostics() const { return diags; }

        // Converting a SourceLoc in this analysis into a range in the file it is in (the document or a module)
        bool location_of(SourceLoc begin, SourceLoc end, Location& out) const;

        HoverInfo hover(Position at, bool& found) const;
        bool definition(Position at, Location& out) const;
        std::vector<DocumentSymbol> symbols() const;
        // What can be written at 'at' in text (the document as it is now, which may differ from this analysis's)
        std::vector<CompletionItem> complete(const std::string& text, Position at) const;
        bool signature(const std::string& text, Position at, SignatureInfo& out) const;

        // A byte column in the document's line, from and to UTF-16 code units
        static int utf16_to_byte(const std::string& text, int line, int character);
        static int byte_to_utf16(const std::string& text, int line, int column);

    private:
        std::string file, source;
        SourceLoc buffer = 0;
        std::unique_ptr<std::vector<LexToken>> tokens;
        std::unique_ptr<ParseTree> tree;
        CheckResult result;
        bool checkRan = false;
        std::vector<Diagnostic> diags;

        // Each token's place in the document (line -1: the token is in an imported module, or is layout)
        std::vector<Position> tokenPos;
        std::vector<uint32_t> nodeOfToken;  // the innermost node whose token it is, or NoNode
        std::vector<uint32_t> parentOf;     // each node's parent, NoNode for the root

        uint32_t token_at(Position at) const;
        uint32_t node_at(Position at) const;
        std::string spelling(uint32_t node) const;              // its token as written
        std::string source_of(uint32_t node) const;             // all its source text (a type: vector<int>)
        std::string signature_of(uint32_t functionDecl, std::vector<std::string>* params = nullptr) const;
        std::string describe_class(uint32_t entity) const;
        uint32_t member_decl(uint32_t classEntity, const std::string& name) const;  // a field or a method
        uint32_t enclosing(Position at, bool wantFunction) const;  // the def / ignite (or the class) around 'at'
        uint32_t enclosing_node(uint32_t node, NodeKind kind) const;
        Range range_of(uint32_t node, bool wholeSubtree) const;
        bool before(const Position& a, const Position& b) const
        {
            return a.line < b.line || (a.line == b.line && a.column < b.column);
        }

        // What a receiver (the part before a '.') is, for member completion
        struct Receiver
        {
            enum Kind { None, Class, Struct, Vector, Map, Tensor, Module } kind = None;
            uint32_t entity = NoEntity;
            std::string module;
        };
        Receiver receiver_of_type(TypeId type) const;
        Receiver receiver_of_type_node(uint32_t typeNode) const;
        Receiver resolve_chain(const std::vector<std::string>& chain, Position at) const;
        uint32_t variable_named(const std::string& name, Position at) const;
        void add_members(const Receiver& r, std::vector<CompletionItem>& out) const;
};

#endif
