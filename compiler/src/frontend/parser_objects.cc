// Class instances as references. A class variable holds a handle into the runtime's object heap
// (src/runtime/interp_objects.cc); every field read or write is an OBJ_GET / OBJ_SET on that handle, compiled to
// a TENSOR_CALL like the container ops. A method receives its object's handle as a hidden first parameter
// (currentSelfHandle while the method is compiled), so a method call copies nothing in or out.
//
// An object is reference-counted by the runtime. A store into a variable (OBJ_STORE, or OBJ_MOVE for a temporary)
// counts the variable's reference; a temporary (a new object, a call's object result) is one reference that a
// store takes, or OBJ_RELEASE drops when nothing does (an argument after its call, a discarded result).
#include "parser_internal.h"

int currentSelfHandle = -1;
map<string, map<string, int>> classMethodSelfHandle;

string lastExprClass;        // the class of the last expression, when it is an object (lastExprType TYPE_CLASS)
bool lastExprTemp = false;   // ... and whether it is a temporary: a new object or a call's result
string currentFuncRetClass;  // the class the function being compiled returns, when it returns an object
map<string, vector<string>> functionParamClasses;                  // function -> each parameter's class ("" if none)
map<string, string> functionReturnClass;
map<string, map<string, vector<string>>> classMethodParamClasses;  // class -> method -> each parameter's class
map<string, map<string, string>> classMethodReturnClass;

// With token on a type's first token: whether it is a class (Point p) or a generic class (Box<int> b, compiled
// here). If so, cls is its name and token is on its last token (the name, or the closing '>').
bool parse_class_type_name(string& cls)
{
    if (token.token_type != ID) return false;
    if (classTable.count(token.lexeme))
    {
        cls = token.lexeme;
        return true;
    }
    if (is_generic_class(token.lexeme) && lexer.peek(1).token_type == LESS)
    {
        cls = parse_generic_class_type();
        return !cls.empty();
    }
    return false;
}

// The type of a value as a message names it: its class, its container type, or its built-in type.
string value_type_name(VarType t, const ContainerInfo& container, const string& cls)
{
    if (t == TYPE_CLASS) return cls;
    if (t == TYPE_VECTOR || t == TYPE_MAP) return container_type_name(container);
    return typeToString(t);
}

// The field of class `cls` named `field` (dotted for a nested class's flattened fields), or nullptr. Its
// position in the class's field list is the index the object heap uses (a parent's fields come first).
const ClassFieldInfo* class_field(const string& cls, const string& field, int* index)
{
    if (!classTable.count(cls)) return nullptr;
    const vector<ClassFieldInfo>& fields = classTable[cls].fields;
    for (int i = 0; i < (int)fields.size(); i++)
    {
        if (fields[i].name != field) continue;
        if (index) *index = i;
        return &fields[i];
    }
    return nullptr;
}

static int int_constant(int v)
{
    int s = alloc_slot();
    mem[s] = v;
    return s;
}

// An object op as a TENSOR_CALL. resultType says where the result goes (TYPE_UNKNOWN: none): a float or double
// comes back in dmem (OBJ_GET_D; a float is then narrowed), everything else in mem. A Tensor read from a field is
// a new owner of its handle, so it is tagged for the tensor-free pass.
static int emit_object_op(TensorOp op, const vector<int>& args, const vector<VarType>& types, VarType resultType,
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

static void emit_set(int handle, int index, int value, VarType valueType, int line,
                     struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    emit_object_op(OBJ_SET, {handle, int_constant(index), value}, {TYPE_INT, TYPE_INT, valueType}, TYPE_UNKNOWN, line,
                   head, tracker);
}

int emit_object_new(const string& cls, int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    const vector<ClassFieldInfo>& fields = classTable[cls].fields;
    int handle = emit_object_op(OBJ_NEW, {int_constant((int)fields.size())}, {TYPE_INT}, TYPE_INT, line, head, tracker);
    for (int i = 0; i < (int)fields.size(); i++)
    {
        const ClassFieldInfo& f = fields[i];
        if (f.array_size > 0)
        {
            // int data[256]: each object gets its own block of slots; the field holds where it starts
            int base = alloc_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = line;
            node->type = ALLOC;
            node->alloc_inst.base_slot = base;
            node->alloc_inst.size_slot = int_constant(f.array_size);
            append(head, tracker, node);
            emit_set(handle, i, base, TYPE_INT, line, head, tracker);
        }
        else if (f.type == TYPE_VECTOR || f.type == TYPE_MAP)
        {
            int container = emit_container_new(classFieldContainers[cls][f.name], line, head, tracker);
            emit_set(handle, i, container, TYPE_INT, line, head, tracker);
        }
        else if (f.type == TYPE_TENSOR)
        {
            emit_set(handle, i, int_constant(-1), TYPE_INT, line, head, tracker);  // unset: a clear error if used
        }
    }
    lastExprType = TYPE_CLASS;
    return handle;
}

// An object op that leaves lastExpr* alone (it runs after a value has been parsed)
static void emit_object_effect(TensorOp op, const vector<int>& args, int result, int line,
                               struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = TENSOR_CALL;
    node->tensor_call_inst.op = op;
    node->tensor_call_inst.result_slot = result;
    node->tensor_call_inst.num_args = (int)args.size();
    for (int i = 0; i < (int)args.size(); i++)
    {
        node->tensor_call_inst.arg_slots[i] = args[i];
        node->tensor_call_inst.arg_types[i] = TYPE_INT;
    }
    append(head, tracker, node);
}

// slot = the object in value: the variable now refers to it (a temporary's reference becomes the variable's), and
// the object the variable referred to before loses that reference.
void emit_object_store(int slot, int value, bool temp, int line, struct InstructionNode*& head,
                       struct InstructionNode*& tracker)
{
    emit_object_effect(temp ? OBJ_MOVE : OBJ_STORE, {slot, value}, slot, line, head, tracker);
}

void release_object_temps(const vector<int>& temps, int line, struct InstructionNode*& head,
                          struct InstructionNode*& tracker)
{
    for (int t : temps) emit_object_effect(OBJ_RELEASE, {t}, -1, line, head, tracker);
}

// With token on the first argument (or ')'): a call's arguments, parsed up to and past the ')'.
CallArgs parse_call_arguments(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    CallArgs a;
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
    {
        a.slots.push_back(parse_expression(head, tracker));
        a.types.push_back(lastExprType);
        a.containers.push_back((lastExprType == TYPE_VECTOR || lastExprType == TYPE_MAP) ? lastExprContainer : ContainerInfo());
        a.classes.push_back(lastExprType == TYPE_CLASS ? lastExprClass : "");
        if (lastExprType == TYPE_CLASS && lastExprTemp) a.temps.push_back(a.slots.back());
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'
    return a;
}

// An object argument goes to a parameter of its class (the class itself: a subclass is not accepted yet), and an
// object parameter takes only an object. Other arguments are left to the caller's own checks.
void check_object_argument(const CallArgs& a, int i, VarType want, const string& wantClass, const string& callee, int line)
{
    if (a.types[i] != TYPE_CLASS && want != TYPE_CLASS) return;
    if (a.types[i] == TYPE_CLASS && want == TYPE_CLASS && a.classes[i] == wantClass) return;
    report_error(line, "argument " + to_string(i + 1) + " of " + callee + " expects " +
                       (want == TYPE_CLASS ? wantClass : typeToString(want)) + " but got " +
                       value_type_name(a.types[i], a.containers[i], a.classes[i]));
}

// With token on the '(' after a class name: a new object, its init run with the arguments. The value is a
// temporary of class cls.
int parse_constructor_expression(const string& cls, int line, struct InstructionNode*& head,
                                 struct InstructionNode*& tracker)
{
    int handle = emit_object_new(cls, line, head, tracker);
    token = lexer.GetToken();  // past '('
    CallArgs a = parse_call_arguments(head, tracker);
    if (!classMethodTable.count(cls) || !classMethodTable[cls].count("init"))
    {
        report_error(line, "class '" + cls + "' has no constructor 'init'");
    }
    else
    {
        vector<string>& params = classMethodParams[cls]["init"];
        if (a.slots.size() != params.size())
            report_error(line, "'" + cls + "' constructor takes " + to_string(params.size()) + " argument(s) but got " +
                               to_string(a.slots.size()));
        vector<VarType>& paramTypes = classMethodParamTypes[cls]["init"];
        vector<ContainerInfo>& paramContainers = classMethodParamContainers[cls]["init"];
        vector<string>& paramClasses = classMethodParamClasses[cls]["init"];
        for (int i = 0; i < (int)a.types.size() && i < (int)paramTypes.size(); i++)
        {
            string pClass = i < (int)paramClasses.size() ? paramClasses[i] : "";
            if (!types_compatible(a.types[i], paramTypes[i]))
                report_error(line, "argument " + to_string(i + 1) + " of '" + cls + "' constructor expects " +
                                   value_type_name(paramTypes[i], ContainerInfo(), pClass) + " but got " +
                                   value_type_name(a.types[i], a.containers[i], a.classes[i]));
            else if ((paramTypes[i] == TYPE_VECTOR || paramTypes[i] == TYPE_MAP) && i < (int)paramContainers.size() &&
                     !same_container(a.types[i], a.containers[i], paramContainers[i]))
                report_error(line, "argument " + to_string(i + 1) + " of '" + cls + "' constructor expects " +
                                   container_type_name(paramContainers[i]) + " but got " + container_type_name(a.containers[i]));
            else
                check_object_argument(a, i, paramTypes[i], pClass, "'" + cls + "' constructor", line);
        }
        emit_method_call(cls, "init", handle, a.slots, a.types, line, head, tracker);
    }
    release_object_temps(a.temps, line, head, tracker);
    lastExprType = TYPE_CLASS;
    lastExprClass = cls;
    lastExprTemp = true;
    return handle;
}

// With token on the '=' of `p = value` or `Point p = value` (shown names the variable): the value must be an
// object of class cls, and slot then refers to it. Token ends on ';'.
void parse_object_value_into(int slot, const string& cls, const string& shown, int line,
                             struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken();  // first token of the value
    int value = parse_expression(head, tracker);
    if (lastExprType != TYPE_CLASS)
    {
        report_error(line, "a " + cls + " is created with " + cls + "(...), which runs its init");
        return;
    }
    if (lastExprClass != cls)
    {
        report_error(line, "'" + shown + "' is a " + cls + ", not a " + lastExprClass);
        return;
    }
    emit_object_store(slot, value, lastExprTemp, line, head, tracker);
}

int object_field_read(int handle, const string& cls, const string& field, int line,
                      struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int index = 0;
    const ClassFieldInfo* f = class_field(cls, field, &index);
    if (!f)
    {
        report_error(line, "class '" + cls + "' has no field '" + field + "'");
        lastExprType = TYPE_UNKNOWN;
        return 0;
    }
    VarType t = (f->array_size > 0) ? TYPE_INT : f->type;  // an array field reads as where its block starts
    bool inDmem = (t == TYPE_FLOAT || t == TYPE_DOUBLE);
    int r = emit_object_op(inDmem ? OBJ_GET_D : OBJ_GET, {handle, int_constant(index)}, {TYPE_INT, TYPE_INT}, t, line,
                           head, tracker);
    if (t == TYPE_VECTOR || t == TYPE_MAP) lastExprContainer = classFieldContainers[cls][field];
    return r;
}

void object_field_write(int handle, const string& cls, const string& field, int value, VarType valueType,
                        const ContainerInfo& valueContainer, const string& shown, int line,
                        struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int index = 0;
    const ClassFieldInfo* f = class_field(cls, field, &index);
    if (!f)
    {
        report_error(line, "class '" + cls + "' has no field '" + field + "'");
        return;
    }
    if (f->array_size > 0)
    {
        report_error(line, "'" + field + "' is an array field: assign its elements, " + field + "[i] = x");
        return;
    }
    if (f->type != TYPE_CLASS && !types_compatible(valueType, f->type))
    {
        report_error(line, "cannot assign " + typeToString(valueType) + " to " + typeToString(f->type) + " field '" +
                           field + "' of class '" + cls + "'");
        return;
    }
    if ((f->type == TYPE_VECTOR || f->type == TYPE_MAP) &&
        !same_container(valueType, valueContainer, classFieldContainers[cls][field]))
    {
        report_error(line, "'" + shown + "' is a " + container_type_name(classFieldContainers[cls][field]) + ", not " +
                           ((valueType == TYPE_VECTOR || valueType == TYPE_MAP) ? container_type_name(valueContainer)
                                                                                : typeToString(valueType)));
        return;
    }
    emit_set(handle, index, value, valueType, line, head, tracker);
}

// An element of an array field, base being the slot that holds where the field's block starts. With value < 0
// it is read (the result slot is returned), otherwise written.
static int array_field_access(int base, const ClassFieldInfo& f, int index, int value, int line,
                              struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int target = (value < 0) ? alloc_temp() : value;
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = (value < 0) ? ARRAY_READ : ARRAY_WRITE;
    node->array_inst.base_index = base;
    node->array_inst.dynamic_base = true;
    node->array_inst.index_slot = index;
    node->array_inst.target_index = target;
    node->array_inst.array_size = f.array_size;
    node->array_inst.size_slot = -1;
    node->array_inst.line_no = line;
    append(head, tracker, node);
    lastExprType = TYPE_INT;
    return target;
}

int parse_field_expression(int handle, const string& cls, const string& field, int line,
                           struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    const ClassFieldInfo* f = class_field(cls, field, nullptr);
    if (f && f->array_size > 0 && token.token_type == LBRAC)
    {
        // self.data[i]
        int base = object_field_read(handle, cls, field, line, head, tracker);
        token = lexer.GetToken();  // first token of the index
        int index = parse_expression(head, tracker);
        token = lexer.GetToken();  // past ']'
        return array_field_access(base, *f, index, -1, line, head, tracker);
    }
    return object_field_read(handle, cls, field, line, head, tracker);  // [i] / .x on it come from parse_factor
}

void parse_field_statement(int handle, const string& cls, const string& field, const string& shown, int line,
                           struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    const ClassFieldInfo* f = class_field(cls, field, nullptr);
    if (!f)
    {
        report_error(line, "class '" + cls + "' has no field '" + field + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }
    if (token.token_type == EQUAL)
    {
        // self.x = value (a container or Tensor field included: the field then names that value)
        token = lexer.GetToken();  // first token of the value
        int value = parse_expression(head, tracker);
        VarType valueType = lastExprType;
        ContainerInfo valueContainer = lastExprContainer;
        object_field_write(handle, cls, field, value, valueType, valueContainer, shown, line, head, tracker);
        return;  // token is on ';'
    }
    if (f->type == TYPE_VECTOR || f->type == TYPE_MAP)
    {
        // self.ids.push(x), self.ids[0] = x
        int container = object_field_read(handle, cls, field, line, head, tracker);
        parse_container_statement_at(container, classFieldContainers[cls][field], shown, line, head, tracker);
        return;
    }
    if (f->type == TYPE_TENSOR && token.token_type == LBRAC)
    {
        // self.W[i, j] = value
        int t = object_field_read(handle, cls, field, line, head, tracker);
        parse_tensor_element_store(t, shown, line, head, tracker);
        return;
    }
    if (f->array_size > 0 && token.token_type == LBRAC)
    {
        // self.data[i] = value
        int base = object_field_read(handle, cls, field, line, head, tracker);
        token = lexer.GetToken();  // first token of the index
        int index = parse_expression(head, tracker);
        token = lexer.GetToken();  // past ']' -> '='
        token = lexer.GetToken();  // past '=' -> the value
        int value = parse_expression(head, tracker);
        array_field_access(base, *f, index, value, line, head, tracker);
        return;
    }
    report_error(line, "expected '=' after '" + shown + "'");
    while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
}

int emit_method_call(const string& cls, const string& method, int objectSlot, const vector<int>& args,
                     const vector<VarType>& argTypes, int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string fullName = cls + "::" + method;
    VarType retType = (classMethodReturnType.count(cls) && classMethodReturnType[cls].count(method))
                      ? classMethodReturnType[cls][method] : TYPE_UNKNOWN;
    vector<int>& paramSlots = classMethodParamSlots[cls][method];
    vector<VarType>& paramTypes = classMethodParamTypes[cls][method];
    int n = 1 + (int)paramSlots.size();  // the object's handle is parameter 0

    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = line;
    callNode->type = CALL;
    callNode->call_inst.function_head = classMethodTable[cls][method];
    callNode->call_inst.ret_val_index = functionReturnIndex[fullName];
    callNode->call_inst.ret_type = retType;  // an object result is handed to the caller (see exec_ret)
    callNode->call_inst.func_slot_base = 0;
    callNode->call_inst.func_slot_count = 0;
    callNode->call_inst.num_params = n;
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    callNode->call_inst.ret_slot_types = nullptr;
    int* pSlots = new int[n];
    int* aSlots = new int[n];
    VarType* pTypes = new VarType[n];
    VarType* aTypes = new VarType[n];
    pSlots[0] = classMethodSelfHandle[cls][method];
    aSlots[0] = objectSlot;
    pTypes[0] = aTypes[0] = TYPE_INT;
    for (int i = 1; i < n; i++)
    {
        pSlots[i] = paramSlots[i - 1];
        aSlots[i] = (i - 1 < (int)args.size()) ? args[i - 1] : 0;
        pTypes[i] = (i - 1 < (int)paramTypes.size()) ? paramTypes[i - 1] : TYPE_UNKNOWN;
        aTypes[i] = (i - 1 < (int)argTypes.size()) ? argTypes[i - 1] : pTypes[i];
    }
    callNode->call_inst.param_slots = pSlots;
    callNode->call_inst.arg_val_slots = aSlots;
    callNode->call_inst.param_types = pTypes;
    callNode->call_inst.arg_types = aTypes;
    append(head, tracker, callNode);

    // The return value, copied out in its own slot space
    int fresh = (retType == TYPE_FLOAT) ? alloc_float_slot() : (retType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_temp();
    struct InstructionNode* copy = new InstructionNode();
    copy->line_no = line;
    if (retType == TYPE_FLOAT)
    {
        copy->type = ASSIGN_F;
        copy->assign_f_inst.left_hand_side_index = fresh;
        copy->assign_f_inst.operand1_index = functionReturnIndex[fullName];
        copy->assign_f_inst.op = OPERATOR_NONE;
    }
    else if (retType == TYPE_DOUBLE)
    {
        copy->type = ASSIGN_D;
        copy->assign_d_inst.left_hand_side_index = fresh;
        copy->assign_d_inst.operand1_index = functionReturnIndex[fullName];
        copy->assign_d_inst.op = OPERATOR_NONE;
    }
    else
    {
        copy->type = ASSIGN;
        copy->assign_inst.left_hand_side_index = fresh;
        copy->assign_inst.operand1_index = functionReturnIndex[fullName];
        copy->assign_inst.op = OPERATOR_NONE;
    }
    append(head, tracker, copy);
    lastExprType = retType;
    if (retType == TYPE_VECTOR || retType == TYPE_MAP) lastExprContainer = classMethodReturnContainer[cls][method];
    if (retType == TYPE_CLASS)
    {
        lastExprClass = classMethodReturnClass[cls][method];
        lastExprTemp = true;
    }
    return fresh;
}
