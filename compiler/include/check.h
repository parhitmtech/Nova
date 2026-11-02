#ifndef NOVA_CHECK_H
#define NOVA_CHECK_H

// The check pass. It walks the parse tree (parse_tree.h) in two steps. The first (check.cc) works out what every
// name refers to: a variable, a function, a struct, a class, a type parameter or a built-in. The second
// (check_types.cc) works out the type of every expression and checks the type rules. Its result is a table of
// entities, a table of types and, for each node, the entity it names and its type; the passes after it read the
// program through these, never by name.
//
// Every definition in the file is known before any body is checked, so a function can call one defined after it.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "parse_tree.h"

using TypeId= uint32_t;

enum class TypeKind : uint8_t
{
    Unknown,  // not known here: an untyped parameter, a void call, or a value the check pass does not type yet
    Int,
    Float,
    Double,
    Bool,
    String,
    Tensor,
    Array,      // array(n): ints
    None,       // the type of none, which goes wherever an object does
    Vector,
    Map,
    Struct,
    Class,
};

struct Type 
{
    TypeKind kind;
    uint32_t a = 0;  // Vector: the element type; Map: the key type; Struct / Class: its entity
    uint32_t b = 0;  // Map: the element type; Class: a generic class's copy, its index in instances + 1
};

// Every type, once: two types are the same exactly when their ids are.
class TypeTable
{
    public:
        static constexpr TypeId Unknown = 0, Int = 1, Float = 2, Double = 3, Bool = 4, String = 5, Tensor = 6,
                                Array = 7, None = 8;
        
        TypeTable();  // the built-in types, in the order of the ids above
        TypeId intern(Type t);
        const Type& operator[](TypeId id) const { return types[id]; }

    private:
        std::vector<Type> types;
        std::unordered_map<uint64_t, TypeId> ids;
};

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
    TypeId type = TypeTable::Unknown;  // a variable's type
};

// A function's parameter and return types. A function without '-> types' gets its return type from its first
// 'return'; one with an untyped parameter is generic, and each call has its own copy.
struct Signature
{
    std::vector<TypeId> params;
    std::vector<TypeId> returns;  // empty: nothing returned (yet)
    bool generic = false;
    bool inferred = false;        // no '-> types' was written
};

// A generic function's or class's copy: the function with these argument types (scale<int, int>), or the class
// with these type arguments (Box<int>). Each copy's body is checked with its own types.
struct Instance
{
    uint32_t decl;              // the FunctionDecl or ClassDecl
    std::vector<TypeId> args;
    std::unordered_map<uint32_t, Signature> signatures;  // by FunctionDecl: the function's, or the class's methods
};

constexpr uint32_t NoInstance = UINT32_MAX;

struct CheckResult
{
    std::vector<Entity> entities;
    TypeTable types;
    std::unordered_map<uint32_t, Signature> signatures;  // by FunctionDecl node; a copy's are in its Instance
    std::deque<Instance> instances;  // a deque: checking a copy can make others, and references must stay valid
    std::vector<uint32_t> refs;     // by node: the entity a name refers to or a declaration declares; else NoEntity
    std::vector<TypeId> nodeTypes;  // by node: an expression's type
    std::vector<std::string> errors;  // "Error at line N: ...", in the order they were found
};

CheckResult check_tree(const ParseTree& tree);

void check_types(const ParseTree& tree, CheckResult& out);      // the second step (check_types.cc)
std::string type_name(const CheckResult& checked, TypeId type);  // as messages write it: int, vector<int>, Point

#endif // NOVA_CHECK_H