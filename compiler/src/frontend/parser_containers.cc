// Built-in containers: vector<T> and map<K, V>. A container variable holds a handle into the runtime's
// container_heap (src/runtime/interp_containers.cc); containerTable keeps its element types, so every
// v[i], m[k] and method call is type-checked here and compiled to a TENSOR_CALL with a CON_* op.
#include "parser_internal.h"

static const char* ELEMENT_TYPES = "int, float, double, bool, string or Tensor";

// vector<T> / map<K, V> with token on 'vector' / 'map': reads the type arguments into info. Token ends on '>'.
static bool parse_container_type(ContainerInfo& info)
{
    int line = token.line_no;
    info.isMap = (token.lexeme == "map");
    token = lexer.GetToken();  // '<'
    vector<Token> args;
    token = lexer.GetToken();
    while (token.token_type != GREATER && token.token_type != END_OF_FILE && token.token_type != SEMICOLON)
    {
        if (token.token_type != COMMA) args.push_back(token);
        token = lexer.GetToken();
    }
    size_t want = info.isMap ? 2 : 1;
    if (args.size() != want)
    {
        report_error(line, string(info.isMap ? "map takes 2 type arguments (map<K, V>)" : "vector takes 1 type argument (vector<T>)") +
                           " but got " + to_string(args.size()));
        return false;
    }
    info.elem = type_of_keyword(args.back().token_type);
    if (info.elem == TYPE_UNKNOWN)
    {
        report_error(line, "'" + args.back().lexeme + "' cannot be an element type: use " + ELEMENT_TYPES);
        return false;
    }
    if (info.isMap)
    {
        info.key = type_of_keyword(args[0].token_type);
        if (info.key != TYPE_INT && info.key != TYPE_STRING)
        {
            report_error(line, "a map key must be int or string, not '" + args[0].lexeme + "'");
            return false;
        }
    }
    return true;
}

static string container_type_name(const ContainerInfo& info)
{
    return info.isMap ? "map<" + typeToString(info.key) + ", " + typeToString(info.elem) + ">"
                      : "vector<" + typeToString(info.elem) + ">";
}

static int int_constant_slot(int v)
{
    int s = alloc_slot();
    mem[s] = v;
    return s;
}

// A container op as a TENSOR_CALL. resultType says where the result goes (TYPE_UNKNOWN: no result): a float or
// double comes back in dmem (a *_D op; a float is then narrowed), everything else in mem. A Tensor result is a
// new owner of its handle, so it is tagged for the tensor-free pass.
static int emit_container_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, VarType resultType,
                             int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    bool inDmem = (resultType == TYPE_FLOAT || resultType == TYPE_DOUBLE);
    int result = (resultType == TYPE_UNKNOWN) ? -1 : inDmem ? alloc_double_slot() : alloc_slot();
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = TENSOR_CALL;
    node->tensor_call_inst.op = op;
    node->tensor_call_inst.result_slot = result;
    node->tensor_call_inst.num_args = (int)args.size();
    for (int i = 0; i < (int)args.size(); i++)
    {
        node->tensor_call_inst.arg_slots[i] = args[i];
        node->tensor_call_inst.arg_types[i] = types[i];
    }
    append(head, tracker, node);
    if (resultType == TYPE_TENSOR) g_tensorSlots.insert(result);
    lastExprType = resultType;
    if (resultType == TYPE_FLOAT)
        return coerce_float_double(result, TYPE_DOUBLE, TYPE_FLOAT, head, tracker, line);
    return result >= 0 ? result : alloc_slot();
}

// An index (vector: int) or key (map: K) expression, with token on its first token. Token ends on ']' / ')'.
static int parse_index(const ContainerInfo& info, const string& name, int line,
                       struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int slot = parse_expression(head, tracker);
    VarType want = info.isMap ? info.key : TYPE_INT;
    if (!types_compatible(lastExprType, want))
        report_error(line, string(info.isMap ? "key" : "index") + " of '" + name + "' must be " + typeToString(want) +
                           ", got " + typeToString(lastExprType));
    return slot;
}

// A value to store, with token on its first token.
static int parse_element_value(const ContainerInfo& info, const string& name, int line, VarType& type,
                               struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int slot = parse_expression(head, tracker);
    type = lastExprType;
    if (!types_compatible(type, info.elem))
        report_error(line, "'" + name + "' holds " + typeToString(info.elem) + ", not " + typeToString(type));
    return slot;
}

// vector<int> v = vector<int>()   or   vector<int> v   (empty either way). Token starts on 'vector' / 'map'
// and ends on ';'.
void parse_container_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int line = token.line_no;
    ContainerInfo info;
    if (!parse_container_type(info))
    {
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }
    token = lexer.GetToken();  // variable name
    string name = token.lexeme;
    if (name_is_declared(name))
    {
        report_error(line, "redeclaration of '" + name + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }
    token = lexer.GetToken();  // '=' or ';'
    if (token.token_type == EQUAL)
    {
        // the constructor: the same type again, then ()
        token = lexer.GetToken();
        ContainerInfo rhs;
        bool ok = (token.lexeme == "vector" || token.lexeme == "map") && lexer.peek(1).token_type == LESS &&
                  parse_container_type(rhs) && container_type_name(rhs) == container_type_name(info) &&
                  lexer.peek(1).token_type == LPAREN && lexer.peek(2).token_type == RPAREN;
        if (!ok)
        {
            report_error(line, "'" + name + "' must start as " + container_type_name(info) + "()");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // ')'
        token = lexer.GetToken();  // ';'
    }

    vector<int> args;
    if (info.isMap) args.push_back(int_constant_slot(info.key));
    args.push_back(int_constant_slot(info.elem));
    vector<VarType> types(args.size(), TYPE_INT);
    int handle = emit_container_op(info.isMap ? CON_MAP_NEW : CON_VEC_NEW, args, types, TYPE_INT, line, head, tracker);

    symbolTable[name] = alloc_slot();
    typeTable[name] = info.isMap ? TYPE_MAP : TYPE_VECTOR;
    containerTable[name] = info;
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[name];
    node->assign_inst.operand1_index = handle;
    node->assign_inst.op = OPERATOR_NONE;
    append(head, tracker, node);
}

// v[i], m[k], v.size(), v.pop(), m.has(k) in an expression, with token on the container's name.
// Token ends on the token after it.
int parse_container_expression(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string name = token.lexeme;
    int line = token.line_no;
    ContainerInfo info = containerTable[name];
    int handle = symbolTable[name];
    bool dmemElem = (info.elem == TYPE_FLOAT || info.elem == TYPE_DOUBLE);
    token = lexer.GetToken();
    if (token.token_type == LBRAC)
    {
        token = lexer.GetToken();  // first token of the index
        int index = parse_index(info, name, line, head, tracker);
        VarType indexType = lastExprType;
        token = lexer.GetToken();  // past ']'
        return emit_container_op(dmemElem ? CON_GET_D : CON_GET, {handle, index}, {TYPE_INT, indexType}, info.elem,
                                 line, head, tracker);
    }
    if (token.token_type == DOT)
    {
        token = lexer.GetToken();  // method name
        string method = token.lexeme;
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // first argument or ')'
        if (method == "size" && token.token_type == RPAREN)
        {
            token = lexer.GetToken();
            return emit_container_op(CON_SIZE, {handle}, {TYPE_INT}, TYPE_INT, line, head, tracker);
        }
        if (method == "pop" && !info.isMap && token.token_type == RPAREN)
        {
            token = lexer.GetToken();
            return emit_container_op(dmemElem ? CON_POP_D : CON_POP, {handle}, {TYPE_INT}, info.elem, line, head, tracker);
        }
        if (method == "has" && info.isMap && token.token_type != RPAREN)
        {
            int key = parse_index(info, name, line, head, tracker);
            VarType keyType = lastExprType;
            token = lexer.GetToken();  // past ')'
            int r = emit_container_op(CON_HAS, {handle, key}, {TYPE_INT, keyType}, TYPE_INT, line, head, tracker);
            lastExprType = TYPE_BOOL;
            return r;
        }
        report_error(line, "'" + name + "." + method + "(...)' does not give a value here; a " +
                           (info.isMap ? string("map has m[k], has(k), size()") : string("vector has v[i], size(), pop()")));
        while (token.token_type != RPAREN && token.token_type != END_OF_FILE) token = lexer.GetToken();
        token = lexer.GetToken();
        lastExprType = TYPE_UNKNOWN;
        return 0;
    }
    report_error(line, "a " + container_type_name(info) + " cannot be used as a value yet (passed, returned or copied): "
                       "index it or call a method");
    lastExprType = TYPE_UNKNOWN;
    return 0;
}

// v[i] = x, m[k] = x, v.push(x), v.pop(), v.clear(), m.remove(k) as a statement, with token on the container's
// name. Token ends on ';'.
void parse_container_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string name = token.lexeme;
    int line = token.line_no;
    ContainerInfo info = containerTable[name];
    int handle = symbolTable[name];
    if (lexer.peek(1).token_type == LBRAC)
    {
        token = lexer.GetToken();  // '['
        token = lexer.GetToken();  // first token of the index
        int index = parse_index(info, name, line, head, tracker);
        VarType indexType = lastExprType;
        token = lexer.GetToken();  // past ']'
        if (token.token_type != EQUAL)
        {
            report_error(line, "expected '=' after '" + name + "[...]'");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // the value
        VarType valueType;
        int value = parse_element_value(info, name, line, valueType, head, tracker);
        emit_container_op(CON_SET, {handle, index, value}, {TYPE_INT, indexType, valueType}, TYPE_UNKNOWN, line, head, tracker);
        return;  // token is on ';'
    }
    if (lexer.peek(1).token_type == DOT)
    {
        token = lexer.GetToken();  // '.'
        token = lexer.GetToken();  // method name
        string method = token.lexeme;
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // first argument or ')'
        if (method == "push" && !info.isMap && token.token_type != RPAREN)
        {
            VarType valueType;
            int value = parse_element_value(info, name, line, valueType, head, tracker);
            emit_container_op(CON_PUSH, {handle, value}, {TYPE_INT, valueType}, TYPE_UNKNOWN, line, head, tracker);
        }
        else if (method == "remove" && info.isMap && token.token_type != RPAREN)
        {
            int key = parse_index(info, name, line, head, tracker);
            emit_container_op(CON_REMOVE, {handle, key}, {TYPE_INT, lastExprType}, TYPE_UNKNOWN, line, head, tracker);
        }
        else if (method == "clear" && token.token_type == RPAREN)
        {
            emit_container_op(CON_CLEAR, {handle}, {TYPE_INT}, TYPE_UNKNOWN, line, head, tracker);
        }
        else if (method == "pop" && !info.isMap && token.token_type == RPAREN)
        {
            bool dmemElem = (info.elem == TYPE_FLOAT || info.elem == TYPE_DOUBLE);
            emit_container_op(dmemElem ? CON_POP_D : CON_POP, {handle}, {TYPE_INT}, info.elem, line, head, tracker);
        }
        else
        {
            report_error(line, "'" + name + "." + method + "(...)' is not a " + (info.isMap ? "map" : "vector") + " method: a " +
                               (info.isMap ? string("map has m[k] = x, remove(k), clear()") : string("vector has v[i] = x, push(x), pop(), clear()")));
            while (token.token_type != RPAREN && token.token_type != END_OF_FILE) token = lexer.GetToken();
        }
        token = lexer.GetToken();  // past ')' -> ';'
        return;
    }
    report_error(line, "a " + container_type_name(info) + " cannot be assigned or copied yet: use " + name + "[...] = x");
    while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
}
