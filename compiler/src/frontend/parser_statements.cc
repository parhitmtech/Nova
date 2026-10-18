/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Statements: declarations, print, assignment, conditions, if/while/do/for/switch. Split out of parser.cc.
#include "parser_internal.h"

void parse_typed_declaration(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    VarType declType = TYPE_UNKNOWN;
    if      (token.token_type == INT_TYPE)    declType = TYPE_INT;
    else if (token.token_type == BOOL_TYPE)   declType = TYPE_BOOL;
    else if (token.token_type == STRING_TYPE) declType = TYPE_STRING;
    else if (token.token_type == FLOAT_TYPE) declType = TYPE_FLOAT;
    else if (token.token_type == DOUBLE_TYPE) declType = TYPE_DOUBLE;

    token = lexer.GetToken();
    string name = token.lexeme;
    int nameLine = token.line_no;

    if (symbolTable.count(name) || arrayTable.count(name)) 
    {
        report_error(nameLine, "redeclaration of '" + name + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
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
    // TYPE_TENSOR is allowed on the RHS of int declarations — tensor handles are integers
    bool tensorToInt = (rhsType == TYPE_TENSOR && declType == TYPE_INT);
    if (rhsType != TYPE_UNKNOWN && rhsType != declType && !tensorToInt)
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
    // Propogate tensor_slot-ness through a plain copy: "int x = someTensorVar ;"
    // (tensorToInt covers "int x = tensor_zeros(...)" directly, but resultIdx is already tagged by 
    // parse_tensor_call in that  case too, so this one check covers both - a fresh tensor-call result
    // and a copy of an existing one).
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
        if (token.token_type == INT_TYPE) vt = TYPE_INT;
        else if (token.token_type == BOOL_TYPE) vt = TYPE_BOOL;
        else if (token.token_type == STRING_TYPE) vt = TYPE_STRING;
        else if (token.token_type == FLOAT_TYPE) vt = TYPE_FLOAT;
        else if (token.token_type == DOUBLE_TYPE) vt = TYPE_DOUBLE;

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

void parse_statement(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == INT_TYPE || token.token_type == BOOL_TYPE || 
        token.token_type == STRING_TYPE || token.token_type == FLOAT_TYPE || 
        token.token_type == DOUBLE_TYPE) 
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
            retExprs.push_back(parse_expression(head, tracker));
            while (token.token_type == COMMA)
            {
                token = lexer.GetToken();
                retExprs.push_back(parse_expression(head, tracker));
            }
            int nExpected = currentFuncRetSlots.empty() ? 1 : (int)currentFuncRetSlots.size();
            if ((int)retExprs.size() != nExpected)
            {
                report_error(token.line_no, "return count mismatch: expected " + to_string(nExpected) + " got " + to_string(retExprs.size()));
            }
            for (int i = 0;i < (int)retExprs.size();i++)
            {
                int rSlot = (i < (int)currentFuncRetSlots.size()) ? currentFuncRetSlots[i] : currentFuncRetIdx;
                VarType rt = (i < (int)currentFuncRetTypes.size()) ? currentFuncRetTypes[i] : currentFuncRetType;
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
        token = lexer.GetToken();  // consume SELF → '.'
        string fieldPath = "";
        while (token.token_type == DOT)
        {
            token = lexer.GetToken();  // consume '.' → field segment
            if (!fieldPath.empty()) fieldPath += ".";
            fieldPath += token.lexeme;
            token = lexer.GetToken();  // consume field segment → next token
        }
        // self.field[index] = value (array write)
        if (token.token_type == LBRAC)
        {
            if (!currentSelfFieldSlots.count(fieldPath))
            {
                report_error(token.line_no, "'" + currentSelfClassName + "' has no array field '" + fieldPath + "'");
                while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
                {
                    token = lexer.GetToken();
                }
                return;
            }
            token = lexer.GetToken();  // first token of index expression
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']' -> '='
            token = lexer.GetToken();  // past '=' -> value expression
            int valueSlot = parse_expression(head, tracker);

            int arrSize = currentSelfFieldArraySizes.count(fieldPath) ? currentSelfFieldArraySizes[fieldPath] : 0;

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_WRITE;
            node->array_inst.base_index = currentSelfFieldSlots[fieldPath];
            node->array_inst.dynamic_base = true;
            node->array_inst.index_slot = indexSlot;
            node->array_inst.target_index = valueSlot;
            node->array_inst.array_size = arrSize;
            node->array_inst.size_slot = -1;
            node->array_inst.line_no = token.line_no;
            append(head, tracker, node);
            return;
        }
        // token = '='
        token = lexer.GetToken();  // consume '=' → RHS
        int resultIdx = parse_expression(head, tracker);

        if (!currentSelfFieldSlots.count(fieldPath))
        {
            report_error(token.line_no, "class '" + currentSelfClassName + "' has no field '" + fieldPath + "'");
            return;
        }
        int slot = currentSelfFieldSlots[fieldPath];
        VarType fType = currentSelfFieldTypes[fieldPath];
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = slot;
            node->assign_f_inst.operand1_index = resultIdx;
            node->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
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
        else if (classTable.count(name) && lexer.peek(1).token_type == ID)
        {
            // class instantiation: MyClass obj ; or MyClass obj = { ... } ;
            parse_class_instantiation(name, head, tracker);
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
                parse_method_call(name, memberName, head, tracker);
                // token is now ';'
            }
            else
            {
                // field assignment
                if (token.token_type != EQUAL)
                {
                    report_error(objLine, "expected '=' or '(' after '" + name + "." + memberName + "'");
                    while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
                    return;
                }
                token = lexer.GetToken();  // consume '=' → RHS
                int resultIdx = parse_expression(head, tracker);

                if (!classFieldSlots.count(name) || !classFieldSlots[name].count(memberName))
                {
                    report_error(objLine, "class instance '" + name + "' has no field '" + memberName + "'");
                    return;
                }
                int slot = classFieldSlots[name][memberName];
                VarType fType = classFieldTypes[name][memberName];
                struct InstructionNode* node = new InstructionNode();
                node->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    node->type = ASSIGN_F;
                    node->assign_f_inst.left_hand_side_index = slot;
                    node->assign_f_inst.operand1_index = resultIdx;
                    node->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
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
        else if ((functionTable.find(name) != functionTable.end()) || functionParams.count(name))
        {
            // functionParams also covers a function calling itself as a statement while its own
            // body is being parsed
            parse_function_call(head, tracker);
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
    bool tensorToInt = (rhsType == TYPE_TENSOR && lhsType == TYPE_INT);
    if (lhsType != TYPE_UNKNOWN && rhsType != TYPE_UNKNOWN && lhsType != rhsType && !tensorToInt)
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
    while (token.token_type != RBRACE && token.token_type != END_OF_FILE) 
    { 
        parse_statement(head, tracker); token = lexer.GetToken(); 
    }
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
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE) 
        { 
            parse_statement(head, tracker); token = lexer.GetToken(); 
        }
        tracker->next = noOpNode; tracker = noOpNode;
    } 
    else 
    {
        condFailNode->next = noOpNode; tracker = noOpNode;
    }
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
    while (token.token_type != RBRACE && token.token_type != END_OF_FILE) { parse_statement(head, tracker); token = lexer.GetToken(); }
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

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        parse_statement(head, tracker);
        token = lexer.GetToken();
    }
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
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE) { parse_statement(head, tracker); token = lexer.GetToken(); }
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
                while (token.token_type != RBRACE && token.token_type != END_OF_FILE) { parse_statement(bodyHead, bodyTracker); token = lexer.GetToken(); }
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
            while (token.token_type != RBRACE && token.token_type != END_OF_FILE) { parse_statement(bodyHead, bodyTracker); token = lexer.GetToken(); }
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
