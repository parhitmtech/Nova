// Statements: declarations, print, assignment, conditions, if/while/do/for/switch. Split out of parser.cc.
#include "parser_internal.h"

// t[i, j, ...] = value with token on '[': store one element of the Tensor in slot t (one index per dimension). Shared 
// by a local (w[0, 1] = v), a self field (self.W[0, 1] = v) and an object field.
void parse_tensor_element_store(int t, const string& name, int line,
                                       struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    vector<int> args = {t};
    vector<VarType> types = {TYPE_TENSOR};
    token = lexer.GetToken();  // past '[' -> first index
    while (token.token_type != RBRAC && token.token_type != END_OF_FILE)
    {
        args.push_back(parse_expression(head, tracker));
        types.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ']'
    if (token.token_type != EQUAL)
    {
        report_error(line, "expected '=' after '" + name + "[...]'");
        return;
    }
    token = lexer.GetToken();  // all the value
    args.push_back(parse_expression(head, tracker));
    types.push_back(lastExprType);
    emit_tensor_op(TEN_SET, "element assignment", args, types, {}, line, head , tracker);
}

// The statements of one block, from its first element to its '}' (token ends on the '}'). The names it
// declares are gone after it.
static void parse_block_body(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    enter_block();
    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }
    exit_block();
}

void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    VarType declType = TYPE_UNKNOWN;
    if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) declType = type_of_keyword(token.token_type);

    token = lexer.GetToken();
    string name = token.lexeme;
    int nameLine = token.line_no;

    if (name_is_declared(name)) 
    {
        report_error(nameLine, "redeclaration of '" + name + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        return;
    }

    // A declaration without a value ("Tensor h", assigned later, e.g. inside a loop). Numbers start
    // at 0; a Tensor starts as the invalid handle -1, so using it before assigning one is a clear
    // runtime error instead of silently reading tensor 0.
    if (lexer.peek(1).token_type == SEMICOLON)
    {
        if (declType == TYPE_STRING)
            report_error(nameLine, "string '" + name + "' needs an initial value, e.g. string " + name + " = \"\"");
        if (declType == TYPE_FLOAT)       floatSymbolTable[name]  = alloc_float_slot();
        else if (declType == TYPE_DOUBLE) doubleSymbolTable[name] = alloc_double_slot();
        else                              symbolTable[name]       = alloc_slot();
        typeTable[name] = declType;
        // Set the starting value each time the declaration runs, not only once at compile time: a loop body
        // declares the variable again on every pass. Numbers start at 0, a Tensor at the invalid handle -1.
        struct InstructionNode* node = new InstructionNode();
        node->line_no = nameLine;
        if (declType == TYPE_FLOAT)
        {
            int zero = alloc_float_slot();
            fmem[zero] = 0.0f;
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = floatSymbolTable[name];
            node->assign_f_inst.operand1_index = zero;
            node->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (declType == TYPE_DOUBLE)
        {
            int zero = alloc_double_slot();
            dmem[zero] = 0.0;
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = doubleSymbolTable[name];
            node->assign_d_inst.operand1_index = zero;
            node->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            int start = alloc_slot();
            mem[start] = (declType == TYPE_TENSOR) ? -1 : 0;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[name];
            node->assign_inst.operand1_index = start;
            node->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, node);
        token = lexer.GetToken();  // ';'
        return;
    }

    if (declType == TYPE_FLOAT)
    {
        int fSlot = alloc_float_slot();
        floatSymbolTable[name] = fSlot;
        typeTable[name] = TYPE_FLOAT;
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to float variable '" + name + "'");
        }
        // emit ASSIGN_F
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_F;
        node->assign_f_inst.left_hand_side_index = fSlot;
        node->assign_f_inst.operand1_index = resultIdx;
        node->assign_f_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    if (declType == TYPE_DOUBLE)
    {
        int dSlot = alloc_double_slot();
        doubleSymbolTable[name] = dSlot;
        typeTable[name] = TYPE_DOUBLE;
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_DOUBLE && lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to double variable '" + name + "'");
        }
        // If RHS was a float literal, promote it to double via CAST
        if (lastExprType == TYPE_FLOAT)
        {
            struct InstructionNode* castNode = new InstructionNode();
            castNode->line_no = token.line_no;
            castNode->type = CAST;
            castNode->cast_inst.src_index = resultIdx;
            castNode->cast_inst.dst_index = dSlot;
            castNode->cast_inst.src_type = TYPE_FLOAT;
            castNode->cast_inst.dst_type = TYPE_DOUBLE;
            append(head, tracker, castNode);
            return;
        }
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_D;
        node->assign_d_inst.left_hand_side_index = dSlot;
        node->assign_d_inst.operand1_index = resultIdx;
        node->assign_d_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    symbolTable[name] = alloc_slot();
    typeTable[name] = declType;

    token = lexer.GetToken();  // '='
    token = lexer.GetToken();  // RHS

    if (declType == TYPE_STRING) 
    {
        if (token.token_type == STRING) 
        {
            // plain literal: string s = "hello" ;
            int strIdx = next_str_available++;
            strMem.push_back(token.lexeme);
            int constSlot = alloc_temp();
            mem[constSlot] = strIdx;
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[name];
            node->assign_inst.operand1_index       = constSlot;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            token = lexer.GetToken();  // ';'
        } 
        else 
        {
            // expression: string c = a + b ;
            int resultIdx = parse_expression(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
                report_error(nameLine, "cannot assign " + typeToString(lastExprType) + " to string variable '" + name + "'");
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[name];
            node->assign_inst.operand1_index       = resultIdx;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            // token is now ';'
        }
        return;
    }

    int resultIdx = parse_expression(head, tracker);
    VarType rhsType = lastExprType;
    if (!types_compatible(rhsType, declType))
    {
        report_error(nameLine, "cannot assign " + typeToString(rhsType) + " to " + typeToString(declType) + " variable '" + name + "'");
    }
    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[name];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
    // Propagate tensor-slot-ness through a plain copy: "Tensor x = someTensorVar" (a fresh tensor-call
    // result is already tagged by parse_tensor_call, so this one check covers both).
    if (g_tensorSlots.count(resultIdx)) g_tensorSlots.insert(symbolTable[name]);
}

void parse_multi_return_assignment(struct InstructionNode*& head, struct InstructionNode*&tracker)
{
    // ON ENTRY: token = first type keyword
    // Parses: int a, double d = func()
    vector<string> varNames;
    vector<VarType> varTypes;
    vector<int> varSlots;

    while (true)
    {
        VarType vt = TYPE_UNKNOWN;
        if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) vt = type_of_keyword(token.token_type);

        token = lexer.GetToken();  // variable name
        string vname = token.lexeme;

        int vs = (vt == TYPE_FLOAT) ? alloc_float_slot() : 
                 (vt == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
        if (vt == TYPE_FLOAT) floatSymbolTable[vname] = vs;
        else if (vt == TYPE_DOUBLE) doubleSymbolTable[vname] = vs;
        else symbolTable[vname] = vs;
        typeTable[vname] = vt;

        varNames.push_back(vname);
        varTypes.push_back(vt);
        varSlots.push_back(vs);

        token = lexer.GetToken(); // ',' or '='
        if (token.token_type != COMMA) break;
        token = lexer.GetToken();  // next type keyword
    }
    // token = '='
    token = lexer.GetToken();  // function name

    string funcName = token.lexeme;
    int callLine = token.line_no;

    if (is_generic_function(funcName))
    {
        report_error(callLine, "'" + funcName + "' has untyped parameters, so it cannot return several values: "
                               "give its parameters types");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return;
    }
    if (!functionTable.count(funcName))
    {
        report_error(callLine, "undeclared function '" + funcName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return;
    }

    // validate return count
    vector<int>& rSlots = functionReturnSlots[funcName];
    vector<VarType>& rTypes = functionReturnTypesList[funcName];
    if (rSlots.size() != varNames.size())
    {
        report_error(callLine, "function '" + funcName + "' returns " + to_string(rSlots.size()) + " value(s) but " + to_string(varNames.size()) + " variables given");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return;
    }

    // validate types
    for (int i = 0;i < (int)varTypes.size();i++)
    {
        if (rTypes[i] != TYPE_UNKNOWN && varTypes[i] != TYPE_UNKNOWN && rTypes[i] != varTypes[i])
        {
            report_error(callLine, "return value " + to_string(i+1) + " of '" + funcName + "': expected " + typeToString(rTypes[i]) + " but got " + typeToString(varTypes[i])); 
        }
    }

    // emit CALL
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first arg or ')'
    vector<int> argIndices;
    vector<VarType> argTypes;
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
    {
        int ai = parse_expression(head, tracker);
        argIndices.push_back(ai);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    vector<string> params = functionParams[funcName];
    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head = functionTable[funcName];
    // callNode->call_inst.ret_val_index = rSlots[0];
    // callNode->call_inst.all_ret_slots = nullptr;
    // callNode->call_inst.num_ret_slots = 0;
    // vector<int>& fRetSlots = functionReturnSlots[funcName];
    // int* allRets = new int[fRetSlots.size()];
    // for (int i = 0;i < (int)fRetSlots.size();i++)
    // {
    //     allRets[i] = fRetSlots[i];
    // }
    // callNode->call_inst.all_ret_slots = allRets;
    // callNode->call_inst.num_ret_slots = (int)fRetSlots.size();
    callNode->call_inst.ret_val_index = rSlots[0];
    callNode->call_inst.ret_type = rTypes[0];
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    callNode->call_inst.ret_slot_types = nullptr;
    vector<int>& fRetSlots = functionReturnSlots[funcName];
    vector<VarType>& fRetTypes = functionReturnTypesList[funcName];
    int* allRets = new int[fRetSlots.size()];
    VarType* allRetTypes = new VarType[fRetSlots.size()];  // one type per slot
    for (int i = 0;i < (int)fRetSlots.size();i++)
    {
        allRets[i] = fRetSlots[i];
        allRetTypes[i] = (i < (int)fRetTypes.size()) ? fRetTypes[i] : TYPE_UNKNOWN;
    }
    callNode->call_inst.all_ret_slots = allRets;
    callNode->call_inst.ret_slot_types = allRetTypes;
    callNode->call_inst.num_ret_slots = (int)fRetSlots.size();
    callNode->call_inst.func_slot_base = functionSlotRange.count(funcName) ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName) ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params = (int)params.size();
    callNode->call_inst.param_types = nullptr;
    if (!params.empty())
    {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = functionParamSlots[funcName];
        for (int i = 0;i < (int)params.size();i++)
        {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* ptypes = new VarType[params.size()];
        vector<VarType>& pTypes = functionParamTypes[funcName];
        for (int i = 0; i < (int)params.size(); i++)
            ptypes[i] = (i < (int)pTypes.size()) ? pTypes[i] : TYPE_UNKNOWN;
        VarType* atypes = new VarType[params.size()];
        for (int i = 0;i < (int)params.size();i++)
        {
            atypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;
        }
        callNode->call_inst.param_types = ptypes;
        callNode->call_inst.arg_types = atypes;
        callNode->call_inst.param_slots = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    }
    else
    {
        callNode->call_inst.param_slots = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
        callNode->call_inst.param_types = nullptr;
        callNode->call_inst.arg_types = nullptr;
    }
    append(head, tracker, callNode);

    // copy each return slot -> variable slot
    for (int i = 0;i < (int)varNames.size();i++)
    {
        struct InstructionNode* an = new InstructionNode();
        an->line_no = token.line_no;
        if (varTypes[i] == TYPE_FLOAT)
        {
            an->type = ASSIGN_F;
            an->assign_f_inst.left_hand_side_index = varSlots[i];
            an->assign_f_inst.operand1_index = rSlots[i];
            an->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (varTypes[i] == TYPE_DOUBLE)
        {
            an->type = ASSIGN_D;
            an->assign_d_inst.left_hand_side_index = varSlots[i];
            an->assign_d_inst.operand1_index = rSlots[i];
            an->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            an->type = ASSIGN;
            an->assign_inst.left_hand_side_index = varSlots[i];
            an->assign_inst.operand1_index = rSlots[i];
            an->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, an);
    }
    // token = ';'
}

static void parse_loop_jump(struct InstructionNode*& head, struct InstructionNode*& tracker);

void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) 
    {
        // multi-return: TYPE ID, TYPE ID = func() ;    
        if (lexer.peek(2).token_type == COMMA)
        {
            parse_multi_return_assignment(head, tracker);
        }
        else
        {
            parse_typed_declaration(head, tracker);
        }
    } 
    else if (token.token_type == ID && TENSOR_OP_MAP.count(token.lexeme) && TENSOR_VOID_OPS.count(TENSOR_OP_MAP.at(token.lexeme)))
    {
        // void tensor call as a statement: tensor_print(t) ; or tensor)set(t, i, j, v) ;
        string name = token.lexeme;
        token = lexer.GetToken();  // move past name to '('
        parse_tensor_call(name, head, tracker);
        // token is now ';' — outer loop will advance past it
    }
    else if (token.token_type == PRINT) 
    {
        parse_output_statement(head, tracker);
    } 
    else if (token.token_type == IF) 
    {
        parse_if_statement(head, tracker);
    } 
    else if (token.token_type == WHILE) 
    {
        parse_while_statement(head, tracker);
    } 
    else if (token.token_type == DO)
    {
        parse_do_while_statement(head, tracker);
    }
    else if (token.token_type == FOR) 
    {
        parse_for_statement(head, tracker);
    } 
    else if (token.token_type == SWITCH)
    {
        parse_switch_statement(head, tracker);
    }
    else if (token.token_type == BREAK || token.token_type == CONTINUE)
    {
        parse_loop_jump(head, tracker);
    }
    else if (token.token_type == RETURN) 
    {
        if (!insideFunction) 
        {
            report_error(token.line_no, "return statement outside of a function");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        } 
        else 
        {
            token = lexer.GetToken();
            vector<int> retExprs;
            vector<VarType> retExprTypes;
            retExprs.push_back(parse_expression(head, tracker));
            retExprTypes.push_back(lastExprType);
            ContainerInfo retContainer = lastExprContainer;  // when the first value is a container
            string retClass = (lastExprType == TYPE_CLASS) ? lastExprClass : "";  // ... or an object
            while (token.token_type == COMMA)
            {
                token = lexer.GetToken();
                retExprs.push_back(parse_expression(head, tracker));
                retExprTypes.push_back(lastExprType);
            }
            if (currentFuncReturnPending) infer_return_type(retExprTypes, retContainer, retClass, token.line_no);
            if (currentFuncRetType == TYPE_CLASS && retExprTypes.size() == 1 && retExprTypes[0] == TYPE_CLASS && 
                !class_fits(currentFuncRetClass, retClass))
            {
                report_error(token.line_no, "'" + currentFunctionLabel + "' returns " + currentFuncRetClass + ", not " + retClass);
            }
            if ((currentFuncRetType == TYPE_VECTOR || currentFuncRetType == TYPE_MAP) && retExprTypes.size() == 1 &&
                 retExprTypes[0] == currentFuncRetType && !same_container(retExprTypes[0], retContainer, currentFuncRetContainer))
            {
                report_error(token.line_no, "'" + currentFunctionLabel + "' returns " + container_type_name(currentFuncRetContainer) + 
                                            ", not " + container_type_name(retContainer));
            }
            int nExpected = currentFuncRetSlots.empty() ? 1 : (int)currentFuncRetSlots.size();
            if ((int)retExprs.size() != nExpected)
            {
                report_error(token.line_no, "function '" + currentFunctionLabel + "' returns " + to_string(nExpected) + 
                                            " value(s) but this return gives " + to_string(retExprs.size()));
            }
            for (int i = 0;i < (int)retExprs.size();i++)
            {
                int rSlot = (i < (int)currentFuncRetSlots.size()) ? currentFuncRetSlots[i] : currentFuncRetIdx;
                VarType rt = (i < (int)currentFuncRetTypes.size()) ? currentFuncRetTypes[i] : currentFuncRetType;
                check_return_type(retExprTypes[i], rt, token.line_no);
                struct InstructionNode* an = new InstructionNode();
                an->line_no = token.line_no;
                if (rt == TYPE_FLOAT)
                {
                    an->type = ASSIGN_F;
                    an->assign_f_inst.left_hand_side_index = rSlot;
                    an->assign_f_inst.operand1_index = retExprs[i];
                    an->assign_f_inst.op = OPERATOR_NONE;   
                }
                else if (rt == TYPE_DOUBLE)
                {
                    an->type = ASSIGN_D;
                    an->assign_d_inst.left_hand_side_index = rSlot;
                    an->assign_d_inst.operand1_index = retExprs[i];
                    an->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    an->type = ASSIGN;
                    an->assign_inst.left_hand_side_index = rSlot;
                    an->assign_inst.operand1_index = retExprs[i];
                    an->assign_inst.op = OPERATOR_NONE;
                    // rSlot is the function's one return slot, read by every caller: once it carries a tensor
                    // from any return, the tensor-free pass treats every caller's copy as a tensor
                    if (g_tensorSlots.count(retExprs[i])) g_tensorSlots.insert(rSlot);
                }
                append(head, tracker, an);
            }
            struct InstructionNode* retNode = new InstructionNode();
            retNode->line_no = token.line_no;
            retNode->type = RET;
            retNode->ret_inst.ret_val_index = currentFuncRetIdx;
            retNode->ret_inst.ret_type = currentFuncRetType;
            append(head, tracker, retNode);
        }
    } 
    else if (token.token_type == SELF)
    {
        // self.field = expr ; inside a class method
        if (!insideFunction || currentSelfClassName.empty())
        {
            report_error(token.line_no, "'self' used outside of a class method");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // past 'self' -> '.'
        token = lexer.GetToken();  // the field's or method's name
        string member = token.lexeme;
        int line = token.line_no;
        token = lexer.GetToken();
        if (token.token_type == LPAREN)
        {
            // self.reset(): a method on this object
            int result = parse_method_call_on(currentSelfHandle, currentSelfClassName, member, head, tracker);
            finish_call_statement(result, line, head, tracker);
        }
        else 
        {
            parse_field_statement(currentSelfHandle, currentSelfClassName, member, "self." + member, line, head, tracker);
        }
    }
    else if (token.token_type == ID)
    {
        string name = token.lexeme;
        if (name == "hf" && lexer.peek(1).token_type == DOT)
        {
            token = lexer.GetToken();  // consume "hf" → '.'
            token = lexer.GetToken();  // consume '.' → method name
            string method = token.lexeme;
            token = lexer.GetToken();  // consume method name → '('
            parse_hf_call(method, head, tracker);
        }
        else if (name == "sol" && lexer.peek(1).token_type == DOT)
        {
            token = lexer.GetToken();  // consume "sol" → '.'
            token = lexer.GetToken();  // consume '.' → method name
            string method = token.lexeme;
            token = lexer.GetToken();  // consume method name → '('
            parse_sol_call(method, head, tracker);
        }
        else if ((name == "vector" || name == "map") && lexer.peek(1).token_type == LESS)
        {
            parse_container_declaration(head, tracker);  // vector<int> v = vector<int>()
        }
        else if (containerTable.count(name))
        {
            parse_container_statement(head, tracker);  // v.push(x), v[i] = x, m.remove(k), ...
        }
        else if (is_generic_class(name))
        {
            // Box<int> b = Box<int>(5): compile the copy for these type arguments, then declare as for any class
            string copyName;
            if (lexer.peek(1).token_type != LESS)
            {
                report_error(token.line_no, "class '" + name + "' is generic: write its type arguments, e.g. " + name + "<int>");
            }
            else
            {
                copyName = parse_generic_class_type();
            }
            if (copyName.empty())
            {
                while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            }
            else
            {
                parse_class_instantiation(copyName, head, tracker);
            }
        }
        else if (classTable.count(name) && lexer.peek(1).token_type == ID)
        {
            // class instantiation: MyClass obj ; or MyClass obj = { ... } ;
            parse_class_instantiation(name, head, tracker);
        }
        else if (varClassType.count(name) && lexer.peek(1).token_type == EQUAL)
        {
            // p = q, p = Point(1, 2), p = make(): p refers to the value's object
            int line = token.line_no;
            token = lexer.GetToken();  // '='
            parse_object_value_into(symbolTable[name], varClassType[name], name, line, head, tracker);
        }
        else if (varClassType.count(name) && lexer.peek(1).token_type == DOT)
        {
            // obj.method(args) ; or obj.field = expr ;
            int objLine = token.line_no;
            token = lexer.GetToken();  // consume obj name → '.'
            token = lexer.GetToken();  // consume '.' → member name
            string memberName = token.lexeme;
            token = lexer.GetToken();  // consume member name → '(' or '='

            if (token.token_type == LPAREN)
            {
                int result = parse_method_call(name, memberName, head, tracker);
                finish_call_statement(result, objLine, head, tracker);  // t.add(a).add(b)
                // token is now ';'
            }
            else
            {
                parse_field_statement(symbolTable[name], varClassType[name], memberName, name + "." + memberName, objLine,
                                      head, tracker);
            }
        }
        else if (structTable.count(name) && lexer.peek(1).token_type == ID)
        {
            // struct instantiation: Point p ; or Point p = {3.14, 2.71} ;
            parse_struct_instantiation(name, head, tracker);
        }
        else if (arrayTable.find(name) != arrayTable.end())
        {
            parse_assignment_statement(head, tracker);
        }
        else if ((functionTable.find(name) != functionTable.end()) || functionParams.count(name) || is_generic_function(name))
        {
            // functionParams also covers a function calling itself as a statement while its own
            // body is being parsed
            int line = token.line_no;
            int result = parse_function_call(head, tracker);
            finish_call_statement(result, line, head, tracker);  // make().x = 1, or the result is unused
        }
        else if (lexer.peek(1).token_type == LPAREN)
        {
            // NOT a defined function or a void tensor op. This used to fall into parse_assignment,
            // while silently turned "name(args) ;" into a bogus variable assignment and emitted no call.
            if (TENSOR_OP_MAP.count(name))
            {
                report_error(token.line_no, "result of '" + name + "' is not used; assign it to a variable");
            }
            else 
            {
                report_error(token.line_no, "call to undefined function '" + name + "'");
            }
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
        }
        else
        {
            parse_assignment_statement(head, tracker);
        }
    }
    else if (token.token_type != RBRACE)
    {
        report_error(token.line_no, "unexpected token '" + token.lexeme + "'");
    }
}

void parse_output_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* newNode = new InstructionNode();
    newNode->line_no = token.line_no;
    newNode->type = OUT;
    newNode->output_inst.newline       = true;
    newNode->output_inst.is_string     = false;
    newNode->output_inst.is_string_var = false;
    newNode->output_inst.value_type = TYPE_INT;  // default

    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // content

    if (token.token_type == STRING) {
        newNode->output_inst.var_index = next_str_available++;
        newNode->output_inst.is_string = true;
        newNode->output_inst.is_string_var = false;
        newNode->output_inst.value_type = TYPE_STRING;
        strMem.push_back(token.lexeme);
        token = lexer.GetToken();
    } 
    else 
    {
        int resultIdx = parse_expression(head, tracker);
        VarType exprType = lastExprType;
        newNode->output_inst.var_index = resultIdx;
        newNode->output_inst.value_type = exprType;
        if (exprType == TYPE_STRING) 
        {
            newNode->output_inst.is_string     = true;
            newNode->output_inst.is_string_var = true;
        }
    }

    token = lexer.GetToken();  // ';'
    append(head, tracker, newNode);
}

void parse_assignment_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string lhs = token.lexeme;
    int lhs_line = token.line_no;

    // t[i, j, ...] = value; store one element of a Tensor (one index per dimension)
    if (typeTable.count(lhs) && typeTable[lhs] == TYPE_TENSOR && lexer.peek(1).token_type == LBRAC)
    {
        token = lexer.GetToken();  // '['
        parse_tensor_element_store(symbolTable[lhs], lhs, lhs_line, head, tracker);
        return;  // token is now ';'
    }

    // struct field write: p.x = 3.14 ; or L.a.x = 1.0 ;
    if (varStructType.count(lhs) && lexer.peek(1).token_type == DOT)
    {
        token = lexer.GetToken();  // consume var name -> DOT

        string fieldPath = "";
        while (token.token_type == DOT)
        {
            token = lexer.GetToken();  // consume DOT → field segment
            if (!fieldPath.empty()) fieldPath += ".";
            fieldPath += token.lexeme;
            token = lexer.GetToken();  // consume field segment → next token
        }

        // token = '='
        token = lexer.GetToken();  // consume '=' → RHS start

        int resultIdx = parse_expression(head, tracker);
        // token = ';'

        if (!structFieldSlots[lhs].count(fieldPath))
        {
            report_error(lhs_line, "struct '" + lhs + "' has no field '" + fieldPath + "'");
            return;
        }

        int slot = structFieldSlots[lhs][fieldPath];
        VarType fieldType = structFieldTypes[lhs][fieldPath];

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        if (fieldType == TYPE_FLOAT)
        {
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = slot;
            node->assign_f_inst.operand1_index = resultIdx;
            node->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fieldType == TYPE_DOUBLE)
        {
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = slot;
            node->assign_d_inst.operand1_index = resultIdx;
            node->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = slot;
            node->assign_inst.operand1_index = resultIdx;
            node->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, node);
        return;
    }

    if (arrayTable.find(lhs) != arrayTable.end()) {
        token = lexer.GetToken();  // '['
        token = lexer.GetToken();
        int indexLine = token.line_no;
        if (token.token_type == NUM && !(arrayDynamic.count(lhs) && arrayDynamic[lhs]))
        {
            check_bounds(lhs, stoi(token.lexeme), indexLine);
        }
        int indexSlot = parse_expression(head, tracker);
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();
        int valueSlot = parse_expression(head, tracker);
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ARRAY_WRITE;
        bool isDyn = arrayDynamic.count(lhs) && arrayDynamic[lhs];
        node->array_inst.base_index   = arrayTable[lhs];
        node->array_inst.dynamic_base = isDyn;
        node->array_inst.index_slot   = indexSlot;
        node->array_inst.target_index = valueSlot;
        node->array_inst.array_size   = isDyn ? -1 : arraySizeTable[lhs];
        node->array_inst.size_slot = isDyn ? arraySizeTable[lhs] : -1;
        node->array_inst.line_no = lhs_line;
        append(head, tracker, node);
        return;
    }

    if (symbolTable.find(lhs) == symbolTable.end() && lexer.peek(1).token_type == EQUAL && lexer.peek(2).token_type == ARRAY) {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // 'array'
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // first token of size expression

        if (token.token_type == NUM && lexer.peek(1).token_type == RPAREN) 
        {
            // ── static: array(5) ─────────────────────────────────────────────────
            int size = stoi(token.lexeme);
            if (size <= 0) { report_error(lhs_line, "array '" + lhs + "' must have size > 0"); return; }
            token = lexer.GetToken();  // ')'
            token = lexer.GetToken();  // ';'
            arrayTable[lhs]    = next_available;
            arraySizeTable[lhs] = size;
            arrayDynamic[lhs]   = false;
            for (int i = 0; i < size; i++) alloc_slot();
        } 
        else 
        {
            // ── dynamic: array(n) or array(n * 2) ────────────────────────────────
            int sizeSlot = parse_expression(head, tracker);
            // token is now ')'
            token = lexer.GetToken();  // ';'

            int baseSlot       = alloc_slot();  // holds base index at runtime
            int storedSizeSlot = alloc_slot();  // holds size at runtime for bounds checks

            // copy computed size into storedSizeSlot
            struct InstructionNode* copySize = new InstructionNode();
            copySize->line_no = token.line_no;
            copySize->type = ASSIGN;
            copySize->assign_inst.left_hand_side_index = storedSizeSlot;
            copySize->assign_inst.operand1_index       = sizeSlot;
            copySize->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, copySize);

            // emit ALLOC — fills baseSlot at runtime
            struct InstructionNode* allocNode = new InstructionNode();
            allocNode->line_no = token.line_no;
            allocNode->type = ALLOC;
            allocNode->alloc_inst.base_slot = baseSlot;
            allocNode->alloc_inst.size_slot = storedSizeSlot;
            append(head, tracker, allocNode);

            arrayTable[lhs]    = baseSlot;
            arraySizeTable[lhs] = storedSizeSlot;
            arrayDynamic[lhs]   = true;
        }
        return;
    }

    // Reassignment of an already-declared double/float vairable: foo = <expr> ;
    // (NOT a fresh `double foo = ...` declaration, which is handled elsewhere.)
    // Without this branch, lhs wasn't in symbolTable (it lives in doubleSymbolTable/floatSymbolTable instead),
    // so the generic fallbackbelow treated every such reassignment as a brand-new, previously-unseen
    // variable: it silently allocated a fresh int-space slot and retyped lhs as a TYPE_UNKNOWN (or whatever
    // the RHS happened to be), completely disconnected from the real dmem/fmem-backed variable.
    // Reads of the original variable kept seeing its last value, so this was a correctness bug, not just
    // a type-checker false positive.
    if (doubleSymbolTable.count(lhs) && typeTable.count(lhs) && typeTable[lhs] == TYPE_DOUBLE)
    {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS start
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_DOUBLE && lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(lhs_line, "cannot assign " + typeToString(lastExprType) + " to double variable '" 
            + lhs + "'");
        }
        int srcIdx = resultIdx;
        if (lastExprType == TYPE_FLOAT)
        {
            // Promote float RHS to double, same as the declaration path does.
            int tmp = alloc_double_slot();
            struct InstructionNode* castNode = new InstructionNode();
            castNode->line_no = token.line_no;
            castNode->type = CAST;
            castNode->cast_inst.src_index = resultIdx;
            castNode->cast_inst.dst_index = tmp;
            castNode->cast_inst.src_type = TYPE_FLOAT;
            castNode->cast_inst.dst_type = TYPE_DOUBLE;
            append(head, tracker, castNode);
            srcIdx = tmp;
        }
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_D;
        node->assign_d_inst.left_hand_side_index = doubleSymbolTable[lhs];
        node->assign_d_inst.operand1_index = srcIdx;
        node->assign_d_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    if (floatSymbolTable.count(lhs) && typeTable.count(lhs) && typeTable[lhs] == TYPE_FLOAT)
    {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS start
        int resultIdx = parse_expression(head, tracker);
        if (lastExprType != TYPE_FLOAT && lastExprType != TYPE_UNKNOWN)
        {
            report_error(lhs_line, "cannot assign " + typeToString(lastExprType) + " to float variable '" + lhs + "'");
        }
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN_F;
        node->assign_f_inst.left_hand_side_index = floatSymbolTable[lhs];
        node->assign_f_inst.operand1_index = resultIdx;
        node->assign_f_inst.op = OPERATOR_NONE;
        append(head, tracker, node);
        return;
    }

    if (symbolTable.count(lhs) && typeTable.count(lhs) && typeTable[lhs] == TYPE_STRING) {
        token = lexer.GetToken();  // '='
        token = lexer.GetToken();  // RHS start
        if (token.token_type == STRING) 
        {
            // plain literal: a = "hello" ;
            int strIdx = next_str_available++;
            strMem.push_back(token.lexeme);
            int constSlot = alloc_temp();
            mem[constSlot] = strIdx;
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[lhs];
            node->assign_inst.operand1_index       = constSlot;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            token = lexer.GetToken();  // ';'
        } 
        else 
        {
            // expression: a = b + c ;
            int resultIdx = parse_expression(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
                report_error(lhs_line, "cannot assign " + typeToString(lastExprType) + " to string variable '" + lhs + "'");
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ASSIGN;
            node->assign_inst.left_hand_side_index = symbolTable[lhs];
            node->assign_inst.operand1_index       = resultIdx;
            node->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, node);
            // token is now ';'
        }
        return;
    }

    if (symbolTable.find(lhs) == symbolTable.end()) {
        symbolTable[lhs] = alloc_slot(); typeTable[lhs] = TYPE_UNKNOWN;
    }

    token = lexer.GetToken(); token = lexer.GetToken();
    int resultIdx = parse_expression(head, tracker);
    VarType rhsType = lastExprType;
    VarType lhsType = typeTable.count(lhs) ? typeTable[lhs] : TYPE_UNKNOWN;
    // Infer type from RHS for auto-declared variables (no explicit type annotation)
    if (lhsType == TYPE_UNKNOWN && rhsType != TYPE_UNKNOWN)
        typeTable[lhs] = lhsType = rhsType;
    if (!types_compatible(rhsType, lhsType))
        report_error(lhs_line, "cannot assign " + typeToString(rhsType) + " to " + typeToString(lhsType) + " variable '" + lhs + "'");

    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
    // Propogate tensor-slot-ness through reassignment: "x = someTensorVar ;" 
    if (g_tensorSlots.count(resultIdx)) g_tensorSlots.insert(symbolTable[lhs]);
}

void parse_assignment_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string lhs = token.lexeme;
    token = lexer.GetToken(); token = lexer.GetToken();
    int resultIdx = parse_expression(head, tracker);
    struct InstructionNode* node = new InstructionNode();
    node->line_no = token.line_no;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = symbolTable[lhs];
    node->assign_inst.operand1_index       = resultIdx;
    node->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, node);
}

// A condition is any expression; it holds when its value is true (non-zero). Emits a test that
// falls through into the body when it holds and jumps to failTarget when it does not.
void parse_condition(struct InstructionNode*& head, struct InstructionNode*& tracker, struct InstructionNode* failTarget)
{
    int line = token.line_no;
    int v = parse_expression(head, tracker);
    if (branch_on_comparison(v, failTarget, tracker)) return;  // a lone comparison: jump on it directly
    emit_truth_test(v, lastExprType, true, failTarget, line, head, tracker);
}

void parse_if_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    noOpNode->line_no = token.line_no;
    struct InstructionNode* condFailNode = new InstructionNode(); condFailNode->type = NOOP;
    condFailNode->line_no = token.line_no;
    string keyword = token.lexeme;            // "if" or "elif"
    token = lexer.GetToken();
    expect_condition_lparen(keyword);
    parse_condition(head, tracker, condFailNode);
    expect_condition_rparen(keyword);         // token is now '{'
    token = lexer.GetToken();
    
    parse_block_body(head, tracker);

    struct InstructionNode* skipJmp = new InstructionNode();
    skipJmp->line_no = token.line_no;
    skipJmp->type = JMP; skipJmp->jmp_inst.target = noOpNode;
    append(head, tracker, skipJmp);
    append(head, tracker, condFailNode);
    tracker = condFailNode;

    Token peeked = lexer.peek(1);
    if (peeked.token_type == ELIF) 
    {
        token = lexer.GetToken();
        struct InstructionNode* elifHead = nullptr, *elifTracker = nullptr;
        parse_if_statement(elifHead, elifTracker);
        condFailNode->next = elifHead;
        if (elifTracker != nullptr) elifTracker->next = noOpNode;
        tracker = noOpNode;
    }
    else if (peeked.token_type == ELSE) 
    {
        token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
        parse_block_body(head, tracker);
        tracker->next = noOpNode; tracker = noOpNode;
    } 
    else 
    {
        condFailNode->next = noOpNode; tracker = noOpNode;
    }
}

// The loops enclosing the statement being parsed, innermost last: where 'break' jumps (just past
// the loop) and where 'continue' jumps (the next iteration: while's condition, for's update,
// do-while's condition). A 'switch' is not a loop: Nova's cases never fall through, so a 'break'
// inside a switch leaves the enclosing loop.
struct LoopTargets
{
    struct InstructionNode* breakTo;
    struct InstructionNode* continueTo;
};
static vector<LoopTargets> g_loops;

static vector<vector<LoopTargets>> g_loopsAside;

void set_loops_aside()
{
    g_loopsAside.push_back(g_loops);
    g_loops.clear();
}

void restore_loops()
{
    g_loops = g_loopsAside.back();
    g_loopsAside.pop_back();
}

// break / continue: one jump to the innermost loop's exit or next iteration.
static void parse_loop_jump(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    bool isBreak = (token.token_type == BREAK);
    if (g_loops.empty())
    {
        report_error(token.line_no, string("'") + (isBreak ? "break" : "continue") + "' is only allowed inside a loop");
    }
    else
    {
        struct InstructionNode* jmp = new InstructionNode();
        jmp->line_no = token.line_no;
        jmp->type = JMP;
        jmp->jmp_inst.target = isBreak ? g_loops.back().breakTo : g_loops.back().continueTo;
        append(head, tracker, jmp);
    }
    token = lexer.GetToken();  // the ';' ending the statement
}

void parse_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* noOpNode = new InstructionNode(); noOpNode->type = NOOP;
    noOpNode->line_no = token.line_no;
    struct InstructionNode* condStart = new InstructionNode(); condStart->type = NOOP;
    condStart->line_no = token.line_no;
    append(head, tracker, condStart);
    token = lexer.GetToken();
    expect_condition_lparen("while");
    parse_condition(head, tracker, noOpNode);
    expect_condition_rparen("while");         // token is now '{'
    token = lexer.GetToken();
    g_loops.push_back({noOpNode, condStart});
    parse_block_body(head, tracker);
    g_loops.pop_back();
    struct InstructionNode* jmpNode = new InstructionNode();
    jmpNode->line_no = token.line_no;
    jmpNode->type = JMP; jmpNode->jmp_inst.target = condStart;
    tracker->next = jmpNode; jmpNode->next = noOpNode; tracker = noOpNode;
}

void parse_do_while_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // already parsed the keyword 'do'
    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->line_no = token.line_no;
    noOpNode->type = NOOP;
    struct InstructionNode* bodyStart = new InstructionNode();
    bodyStart->line_no = token.line_no;
    bodyStart->type = NOOP;

    // Mark top of loop body
    append(head, tracker, bodyStart);

    token = lexer.GetToken();  // consume '{'
    token = lexer.GetToken();  // first token inside body

    // 'continue' goes to the condition test after the body
    struct InstructionNode* condLabel = new InstructionNode();
    condLabel->line_no = token.line_no;
    condLabel->type = NOOP;
    g_loops.push_back({noOpNode, condLabel});

    parse_block_body(head, tracker);

    g_loops.pop_back();
    append(head, tracker, condLabel);
    // token = '}'

    token = lexer.GetToken();  // must be WHILE
    if (token.token_type != WHILE)
        report_error(token.line_no, "expected 'while (...)' after the body of 'do'");
    token = lexer.GetToken();
    expect_condition_lparen("while");

    // Condition: if TRUE -> jump back to bodyStart, if FALSE -> fall to noOpNode
    parse_condition(head, tracker, noOpNode);
    expect_condition_rparen("while");

    // Unconditional jump back to body top (condition passed)
    struct InstructionNode* jmpBack = new InstructionNode();
    jmpBack->line_no = token.line_no;
    jmpBack->type = JMP;
    jmpBack->jmp_inst.target = bodyStart;
    append(head, tracker, jmpBack);
    append(head, tracker, noOpNode);
    tracker = noOpNode;
    // token is now ';' - outer loop advances past it
}

void parse_for_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken(); token = lexer.GetToken();
    if (token.token_type == ID) parse_assignment_statement(head, tracker);

    // The condition is any expression, evaluated again before every iteration (condStart).
    struct InstructionNode* condStart = new InstructionNode();
    condStart->line_no = token.line_no;
    condStart->type = NOOP;
    append(head, tracker, condStart);
    struct InstructionNode* noOpNode = new InstructionNode();
    noOpNode->line_no = token.line_no;
    noOpNode->type = NOOP;
    token = lexer.GetToken();  // first token of the condition
    parse_condition(head, tracker, noOpNode);
    // token is now the ';' before the update

    token = lexer.GetToken();
    struct InstructionNode* updateNode = nullptr, *updateTracker = nullptr;
    if (token.token_type == ID && symbolTable.count(token.lexeme)) {
        parse_assignment_for_statement(updateNode, updateTracker);
        token = lexer.GetToken(); token = lexer.GetToken();
        g_loops.push_back({noOpNode, updateNode});  // 'continue' runs the update, then the condition
        parse_block_body(head, tracker);
        g_loops.pop_back();
        struct InstructionNode* jmpNode = new InstructionNode();
        jmpNode->line_no = token.line_no;
        jmpNode->type = JMP; jmpNode->jmp_inst.target = condStart;
        tracker->next = updateNode; updateTracker->next = jmpNode; jmpNode->next = noOpNode; tracker = noOpNode;
    }
}

void parse_switch_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    token = lexer.GetToken();
    expect_condition_lparen("switch");
    int switchSlot = alloc_temp();
    int resultIdx  = parse_expression(head, tracker);
    struct InstructionNode* copyNode = new InstructionNode();
    copyNode->line_no = token.line_no;
    copyNode->type = ASSIGN;
    copyNode->assign_inst.left_hand_side_index = switchSlot;
    copyNode->assign_inst.operand1_index       = resultIdx;
    copyNode->assign_inst.op                   = OPERATOR_NONE;
    append(head, tracker, copyNode);
    expect_condition_rparen("switch");        // token is now '{'
    token = lexer.GetToken();
    struct InstructionNode* noOpNode = new InstructionNode(); 
    noOpNode->line_no = token.line_no;
    noOpNode->type = NOOP;
    struct InstructionNode* lastCaseNode = nullptr;
    while (token.token_type == CASE || token.lexeme == "default") {
        if (token.token_type == CASE) 
        {
            token = lexer.GetToken();
            if (token.token_type == NUM) 
            {
                struct InstructionNode* caseNode = new InstructionNode();
                caseNode->line_no = token.line_no;
                caseNode->type = CJMP;
                caseNode->cjmp_inst.operand_type = TYPE_INT;
                caseNode->cjmp_inst.operand1_index = switchSlot;
                int s = alloc_slot();
                mem[s] = stoi(token.lexeme);
                caseNode->cjmp_inst.operand2_index = s;
                caseNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
                if (lastCaseNode) lastCaseNode->next = caseNode;
                lastCaseNode = caseNode;
                if (head == nullptr) { head = tracker = caseNode; } else { tracker->next = caseNode; tracker = caseNode; }
                token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
                struct InstructionNode* bodyHead = nullptr, *bodyTracker = nullptr;
                parse_block_body(bodyHead, bodyTracker);
                caseNode->cjmp_inst.target = bodyHead;
                struct InstructionNode* jmpNode = new InstructionNode();
                jmpNode->line_no = token.line_no;
                jmpNode->type = JMP; jmpNode->jmp_inst.target = noOpNode;
                bodyTracker->next = jmpNode;
            }
        } 
        else if (token.lexeme == "default") 
        {
            struct InstructionNode* defaultNode = new InstructionNode();
            defaultNode->line_no = token.line_no; 
            defaultNode->type = JMP;
            if (lastCaseNode) lastCaseNode->next = defaultNode;
            lastCaseNode = defaultNode;
            if (head == nullptr) { head = tracker = defaultNode; } else { tracker->next = defaultNode; tracker = defaultNode; }
            token = lexer.GetToken(); token = lexer.GetToken(); token = lexer.GetToken();
            struct InstructionNode* bodyHead = nullptr, *bodyTracker = nullptr;
            parse_block_body(bodyHead, bodyTracker);
            defaultNode->jmp_inst.target = bodyHead;
            struct InstructionNode* jmpNode = new InstructionNode();
            jmpNode->line_no = token.line_no;
            jmpNode->type = JMP; 
            jmpNode->jmp_inst.target = noOpNode;
            bodyTracker->next = jmpNode;
        }
        token = lexer.GetToken();
    }
    if (lastCaseNode) lastCaseNode->next = noOpNode;
    tracker->next = noOpNode; tracker = noOpNode;
}
