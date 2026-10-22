// Built-in containers: vector<T> and map<K, V>. A container variable holds a handle into the runtime's
// container_heap (src/runtime/interp_containers.cc); containerTable keeps its element types, so every
// v[i], m[k] and method call is type-checked here and compiled to a TENSOR_CALL with a CON_* op.
#include "parser_internal.h"

static const char* ELEMENT_TYPES = "int, float, double, bool, string or Tensor";

bool at_container_type()
{
    return (token.lexeme == "vector" || token.lexeme == "map") && lexer.peek(1).token_type == LESS;
}

// vector<T> / map<K, V> with token on 'vector' / 'map': reads the type arguments into info. Token ends on '>'.
bool parse_container_type(ContainerInfo& info)
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

string container_type_name(const ContainerInfo& info)
{
    return info.isMap ? "map<" + typeToString(info.key) + ", " + typeToString(info.elem) + ">"
                      : "vector<" + typeToString(info.elem) + ">";
}

// The algorithm methods need ordered (or at least comparable) elements: everything but a Tensor, and for
// sum() a number. Reports and returns false when info's element type does not allow the method.
static bool element_type_allows(const ContainerInfo& info, const string& name, const string& method, int line)
{
    bool ok = (method == "sum") ? (info.elem == TYPE_INT || info.elem == TYPE_FLOAT || info.elem == TYPE_DOUBLE) 
                                : (info.elem != TYPE_TENSOR);
    if (!ok)
    {
        report_error(line, "'" + name + "." + method + "()' needs " + (method == "sum" ? "numbers" : "comparable elements") + 
                           ", but '" + name + "' holds " + typeToString(info.elem));
    }
    return ok;
}

static int int_constant_slot(int v)
{
    int s = alloc_slot();
    mem[s] = v;
    return s;
}

bool same_container(VarType t, const ContainerInfo& c, const ContainerInfo& want)
{
    return t == (want.isMap ? TYPE_MAP : TYPE_VECTOR) && container_type_name(c) == container_type_name(want);
}

static int emit_container_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, VarType resultType,
                             int line, struct InstructionNode*& head, struct InstructionNode*& tracker);

// A new, empty container of this type: its handle. Sets lastExprType / lastExprContainer.
int emit_container_new(const ContainerInfo& info, int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    vector<int> args;
    if (info.isMap) args.push_back(int_constant_slot(info.key));
    args.push_back(int_constant_slot(info.elem));
    vector<VarType> types(args.size(), TYPE_INT);
    int handle = emit_container_op(info.isMap ? CON_MAP_NEW : CON_VEC_NEW, args, types, TYPE_INT, line, head, tracker);
    lastExprType = info.isMap ? TYPE_MAP : TYPE_VECTOR;
    lastExprContainer = info;
    return handle;
}

int parse_container_constructor(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int line = token.line_no;
    ContainerInfo info;
    if (!parse_container_type(info) || lexer.peek(1).token_type != LPAREN || lexer.peek(2).token_type != RPAREN)
    {
        if (info.elem != TYPE_UNKNOWN) report_error(line, "expected " + container_type_name(info) + "()");
        token = lexer.GetToken();
        lastExprType = TYPE_UNKNOWN;
        return 0;
    }
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // ')'
    token = lexer.GetToken();  // the token after the expression
    return emit_container_new(info, line, head, tracker);
}

// The right side of `vector<int> v = ...` or `v = ...`, with token on its first token: an expression giving a 
// container of exactly this type (vector<int>(), another vector<int>, a function returning one). The two
// variables then share one container. Returns the handle's slot, or -1 after an error.
static int parse_container_value(const ContainerInfo& info, const string& name, int line,
                                 struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int slot = parse_expression(head, tracker);
    if (lastExprType == TYPE_UNKNOWN) return -1;  // already reported
    if (!same_container(lastExprType, lastExprContainer, info))
    {
        string got = (lastExprType == TYPE_VECTOR || lastExprType == TYPE_MAP) ? container_type_name(lastExprContainer)
                                                                               : typeToString(lastExprType);
        report_error(line, "'" + name + "' is a " + container_type_name(info) + ", not " + got);
        return -1;
    }
    return slot;
}

static void emit_copy(int dst, int src, int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = dst;
    node->assign_inst.operand1_index = src;
    node->assign_inst.op = OPERATOR_NONE;
    append(head, tracker, node);
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
    int handle;
    if (token.token_type == EQUAL)
    {
        token = lexer.GetToken();  // first token of the value
        handle = parse_container_value(info, name, line, head, tracker);
        if (handle < 0)
        {
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
    }
    else
    {
        handle = emit_container_new(info, line, head, tracker);  // vector<int> v: empty
    }
    symbolTable[name] = alloc_slot();
    typeTable[name] = info.isMap ? TYPE_MAP : TYPE_VECTOR;
    containerTable[name] = info;
    emit_copy(symbolTable[name], handle, line, head, tracker);
}

// [i] / [k] or .method(...) on a container value (a variable, a field, a call's result), with token on the '['
// or '.'. `name` is what error messages call it. Token ends on the token after it.
int parse_container_postfix(int handle, const ContainerInfo& info, const string& name, int line, 
                               struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    bool dmemElem = (info.elem == TYPE_FLOAT || info.elem == TYPE_DOUBLE);
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
        if ((method == "contains" || method == "index_of" || method == "binary_search") && !info.isMap && 
            token.token_type != RPAREN)
        {
            VarType valueType;
            int value = parse_element_value(info, name, line, valueType, head, tracker);
            token = lexer.GetToken();  // past ')'
            if (!element_type_allows(info, name, method, line)) { lastExprType = TYPE_UNKNOWN; return 0; }
            TensorOp op = (method == "contains") ? CON_CONTAINS : (method == "index_of") ? CON_INDEX_OF : CON_BSEARCH;
            int r = emit_container_op(op, {handle, value}, {TYPE_INT, valueType}, TYPE_INT, line, head, tracker);
            if (method == "contains") lastExprType = TYPE_BOOL;
            return r;
        }
        if ((method == "min" || method == "max" || method == "sum") && !info.isMap && token.token_type == RPAREN)
        {
            token = lexer.GetToken();  // past ')'
            if (!element_type_allows(info, name, method, line)) { lastExprType = TYPE_UNKNOWN; return 0; }
            TensorOp op = (method == "min") ? (dmemElem ? CON_MIN_D : CON_MIN)
                        : (method == "max") ? (dmemElem ? CON_MAX_D : CON_MAX)
                                            : (dmemElem ? CON_SUM_D : CON_SUM);
            return emit_container_op(op, {handle}, {TYPE_INT}, info.elem, line, head, tracker);

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
                           (info.isMap ? string("map has m[k], has(k), size()") : string("vector has v[i], size(), pop(), contains(x), index_of(x), binary_search(x), min(), max(), sum()")));
        while (token.token_type != RPAREN && token.token_type != END_OF_FILE) token = lexer.GetToken();
        token = lexer.GetToken();
        lastExprType = TYPE_UNKNOWN;
        return 0;
    }
    lastExprType = TYPE_UNKNOWN;
    return 0;
}

// v[i], m[k], v.size(), v.pop(), m.has(k), or the container itself in an expression, with token on its name.
// Token ends on the token after it.
int parse_container_expression(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string name = token.lexeme;
    int line = token.line_no;
    ContainerInfo info = containerTable[name];
    int handle = symbolTable[name];
    token = lexer.GetToken();
    if (token.token_type == LBRAC || token.token_type == DOT)
    {
        return parse_container_postfix(handle, info, name, line, head, tracker);
    }
    // The container itself, as a value: passed, returned or assigned (the handle; both sides share it)
    lastExprType = info.isMap ? TYPE_MAP : TYPE_VECTOR;
    lastExprContainer = info;
    return handle;
}

void parse_container_statement_at(int handle, const ContainerInfo& info, const string& name, int line, 
                                  struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == LBRAC)
    {
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
    if (token.token_type == DOT)
    {
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
        else if ((method == "sort" || method == "reverse") && !info.isMap && token.token_type == RPAREN)
        {
            if (method == "reverse" || element_type_allows(info, name, method, line))
            {
                emit_container_op(method == "sort" ? CON_SORT : CON_REVERSE, {handle}, {TYPE_INT}, TYPE_UNKNOWN, line, head, tracker);
            }
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
                               (info.isMap ? string("map has m[k] = x, remove(k), clear()") : string("vector has v[i] = x, push(x), pop(), sort(), reverse(), clear()")));
            while (token.token_type != RPAREN && token.token_type != END_OF_FILE) token = lexer.GetToken();
        }
        token = lexer.GetToken();  // past ')' -> ';'
        return;
    }
    if (token.token_type == EQUAL)
    {
        // v = w: v now names the same container as w
        token = lexer.GetToken();  // first token of the value
        int value = parse_container_value(info, name, line, head, tracker);
        if (value >= 0) emit_copy(handle, value, line, head, tracker);
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }
    report_error(line, "expected '[', '.' or '=' after '" + name + "'");
    while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
}

//v[i] = x, m[k] = x, v.push(x), v.pop(), v.clear(), m.remove(k), v = w as a statement, with token on the 
// container's name. Token ends on ';'.
void parse_container_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string name = token.lexeme;
    int line = token.line_no;
    ContainerInfo info = containerTable[name];
    int handle = symbolTable[name];
    token = lexer.GetToken();  // '[', '.' or '='
    parse_container_statement_at(handle, info, name, line, head, tracker);
}