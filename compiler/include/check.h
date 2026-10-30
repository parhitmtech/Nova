#ifndef NOVA_CHECK_H
#define NOVA_CHECK_H
// The check pass. It walks the parse tree (parse_tree.h) and works out what every name refers to: a variable, a 
// function, a struct, a class, a type parameter or a built-in. Its result is a table of entities and, for each
// node that names one, which entity that is; the passes after it read the program through these, never by name.
//
// Every definition in the file is known before any body is checked, so a function can call one defined after it.

#include <cstdint>
#include <string>
#include <vector>
#include "parse_tree.h"

enum class EntityKind : uint8_t
{
    Variable,       // a local variable or a parameter; decl: its VarDecl or Param, or the Assign that first sets it
    Function,       // decl: FunctionDecl
    Struct,         // decl: StructDecl
    Class,          // decl: ClassDecl
    TypeParam,      // a generic class's type parameter; decl: TypeParam
    Builtin,        // a built-in function: zeros, matmul, print, input, array, ...
    BuiltinType,    // vector, map
    Module,         // hf, sol
    None,           // none: no object
};

constexpr uint32_t NoEntity = UINT32_MAX;
constexpr uint32_t NoNode = UINT32_MAX;

struct Entity
{
    EntityKind kind;
    Symbol name;
    uint32_t decl;  // the node that declares it; NoNode for a built-in
};

struct CheckResult
{
    std::vector<Entity> entities;
    std::vector<uint32_t> refs;     // by node: the entity a name refers to or a declaration declares; else NoEntity
    std::vector<std::string> errors;  // "Error at line N: ...", in the order they were found
};

CheckResult check_tree(const ParseTree& tree);

#endif // NOVA_CHECK_H