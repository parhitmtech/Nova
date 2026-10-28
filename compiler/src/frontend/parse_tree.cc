// The parse tree's navigation and its printed form (--dump-tree). The parser that builds it is parse.cc.
#include <algorithm>
#include "parse_tree.h"

using namespace std;

// Each kind's name; whether its token says something (a name, a literal, an operator), so the printed form shows
// it; and whether it starts its own line there (definitions and statements).
static const struct { const char* name; bool spelled; bool ownLine; } kinds[] = {
    {"File", false, true},          {"FunctionDecl", true, true},  {"ParamList", false, false},
    {"Param", true, false},        {"ReturnTypes", false, false}, {"StructDecl", true, true},
    {"ClassDecl", true, true},      {"TypeParams", false, false},  {"TypeParam", true, false},
    {"Extends", false, false},      {"FieldDecl", true, true},     {"Ignite", false, true},
    {"TypeName", true, false},      {"GenericType", true, false},
    {"Block", false, true},         {"VarDecl", true, true},        {"MultiDecl", false, true},
    {"ExprStatement", false, true}, {"Assign", false, true},        {"CompoundAssign", true, true},
    {"If", false, true},            {"Elif", false, true},          {"Else", false, true},
    {"While", false, true},         {"DoWhile", false, true},       {"For", false, true},
    {"Switch", false, true},        {"Case", false, true},          {"Default", false, true},
    {"Return", false, true},        {"Break", false, true},         {"Continue", false, true},
    {"Pass", false, true},          {"Empty", false, false},
    {"IntLiteral", true, false},    {"FloatLiteral", true, false},  {"StringLiteral", true, false},
    {"BoolLiteral", true, false},   {"Name", true, false},          {"Self", false, false},
    {"Paren", false, false},        {"Unary", true, false},         {"Binary", true, false},
    {"Call", false, false},         {"Index", false, false},        {"Member", true, false},
};
static_assert(sizeof(kinds) / sizeof(kinds[0]) == (size_t)NodeKind::Member + 1, "kinds must list every NodeKind");

string_view ParseTree::spelling(uint32_t n) const 
{
    const LexToken& t = token(n);
    return string_view(sourceManager.text_at(t.loc), t.length);
}

// The last child is the node just before n; each earlier one ends just before the subtree of the one after it.
vector<uint32_t> ParseTree::children(uint32_t n) const 
{
    vector<uint32_t> out;
    uint32_t first = n + 1 - nodes[n].size;  // where n's subtree starts
    for (uint32_t c = n; c > first; c -= nodes[c - 1].size) out.push_back(c - 1);
    reverse(out.begin(), out.end());
    return out;
}

static void print_node(const ParseTree& tree, uint32_t n, int depth, ostream& out)
{
    const auto& k = kinds[(int)tree.kind(n)];
    out << '(' << k.name;
    if (k.spelled) out << ' ' << tree.spelling(n);
    for (uint32_t c : tree.children(n))
    {
        if (kinds[(int)tree.kind(c)].ownLine) out << '\n' << string(2 * (depth + 1), ' ');
        else out << ' ';
        print_node(tree, c, depth + 1, out);
    }
    out << ')';
}

//  (FunctionDecl add (ParamList (Param a (TypeName int)) (Param b (TypeName int))) (ReturnTypes (TypeName int))
//    (Block
//      (Return (Binary + (Name a) (Name b)))))
void ParseTree::print(ostream& out) const
{
    print_node(*this, root(), 0, out);
    out << '\n';
}