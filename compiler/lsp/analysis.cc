// The language server's view of a document (analysis.h): the front end's run over it, and the answers to an
// editor's questions, worked out from the parse tree and the check pass's result.
#include <algorithm>
#include <map>
#include <set>
#include "analysis.h"
#include "runtime.h"  // preprocess_import

using namespace std;

extern const map<string, TensorOp> TENSOR_OP_MAP;  // builtins.cc

namespace
{
    bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

    bool is_layout(TokenType k) { return k == NEWLINE || k == INDENT || k == DEDENT || k == END_OF_FILE; }

    // The byte offset where line (0-based) starts in text, or npos past the last line
    size_t line_start(const string& text, int line)
    {
        size_t at = 0;
        for (int l = 0; l < line; l++)
        {
            at = text.find('\n', at);
            if (at == string::npos) return string::npos;
            at++;
        }
        return at;
    }

    string line_of(const string& text, int line)
    {
        size_t s = line_start(text, line);
        if (s == string::npos) return "";
        size_t e = text.find('\n', s);
        string l = text.substr(s, e == string::npos ? string::npos : e - s);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        return l;
    }

    // The names before a '.' (or a '('), read backwards from end in line: "self.items" -> {"self", "items"}
    vector<string> chain_before(const string& line, size_t end)
    {
        vector<string> chain;
        size_t q = end;
        while (true)
        {
            while (q > 0 && line[q - 1] == ' ') q--;
            size_t e = q;
            while (q > 0 && is_ident(line[q - 1])) q--;
            if (q == e) return {};
            chain.insert(chain.begin(), line.substr(q, e - q));
            if (q > 0 && line[q - 1] == '.')
            {
                q--;
                continue;
            }
            return chain;
        }
    }

    struct Builtin { const char* name; const char* detail; };
    const Builtin vectorMethods[] = {
        {"push", "push(x): add x at the end"}, {"pop", "pop(): remove the last element"},
        {"size", "size() -> int"}, {"contains", "contains(x) -> bool"}, {"index_of", "index_of(x) -> int (-1: none)"},
        {"binary_search", "binary_search(x) -> int, in a sorted vector"}, {"min", "min(): the smallest element"},
        {"max", "max(): the largest element"}, {"sum", "sum(): the sum of the elements"}, {"sort", "sort()"},
        {"reverse", "reverse()"}, {"clear", "clear(): remove every element"},
    };
    const Builtin mapMethods[] = {
        {"has", "has(k) -> bool"}, {"size", "size() -> int"}, {"remove", "remove(k)"},
        {"clear", "clear(): remove every key"},
    };
    const Builtin tensorMembers[] = {
        {"T", "the transpose"}, {"rows", "int: the number of rows"}, {"cols", "int: the number of columns"},
        {"rank", "int: the number of dimensions"},
    };
    const Builtin hfMethods[] = {
        {"infer", "hf.infer(model, input) -> string"}, {"generate", "hf.generate(model, prompt, max_tokens=N) -> string"},
        {"classify", "hf.classify(model, text) -> string"}, {"dataset", "hf.dataset(name[, split]) -> string"},
        {"set_token", "hf.set_token(token)"},
    };
    const Builtin solMethods[] = {
        {"finetune", "sol.finetune(model=..., task=..., dataset=..., epochs=..., lr=..., batch_size=...) -> string"},
        {"wait", "sol.wait(job)"}, {"predict", "sol.predict(job, input) -> string"}, {"save", "sol.save(job, path)"},
        {"set_key", "sol.set_key(path)"},
    };
    const char* const keywords[] = {
        "ignite", "def", "struct", "class", "extends", "self", "return", "if", "elif", "else", "while", "do", "for",
        "switch", "case", "default", "break", "continue", "pass", "and", "or", "not", "true", "false", "orbit", "none",
    };
    const char* const typeKeywords[] = {"int", "float", "double", "bool", "string", "Tensor"};
}

// ─────────────────────────────────────────────────────────────────────────────
// The run
// ─────────────────────────────────────────────────────────────────────────────

Analysis::Analysis(const string& path, const string& text) : file(path), source(text)
{
    ::diagnostics.clear();
    ::diagnostics.set_error_limit(100);
    ::diagnostics.set_warnings(true);
    size_t slash = path.find_last_of("/\\");
    string dir = slash == string::npos ? "." : path.substr(0, slash);
    set<string> imported;
    string processed = preprocess_import(text, dir, imported, path);
    buffer = sourceManager.next_location();
    tokens.reset(new vector<LexToken>(scan_program(processed, path)));
    tree.reset(new ParseTree(build_parse_tree(*tokens)));
    if (!::diagnostics.has_errors())
    {
        result = check_tree(*tree);
        checkRan = true;
    }
    diags = ::diagnostics.all();

    // Where each token is in the document
    const vector<LexToken>& toks = *tokens;
    tokenPos.assign(toks.size(), Position{-1, -1});
    for (size_t i = 0; i < toks.size(); i++)
    {
        if (is_layout(toks[i].kind)) continue;
        PresumedLoc p = sourceManager.presumed(toks[i].loc);
        if (p.file == file) tokenPos[i] = Position{p.line - 1, p.column - 1};
    }
    nodeOfToken.assign(toks.size(), NoNode);
    parentOf.assign(tree->nodes.size(), NoNode);
    for (uint32_t n = 0; n < tree->nodes.size(); n++)
    {
        uint32_t t = tree->nodes[n].token;
        if (t < nodeOfToken.size() && nodeOfToken[t] == NoNode) nodeOfToken[t] = n;  // postorder: innermost first
        for (uint32_t c : tree->children(n)) parentOf[c] = n;
    }
}

Analysis::~Analysis()
{
    sourceManager.remove_buffer(buffer);
}

bool Analysis::location_of(SourceLoc begin, SourceLoc end, Location& out) const
{
    PresumedLoc b = sourceManager.presumed(begin);
    if (b.file.empty()) return false;
    PresumedLoc e = sourceManager.presumed(end);
    out.file = b.file;
    out.range.start = Position{b.line - 1, b.column - 1};
    out.range.end = e.file == b.file ? Position{e.line - 1, e.column - 1} : out.range.start;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Positions
// ─────────────────────────────────────────────────────────────────────────────

int Analysis::utf16_to_byte(const string& text, int line, int character)
{
    string l = line_of(text, line);
    int units = 0;
    size_t i = 0;
    while (i < l.size() && units < character)
    {
        unsigned char c = (unsigned char)l[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return (int)min(i, l.size()) + (character > units ? character - units : 0);
}

int Analysis::byte_to_utf16(const string& text, int line, int column)
{
    string l = line_of(text, line);
    int units = 0;
    size_t i = 0;
    while (i < l.size() && (int)i < column)
    {
        unsigned char c = (unsigned char)l[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return units + (column > (int)l.size() ? column - (int)l.size() : 0);
}

uint32_t Analysis::token_at(Position at) const
{
    uint32_t best = NoNode;
    for (uint32_t i = 0; i < tokenPos.size(); i++)
    {
        const Position& p = tokenPos[i];
        if (p.line != at.line || p.column > at.column) continue;
        int end = p.column + (int)(*tokens)[i].length;
        if (at.column < end) return i;
        if (at.column == end && (*tokens)[i].length > 0) best = i;  // the cursor just after a name
    }
    return best;
}

uint32_t Analysis::node_at(Position at) const
{
    uint32_t t = token_at(at);
    return t == NoNode ? NoNode : nodeOfToken[t];
}

Range Analysis::range_of(uint32_t node, bool wholeSubtree) const
{
    const LexToken& t = tree->token(node);
    SourceRange r = wholeSubtree ? tree->range(node) : SourceRange{t.loc, t.loc + (t.length ? t.length : 1)};
    // A definition's token is its name: the whole of it starts at the keyword before (def, class, struct)
    NodeKind k = tree->kind(node);
    uint32_t ti = tree->nodes[node].token;
    if (wholeSubtree && ti > 0 && (k == NodeKind::FunctionDecl || k == NodeKind::ClassDecl || k == NodeKind::StructDecl))
    {
        r.begin = (*tokens)[ti - 1].loc;
    }
    Location l;
    if (!location_of(r.begin, r.end, l) || l.file != file) return Range{Position{-1, -1}, Position{-1, -1}};
    return l.range;
}

string Analysis::spelling(uint32_t node) const { return string(tree->spelling(node)); }

string Analysis::source_of(uint32_t node) const
{
    SourceRange r = tree->range(node);
    return string(sourceManager.text_at(r.begin), r.end - r.begin);
}

uint32_t Analysis::enclosing_node(uint32_t node, NodeKind kind) const
{
    for (uint32_t n = node; n != NoNode; n = parentOf[n]) if (tree->kind(n) == kind) return n;
    return NoNode;
}

// The innermost function (or the ignite block) whose source holds 'at'; with wantFunction false, a class too
uint32_t Analysis::enclosing(Position at, bool wantFunction) const
{
    uint32_t best = NoNode;
    Position bestStart{-1, -1};
    for (uint32_t n = 0; n < tree->nodes.size(); n++)
    {
        NodeKind k = tree->kind(n);
        bool want = k == NodeKind::FunctionDecl || k == NodeKind::Ignite || (!wantFunction && k == NodeKind::ClassDecl);
        if (!want) continue;
        Range r = range_of(n, true);
        if (r.start.line < 0) continue;
        bool inside = !before(at, r.start) && (before(at, r.end) || at.line == r.end.line);
        // a body ends at its last token: the lines typed after it, more indented, still belong to it
        if (!inside && at.line > r.end.line) inside = false;
        if (inside && !before(r.start, bestStart)) { best = n; bestStart = r.start; }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
// Declarations as code
// ─────────────────────────────────────────────────────────────────────────────

string Analysis::signature_of(uint32_t fn, vector<string>* params) const
{
    vector<uint32_t> kids = tree->children(fn);  // ParamList, ReturnTypes if written, Block
    string s = "def " + spelling(fn) + "(";
    bool first = true;
    for (uint32_t p : tree->children(kids[0]))
    {
        if (tree->token(p).kind == SELF) continue;
        vector<uint32_t> type = tree->children(p);
        string param = type.empty() ? spelling(p) : source_of(type[0]) + " " + spelling(p);
        s += (first ? "" : ", ") + param;
        if (params) params->push_back(param);
        first = false;
    }
    s += ")";
    if (kids.size() == 3)
    {
        vector<uint32_t> rets = tree->children(kids[1]);
        s += " -> ";
        for (size_t i = 0; i < rets.size(); i++) s += (i ? ", " : "") + source_of(rets[i]);
    }
    else if (checkRan)
    {
        auto it = result.signatures.find(fn);
        if (it != result.signatures.end() && !it->second.generic && !it->second.returns.empty())
        {
            s += " -> ";
            for (size_t i = 0; i < it->second.returns.size(); i++)
            {
                s += (i ? ", " : "") + type_name(result, it->second.returns[i]);
            }
        }
    }
    return s;
}

string Analysis::describe_class(uint32_t entity) const
{
    const Entity& e = result.entities[entity];
    if (e.decl == NoNode) return names.text(e.name).data();
    bool isStruct = tree->kind(e.decl) == NodeKind::StructDecl;
    string s = (isStruct ? "struct " : "class ") + spelling(e.decl);
    string fields;
    for (uint32_t m : tree->children(e.decl))
    {
        switch (tree->kind(m))
        {
            case NodeKind::TypeParams:
            {
                s += "<";
                vector<uint32_t> ps = tree->children(m);
                for (size_t i = 0; i < ps.size(); i++) s += (i ? ", " : "") + spelling(ps[i]);
                s += ">";
                break;
            }
            case NodeKind::Extends: s += " extends " + source_of(tree->children(m)[0]); break;
            case NodeKind::FieldDecl: fields += "\n    " + source_of(tree->children(m)[0]) + " " + spelling(m); break;
            default: break;
        }
    }
    return s + ":" + (fields.empty() ? "\n    pass" : fields);
}

uint32_t Analysis::member_decl(uint32_t entity, const string& name) const
{
    for (size_t steps = 0; entity < result.entities.size() && steps < 64; steps++)
    {
        uint32_t decl = result.entities[entity].decl;
        if (decl == NoNode) return NoNode;
        uint32_t parent = NoEntity;
        for (uint32_t m : tree->children(decl))
        {
            NodeKind k = tree->kind(m);
            if ((k == NodeKind::FieldDecl || k == NodeKind::FunctionDecl) && spelling(m) == name) return m;
            if (k == NodeKind::Extends && checkRan) parent = result.refs[tree->children(m)[0]];
        }
        entity = parent;
    }
    return NoNode;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hover and definition
// ─────────────────────────────────────────────────────────────────────────────

Analysis::Receiver Analysis::receiver_of_type(TypeId type) const
{
    Receiver r;
    if (type >= TypeTable::None + 1 || type == TypeTable::Tensor)
    {
        const Type& t = result.types[type];
        switch (t.kind)
        {
            case TypeKind::Class: r.kind = Receiver::Class; r.entity = t.a; break;
            case TypeKind::Struct: r.kind = Receiver::Struct; r.entity = t.a; break;
            case TypeKind::Vector: r.kind = Receiver::Vector; break;
            case TypeKind::Map: r.kind = Receiver::Map; break;
            case TypeKind::Tensor: r.kind = Receiver::Tensor; break;
            default: break;
        }
    }
    return r;
}

Analysis::Receiver Analysis::receiver_of_type_node(uint32_t typeNode) const
{
    Receiver r;
    const LexToken& t = tree->token(typeNode);
    if (t.kind == TENSOR_TYPE)
    {
        r.kind = Receiver::Tensor;
        return r;
    }
    string name = spelling(typeNode);
    if (tree->kind(typeNode) == NodeKind::GenericType && (name == "vector" || name == "map"))
    {
        r.kind = name == "vector" ? Receiver::Vector : Receiver::Map;
        return r;
    }
    if (!checkRan || typeNode >= result.refs.size()) return r;
    uint32_t e = result.refs[typeNode];
    if (e == NoEntity) return r;
    if (result.entities[e].kind == EntityKind::Class) r.kind = Receiver::Class;
    else if (result.entities[e].kind == EntityKind::Struct) r.kind = Receiver::Struct;
    else return r;
    r.entity = e;
    return r;
}

HoverInfo Analysis::hover(Position at, bool& found) const
{
    HoverInfo h;
    found = false;
    uint32_t n = node_at(at);
    if (n == NoNode) return h;
    h.range = range_of(n, false);
    NodeKind k = tree->kind(n);
    string name = spelling(n);
    found = true;

    if (k == NodeKind::FieldDecl)
    {
        uint32_t owner = parentOf[n];
        h.code = source_of(tree->children(n)[0]) + " " + name;
        if (owner != NoNode) h.detail = "field of " + spelling(owner);
        return h;
    }
    if (k == NodeKind::FunctionDecl && !checkRan)
    {
        h.code = signature_of(n);
        return h;
    }
    if (!checkRan) { found = false; return h; }

    if (k == NodeKind::Member)
    {
        uint32_t value = tree->children(n)[0];
        TypeId t = value < result.nodeTypes.size() ? result.nodeTypes[value] : TypeTable::Unknown;
        Receiver r = receiver_of_type(t);
        if (r.kind == Receiver::Class || r.kind == Receiver::Struct)
        {
            uint32_t m = member_decl(r.entity, name);
            string owner = names.text(result.entities[r.entity].name).data();
            if (m != NoNode && tree->kind(m) == NodeKind::FieldDecl)
            {
                h.code = source_of(tree->children(m)[0]) + " " + name;
                h.detail = "field of " + owner;
                return h;
            }
            if (m != NoNode)
            {
                h.code = signature_of(m);
                h.detail = "method of " + owner;
                return h;
            }
        }
        const Builtin* list = nullptr;
        size_t count = 0;
        if (r.kind == Receiver::Vector) { list = vectorMethods; count = sizeof vectorMethods / sizeof *vectorMethods; }
        if (r.kind == Receiver::Map) { list = mapMethods; count = sizeof mapMethods / sizeof *mapMethods; }
        if (r.kind == Receiver::Tensor) { list = tensorMembers; count = sizeof tensorMembers / sizeof *tensorMembers; }
        for (size_t i = 0; i < count; i++)
        {
            if (name == list[i].name)
            {
                h.code = list[i].detail;
                h.detail = r.kind == Receiver::Tensor ? "tensor member" : r.kind == Receiver::Map ? "map method" : "vector method";
                return h;
            }
        }
        if (n < result.nodeTypes.size() && result.nodeTypes[n] != TypeTable::Unknown)
        {
            h.code = type_name(result, result.nodeTypes[n]);
            return h;
        }
        found = false;
        return h;
    }

    uint32_t e = n < result.refs.size() ? result.refs[n] : NoEntity;
    if (e != NoEntity)
    {
        const Entity& en = result.entities[e];
        switch (en.kind)
        {
            case EntityKind::Variable:
            {
                bool param = en.decl != NoNode && tree->kind(en.decl) == NodeKind::Param;
                h.code = (en.type != TypeTable::Unknown ? type_name(result, en.type) + " " : "") + name;
                h.detail = param ? "parameter" : "variable";
                if (param && en.type == TypeTable::Unknown) h.detail = "parameter (untyped: the function is generic)";
                return h;
            }
            case EntityKind::Function:
            {
                h.code = signature_of(en.decl);
                auto it = result.signatures.find(en.decl);
                if (it != result.signatures.end() && it->second.generic)
                {
                    h.detail = "generic: compiled once for each combination of argument types";
                }
                return h;
            }
            case EntityKind::Struct:
            case EntityKind::Class: h.code = describe_class(e); return h;
            case EntityKind::TypeParam: h.code = name; h.detail = "type parameter"; return h;
            case EntityKind::Builtin: h.code = name + "(...)"; h.detail = "built-in function"; return h;
            case EntityKind::BuiltinType:
                h.code = name == "map" ? "map<K, V>" : "vector<T>";
                h.detail = name == "map" ? "a map from int or string keys" : "a growable list";
                return h;
            case EntityKind::Module:
                h.code = "module " + name;
                h.detail = name == "hf" ? "built-in: Hugging Face models" : "built-in: training on the SOL cluster's GPUs";
                return h;
            case EntityKind::None: h.code = "none"; h.detail = "the object that is no object"; return h;
        }
    }
    if (k == NodeKind::Self)
    {
        uint32_t cls = enclosing_node(n, NodeKind::ClassDecl);
        if (cls != NoNode) { h.code = "self: " + spelling(cls); h.detail = "the object the method was called on"; return h; }
    }
    if (n < result.nodeTypes.size() && result.nodeTypes[n] != TypeTable::Unknown)
    {
        h.code = type_name(result, result.nodeTypes[n]);
        return h;
    }
    found = false;
    return h;
}

bool Analysis::definition(Position at, Location& out) const
{
    uint32_t n = node_at(at);
    if (n == NoNode || !checkRan) return false;
    uint32_t decl = NoNode;
    if (tree->kind(n) == NodeKind::Member)
    {
        uint32_t value = tree->children(n)[0];
        TypeId t = value < result.nodeTypes.size() ? result.nodeTypes[value] : TypeTable::Unknown;
        Receiver r = receiver_of_type(t);
        if (r.kind == Receiver::Class || r.kind == Receiver::Struct) decl = member_decl(r.entity, spelling(n));
    }
    else
    {
        uint32_t e = n < result.refs.size() ? result.refs[n] : NoEntity;
        if (e != NoEntity) decl = result.entities[e].decl;
    }
    if (decl == NoNode) return false;
    const LexToken& t = tree->token(decl);
    return location_of(t.loc, t.loc + t.length, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// The outline
// ─────────────────────────────────────────────────────────────────────────────

vector<DocumentSymbol> Analysis::symbols() const
{
    vector<DocumentSymbol> out;
    for (uint32_t n : tree->children(tree->root()))
    {
        NodeKind k = tree->kind(n);
        DocumentSymbol s;
        s.range = range_of(n, true);
        if (s.range.start.line < 0) continue;  // an imported module's
        s.selection = range_of(n, false);
        switch (k)
        {
            case NodeKind::FunctionDecl:
                s.name = spelling(n);
                s.kind = DocumentSymbol::Function;
                s.detail = signature_of(n).substr(4 + s.name.size());
                break;
            case NodeKind::StructDecl:
            case NodeKind::ClassDecl:
            {
                s.name = spelling(n);
                s.kind = k == NodeKind::StructDecl ? DocumentSymbol::Struct : DocumentSymbol::Class;
                for (uint32_t m : tree->children(n))
                {
                    DocumentSymbol c;
                    c.range = range_of(m, true);
                    c.selection = range_of(m, false);
                    c.name = spelling(m);
                    if (tree->kind(m) == NodeKind::FieldDecl)
                    {
                        c.kind = DocumentSymbol::Field;
                        c.detail = source_of(tree->children(m)[0]);
                    }
                    else if (tree->kind(m) == NodeKind::FunctionDecl)
                    {
                        c.kind = DocumentSymbol::Method;
                        c.detail = signature_of(m).substr(4 + c.name.size());
                    }
                    else continue;
                    s.children.push_back(c);
                }
                break;
            }
            case NodeKind::Ignite:
                s.name = "ignite";
                s.kind = DocumentSymbol::Entry;
                s.detail = "the entry point";
                break;
            default: continue;
        }
        out.push_back(s);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Completion and signature help
// ─────────────────────────────────────────────────────────────────────────────

// The variable called name that is visible at 'at': declared earlier in the same function, the latest one
uint32_t Analysis::variable_named(const string& name, Position at) const
{
    uint32_t fn = enclosing(at, true);
    uint32_t best = NoEntity, any = NoEntity;
    for (uint32_t e = 0; e < result.entities.size(); e++)
    {
        const Entity& v = result.entities[e];
        if (v.kind != EntityKind::Variable || v.decl == NoNode || names.text(v.name) != name) continue;
        any = e;
        Range r = range_of(v.decl, false);
        if (r.start.line < 0 || !before(r.start, at)) continue;
        uint32_t declFn = enclosing_node(v.decl, NodeKind::FunctionDecl);
        if (declFn == NoNode) declFn = enclosing_node(v.decl, NodeKind::Ignite);
        if (declFn == fn) best = e;
    }
    return best != NoEntity ? best : any;  // positions may have moved since this analysis: any one of that name
}

Analysis::Receiver Analysis::resolve_chain(const vector<string>& chain, Position at) const
{
    Receiver r;
    if (chain.empty() || !checkRan) return r;
    if (chain[0] == "self")
    {
        uint32_t fn = enclosing(at, true);
        uint32_t cls = fn == NoNode ? NoNode : enclosing_node(fn, NodeKind::ClassDecl);
        if (cls != NoNode && result.refs[cls] != NoEntity)
        {
            r.kind = Receiver::Class;
            r.entity = result.refs[cls];
        }
    }
    else if (chain[0] == "hf" || chain[0] == "sol")
    {
        r.kind = Receiver::Module;
        r.module = chain[0];
    }
    else
    {
        uint32_t v = variable_named(chain[0], at);
        if (v != NoEntity) r = receiver_of_type(result.entities[v].type);
    }
    for (size_t i = 1; i < chain.size(); i++)
    {
        if (r.kind != Receiver::Class && r.kind != Receiver::Struct) return Receiver();
        uint32_t m = member_decl(r.entity, chain[i]);
        if (m == NoNode || tree->kind(m) != NodeKind::FieldDecl) return Receiver();
        r = receiver_of_type_node(tree->children(m)[0]);
    }
    return r;
}

void Analysis::add_members(const Receiver& r, vector<CompletionItem>& out) const
{
    auto add_list = [&](const Builtin* list, size_t count, CompletionKind kind) {
        for (size_t i = 0; i < count; i++) out.push_back({list[i].name, kind, list[i].detail});
    };
    switch (r.kind)
    {
        case Receiver::Class:
        case Receiver::Struct:
        {
            set<string> seen;  // a method that overrides its parent's is listed once
            uint32_t entity = r.entity;
            for (size_t steps = 0; entity < result.entities.size() && steps < 64; steps++)
            {
                uint32_t decl = result.entities[entity].decl;
                if (decl == NoNode) break;
                uint32_t parent = NoEntity;
                for (uint32_t m : tree->children(decl))
                {
                    NodeKind k = tree->kind(m);
                    if (k == NodeKind::Extends) parent = result.refs[tree->children(m)[0]];
                    if (k != NodeKind::FieldDecl && k != NodeKind::FunctionDecl) continue;
                    string name = spelling(m);
                    if (name == "init" || !seen.insert(name).second) continue;
                    if (k == NodeKind::FieldDecl)
                        out.push_back({name, CompletionKind::Field, source_of(tree->children(m)[0]) + " " + name});
                    else
                        out.push_back({name, CompletionKind::Method, signature_of(m)});
                }
                entity = parent;
            }
            return;
        }
        case Receiver::Vector: add_list(vectorMethods, sizeof vectorMethods / sizeof *vectorMethods, CompletionKind::Method); return;
        case Receiver::Map: add_list(mapMethods, sizeof mapMethods / sizeof *mapMethods, CompletionKind::Method); return;
        case Receiver::Tensor: add_list(tensorMembers, sizeof tensorMembers / sizeof *tensorMembers, CompletionKind::Field); return;
        case Receiver::Module:
            if (r.module == "hf") add_list(hfMethods, sizeof hfMethods / sizeof *hfMethods, CompletionKind::Method);
            else add_list(solMethods, sizeof solMethods / sizeof *solMethods, CompletionKind::Method);
            return;
        case Receiver::None: return;
    }
}

vector<CompletionItem> Analysis::complete(const string& text, Position at) const
{
    vector<CompletionItem> out;
    string line = line_of(text, at.line);
    size_t col = min((size_t)max(at.column, 0), line.size());
    size_t p = col;
    while (p > 0 && is_ident(line[p - 1])) p--;  // the start of the name being typed
    if (p > 0 && line[p - 1] == '.')
    {
        add_members(resolve_chain(chain_before(line, p - 1), at), out);
        return out;
    }
    // A comment or a string: nothing to complete
    bool inString = false;
    for (size_t i = 0; i < p; i++)
    {
        if (line[i] == '"') inString = !inString;
        else if (line[i] == '#' && !inString) return out;
    }
    if (inString) return out;

    set<string> seen;
    auto add = [&](const string& label, CompletionKind kind, const string& detail) {
        if (seen.insert(label).second) out.push_back({label, kind, detail});
    };
    for (const char* k : keywords) add(k, CompletionKind::Keyword, "");
    for (const char* t : typeKeywords) add(t, CompletionKind::Type, "type");
    // the variables visible here: declared before 'at', in the same function
    if (checkRan)
    {
        uint32_t fn = enclosing(at, true);
        for (uint32_t e = 0; e < result.entities.size(); e++)
        {
            const Entity& v = result.entities[e];
            if (v.kind != EntityKind::Variable || v.decl == NoNode) continue;
            Range r = range_of(v.decl, false);
            if (r.start.line < 0 || !before(r.start, at)) continue;
            uint32_t declFn = enclosing_node(v.decl, NodeKind::FunctionDecl);
            if (declFn == NoNode) declFn = enclosing_node(v.decl, NodeKind::Ignite);
            if (declFn != fn) continue;
            string name(names.text(v.name));
            add(name, CompletionKind::Variable, v.type != TypeTable::Unknown ? type_name(result, v.type) : "");
        }
    }
    // the file's definitions, and those of the modules it brings in
    for (uint32_t n : tree->children(tree->root()))
    {
        NodeKind k = tree->kind(n);
        if (k == NodeKind::FunctionDecl) add(spelling(n), CompletionKind::Function, signature_of(n));
        else if (k == NodeKind::ClassDecl) add(spelling(n), CompletionKind::Class, "class");
        else if (k == NodeKind::StructDecl) add(spelling(n), CompletionKind::Struct, "struct");
    }
    for (const char* b : {"print", "input", "array"}) add(b, CompletionKind::Function, "built-in");
    add("vector", CompletionKind::Type, "vector<T>");
    add("map", CompletionKind::Type, "map<K, V>");
    add("hf", CompletionKind::Module, "Hugging Face models");
    add("sol", CompletionKind::Module, "training on the SOL cluster");
    for (const auto& op : TENSOR_OP_MAP) add(op.first, CompletionKind::Function, "built-in tensor function");
    return out;
}

bool Analysis::signature(const string& text, Position at, SignatureInfo& out) const
{
    // The '(' that is still open before the cursor, and the commas after it at its own depth
    size_t offset = line_start(text, at.line);
    if (offset == string::npos) return false;
    offset = min(offset + (size_t)max(at.column, 0), text.size());
    int depth = 0, commas = 0;
    size_t open = string::npos;
    for (size_t i = offset; i-- > 0;)
    {
        char c = text[i];
        if (c == ')' || c == ']') depth++;
        else if (c == '(' || c == '[')
        {
            if (depth == 0)
            {
                if (c == '(') open = i;
                break;
            }
            depth--;
        }
        else if (c == ',' && depth == 0) commas++;
        else if (c == ':' && depth == 0) break;  // a block's ':': no call is open
    }
    if (open == string::npos || !checkRan) return false;
    size_t lineBegin = text.rfind('\n', open);
    lineBegin = lineBegin == string::npos ? 0 : lineBegin + 1;
    string line = text.substr(lineBegin, open - lineBegin);
    vector<string> chain = chain_before(line, line.size());
    if (chain.empty()) return false;

    uint32_t fn = NoNode;
    string constructorOf;
    if (chain.size() == 1)
    {
        for (uint32_t n : tree->children(tree->root()))
        {
            if (spelling(n) != chain[0]) continue;
            if (tree->kind(n) == NodeKind::FunctionDecl) fn = n;
            else if (tree->kind(n) == NodeKind::ClassDecl && result.refs[n] != NoEntity)
            {
                uint32_t init = member_decl(result.refs[n], "init");
                if (init != NoNode && tree->kind(init) == NodeKind::FunctionDecl)
                {
                    fn = init;
                    constructorOf = chain[0];
                }
            }
        }
    }
    else
    {
        vector<string> receiver(chain.begin(), chain.end() - 1);
        Receiver r = resolve_chain(receiver, at);
        if (r.kind == Receiver::Class || r.kind == Receiver::Struct)
        {
            uint32_t m = member_decl(r.entity, chain.back());
            if (m != NoNode && tree->kind(m) == NodeKind::FunctionDecl) fn = m;
        }
    }
    if (fn == NoNode) return false;
    out.label = signature_of(fn, &out.params);
    if (!constructorOf.empty()) out.label = constructorOf + out.label.substr(8);  // "def init(...)" -> "Point(...)"
    out.active = commas;
    return true;
}
