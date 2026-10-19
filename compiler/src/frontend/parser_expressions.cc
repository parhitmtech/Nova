/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Expressions: function calls, factors, terms, arithmetic and type coercion. Split out of parser.cc.
#include "parser_internal.h"

int parse_function_call(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    string funcName = token.lexeme;
    token = lexer.GetToken();  // '('
    token = lexer.GetToken();  // first arg or ')'

    vector<int>     argIndices;
    vector<VarType> argTypes;

    while (token.token_type != RPAREN && token.token_type != END_OF_FILE) {
        int argIdx = parse_expression(head, tracker);
        argIndices.push_back(argIdx);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    vector<string>& params = functionParams[funcName];
    if (argIndices.size() != params.size())
        report_error(token.line_no, "function '" + funcName + "' expects " + to_string(params.size()) + " argument(s) but got " + to_string(argIndices.size()));

    if (functionParamTypes.count(funcName)) {
        vector<VarType>& pTypes = functionParamTypes[funcName];
        for (int i = 0; i < (int)argTypes.size() && i < (int)pTypes.size(); i++) {
            if (!types_compatible(argTypes[i], pTypes[i]))
                report_error(token.line_no, "argument " + to_string(i+1) + " of '" + funcName + "' expects " + typeToString(pTypes[i]) + " but got " + typeToString(argTypes[i]));
        }
    }

    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head   = functionTable[funcName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[funcName];
    callNode->call_inst.func_slot_base  = functionSlotRange.count(funcName) ? functionSlotRange[funcName].first : 0;
    callNode->call_inst.func_slot_count = functionSlotRange.count(funcName) ? functionSlotRange[funcName].second : 0;
    callNode->call_inst.num_params      = (int)params.size();
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    callNode->call_inst.ret_type = functionReturnType.count(funcName) ? functionReturnType[funcName] : TYPE_UNKNOWN;
    callNode->call_inst.ret_slot_types = nullptr;

    if (params.size() > 0) {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = functionParamSlots[funcName];
        for (int i = 0; i < (int)params.size(); i++) {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* pTypes = new VarType[params.size()];
        vector<VarType>& storedParamTypes = functionParamTypes[funcName];
        for (int i = 0;i < (int)params.size();i++)
        {
            pTypes[i] = (i < (int)storedParamTypes.size()) ? storedParamTypes[i] : TYPE_UNKNOWN;
        }
        VarType* aTypes = new VarType[params.size()];
        for (int i = 0;i < (int)params.size();i++)
        {
            aTypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;
        }
        callNode->call_inst.param_types = pTypes;
        callNode->call_inst.arg_types = aTypes;
        callNode->call_inst.param_slots   = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    } 
    else {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
        callNode->call_inst.param_types = nullptr;
        callNode->call_inst.arg_types = nullptr;
    }
    append(head, tracker, callNode);

    // int freshSlot = alloc_temp();
    // struct InstructionNode* copyRetNode = new InstructionNode();
    // copyRetNode->line_no = token.line_no;
    // copyRetNode->type = ASSIGN;
    // copyRetNode->assign_inst.left_hand_side_index = freshSlot;
    // copyRetNode->assign_inst.operand1_index       = functionReturnIndex[funcName];
    // copyRetNode->assign_inst.op                   = OPERATOR_NONE;
    // append(head, tracker, copyRetNode);

    // lastExprType = functionReturnType.count(funcName) ? functionReturnType[funcName] : TYPE_UNKNOWN;

    // Copy the return value out in the callee's own slot space.
    // (Old code always used an int ASSIGN, which broke float/double.)
    VarType funcRetType = functionReturnType.count(funcName) ? functionReturnType[funcName] : TYPE_UNKNOWN;
    int freshSlot = (funcRetType == TYPE_FLOAT) ? alloc_float_slot() : 
                    (funcRetType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_temp();
    struct InstructionNode* copyRetNode = new InstructionNode();
    copyRetNode->line_no = token.line_no;
    if (funcRetType == TYPE_FLOAT)
    {
        copyRetNode->type = ASSIGN_F;
        copyRetNode->assign_f_inst.left_hand_side_index = freshSlot;
        copyRetNode->assign_f_inst.operand1_index = functionReturnIndex[funcName];
        copyRetNode->assign_f_inst.op = OPERATOR_NONE;
    }
    else if (funcRetType == TYPE_DOUBLE)
    {
        copyRetNode->type = ASSIGN_D;
        copyRetNode->assign_d_inst.left_hand_side_index = freshSlot;
        copyRetNode->assign_d_inst.operand1_index = functionReturnIndex[funcName];
        copyRetNode->assign_d_inst.op = OPERATOR_NONE;
    }
    else 
    {
        copyRetNode->type = ASSIGN;
        copyRetNode->assign_inst.left_hand_side_index = freshSlot;
        copyRetNode->assign_inst.operand1_index = functionReturnIndex[funcName];
        copyRetNode->assign_inst.op = OPERATOR_NONE;
    }
    append(head, tracker, copyRetNode);
    lastExprType = funcRetType;
    return freshSlot;
}

// dst = a <op> b on int slots (op may be OPERATOR_NONE for a plain copy: dst = a).
static struct InstructionNode* emit_int_assign(int dst, int a, ArithmeticOperatorType op, int b, int line,
                                               struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = ASSIGN;
    node->assign_inst.left_hand_side_index = dst;
    node->assign_inst.operand1_index       = a;
    node->assign_inst.operand2_index       = b;
    node->assign_inst.op                   = op;
    append(head, tracker, node);
    return node;
}

static int int_constant(int v)
{
    int s = alloc_slot();
    mem[s] = v;
    return s;
}

// -x, emitted as 0 - x in x's own type.
static int emit_negate(int v, VarType t, int line,
                       struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (t == TYPE_FLOAT || t == TYPE_DOUBLE)
    {
        bool isD = (t == TYPE_DOUBLE);
        int zero = isD ? alloc_double_slot() : alloc_float_slot();  // starts at 0.0
        int dst  = isD ? alloc_double_slot() : alloc_float_slot();
        struct InstructionNode* node = new InstructionNode();
        node->line_no = line;
        if (isD)
        {
            node->type = ASSIGN_D;
            node->assign_d_inst.left_hand_side_index = dst;
            node->assign_d_inst.operand1_index = zero;
            node->assign_d_inst.operand2_index = v;
            node->assign_d_inst.op = OPERATOR_MINUS;
        }
        else
        {
            node->type = ASSIGN_F;
            node->assign_f_inst.left_hand_side_index = dst;
            node->assign_f_inst.operand1_index = zero;
            node->assign_f_inst.operand2_index = v;
            node->assign_f_inst.op = OPERATOR_MINUS;
        }
        append(head, tracker, node);
        lastExprType = t;
        return dst;
    }
    if (t != TYPE_INT && t != TYPE_UNKNOWN)
        report_error(line, "unary '-' needs a number, got " + typeToString(t));
    int dst = alloc_temp();
    emit_int_assign(dst, int_constant(0), OPERATOR_MINUS, v, line, head, tracker);
    lastExprType = TYPE_INT;
    return dst;
}

int parse_factor(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type == MINUS)
    {
        // unary minus: a literal folds into the constant (-2, -0.5); anything else becomes 0 - x
        int line = token.line_no;
        token = lexer.GetToken();
        if (token.token_type == NUM)
        {
            int idx = int_constant((int)-stoll(token.lexeme));
            lastExprType = TYPE_INT;
            token = lexer.GetToken();
            return idx;
        }
        if (token.token_type == FLOAT_LITERAL)
        {
            int idx = alloc_float_slot();
            fmem[idx] = -stof(token.lexeme);
            lastExprType = TYPE_FLOAT;
            token = lexer.GetToken();
            return idx;
        }
        int v = parse_factor(head, tracker);
        return emit_negate(v, lastExprType, line, head, tracker);
    }
    if (token.token_type == LPAREN)
    {
        token = lexer.GetToken();
        int result = parse_expression(head, tracker);
        token = lexer.GetToken();
        return result;
    } 
    else if (token.token_type == NUM) 
    {
        int idx = alloc_slot();
        mem[idx] = stoi(token.lexeme);
        lastExprType = TYPE_INT;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == TRUE) 
    {
        int idx = alloc_slot();
        mem[idx] = 1;
        lastExprType = TYPE_BOOL;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == FALSE) 
    {
        int idx = alloc_slot();
        mem[idx] = 0;
        lastExprType = TYPE_BOOL;
        token = lexer.GetToken();
        return idx;
    } 
    else if (token.token_type == INPUT) 
    {
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();  // prompt or ')'
        if (token.token_type == STRING) token = lexer.GetToken();
        token = lexer.GetToken();  // past ')'
        int slot = alloc_temp();
        struct InstructionNode* inNode = new InstructionNode();
        inNode->line_no = token.line_no;
        inNode->type = IN;
        inNode->input_inst.var_index = slot;
        append(head, tracker, inNode);
        lastExprType = TYPE_INT;
        return slot;
    } 
    else if (token.token_type == ID)
    {
        if (token.lexeme == "hf" && lexer.peek(1).token_type == DOT)
        {
            token = lexer.GetToken();  // consume "hf" → '.'
            token = lexer.GetToken();  // consume '.' → method name
            string method = token.lexeme;
            token = lexer.GetToken();  // consume method name → '('
            return parse_hf_call(method, head, tracker);
        }

        if (token.lexeme == "sol" && lexer.peek(1).token_type == DOT)
        {
            token = lexer.GetToken();  // consume "sol" → '.'
            token = lexer.GetToken();  // consume '.' → method name
            string method = token.lexeme;
            token = lexer.GetToken();  // consume method name → '('
            return parse_sol_call(method, head, tracker);
        }

        if (functionTable.find(token.lexeme) != functionTable.end()) return parse_function_call(head, tracker);

        if (TENSOR_OP_MAP.count(token.lexeme))
        {
            string name = token.lexeme;
            token = lexer.GetToken();  // move past the name to '('
            return parse_tensor_call(name, head, tracker);
        }

        if (arrayTable.find(token.lexeme) != arrayTable.end()) 
        {
            string arrName = token.lexeme;
            int    base    = arrayTable[arrName];
            token = lexer.GetToken();  // '['
            token = lexer.GetToken();
            int indexLine = token.line_no;
            if (token.token_type == NUM) check_bounds(arrName, stoi(token.lexeme), indexLine);
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']'
            int tempSlot = alloc_temp();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_READ;
            bool isDyn = arrayDynamic.count(arrName) && arrayDynamic[arrName];
            node->array_inst.base_index   = base;
            node->array_inst.dynamic_base = isDyn;
            node->array_inst.index_slot   = indexSlot;
            node->array_inst.target_index = tempSlot;
            node->array_inst.array_size   = isDyn ? -1 : arraySizeTable[arrName];
            node->array_inst.size_slot = isDyn ? arraySizeTable[arrName] : -1;
            node->array_inst.line_no = indexLine;
            append(head, tracker, node);
            lastExprType = TYPE_INT;
            return tempSlot;
        }

        if (typeTable.count(token.lexeme) && typeTable[token.lexeme] == TYPE_FLOAT)
        {
            lastExprType = TYPE_FLOAT;
            int idx = floatSymbolTable[token.lexeme];
            token = lexer.GetToken();
            return idx;
        }

        if (typeTable.count(token.lexeme) && typeTable[token.lexeme] == TYPE_DOUBLE)
        {
            lastExprType = TYPE_DOUBLE;
            int idx = doubleSymbolTable[token.lexeme];
            token = lexer.GetToken();
            return idx;
        }

        // SELF keyword: self.field read inside a class method
        // (handled as a separate else-if branch outside this ID block;
        //  but if someone aliases "self" as an ID, handle gracefully)

        // class instance field read or method call: obj.field or obj.method(args)
        if (varClassType.count(token.lexeme) && lexer.peek(1).token_type == DOT)
        {
            string varName = token.lexeme;
            token = lexer.GetToken();  // consume var name → '.'
            token = lexer.GetToken();  // consume '.' → member name
            string memberName = token.lexeme;
            token = lexer.GetToken();  // consume member name → next token

            if (token.token_type == LPAREN)
            {
                // method call in expression context: obj.method(args)
                return parse_method_call(varName, memberName, head, tracker);
            }
            else
            {
                // field read: obj.field
                if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(memberName))
                {
                    report_error(token.line_no, "class instance '" + varName + "' has no field '" + memberName + "'");
                    lastExprType = TYPE_UNKNOWN;
                    return 0;
                }
                lastExprType = classFieldTypes[varName][memberName];
                return classFieldSlots[varName][memberName];
            }
        }

        // struct field read: p.x or L.a.x
        if (varStructType.count(token.lexeme) && lexer.peek(1).token_type == DOT)
        {
            string varName = token.lexeme;
            token = lexer.GetToken();  // consume var name -> token = DOT

            string fieldPath = "";
            while (token.token_type == DOT)
            {
                token = lexer.GetToken();  // consume DOT -> token = field segment
                if (!fieldPath.empty()) fieldPath += ".";
                fieldPath += token.lexeme;
                token = lexer.GetToken();  // consume field segment -> next token
            }
            // token is now whatever follows the field access

            if (!structFieldSlots[varName].count(fieldPath))
            {
                report_error(token.line_no, "struct '" + varName + "' has no field '" + fieldPath + "'");
                lastExprType = TYPE_UNKNOWN;
                return 0;
            }
            lastExprType = structFieldTypes[varName][fieldPath];
            return structFieldSlots[varName][fieldPath];
        }

        if (!check_declared(token.lexeme, token.line_no)) { token = lexer.GetToken(); lastExprType = TYPE_UNKNOWN; return 0; }

        lastExprType = typeTable.count(token.lexeme) ? typeTable[token.lexeme] : TYPE_UNKNOWN;
        int idx = symbolTable[token.lexeme];
        token = lexer.GetToken();
        return idx;
    }
    else if (token.token_type == STRING)
    {
        int strIdx = next_str_available++;
        strMem.push_back(token.lexeme);
        int slot = alloc_slot();
        mem[slot] = strIdx;
        lastExprType = TYPE_STRING;
        token = lexer.GetToken();
        return slot;
    }
    else if (token.token_type == FLOAT_LITERAL)
    {
        // store in fmem, return from index
        int idx = alloc_float_slot();
        fmem[idx] = stof(token.lexeme);
        lastExprType = TYPE_FLOAT;
        token = lexer.GetToken();
        return idx;
    }
    else if (token.token_type == SELF)
    {
        // self.field read inside a class method
        if (!insideFunction || currentSelfClassName.empty())
        {
            report_error(token.line_no, "'self' used outside of a class method");
            lastExprType = TYPE_UNKNOWN;
            return 0;
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
        // self.field[index] (array read)
        if (token.token_type == LBRAC)
        {
            if (!currentSelfFieldSlots.count(fieldPath))
            {
                report_error(token.line_no, "'" + currentSelfClassName + "' has no array field '" + fieldPath + "'");
                lastExprType = TYPE_UNKNOWN;
                return 0;
            }
            int indexLine = token.line_no;
            token = lexer.GetToken();  // first token of index expression
            int indexSlot = parse_expression(head, tracker);
            token = lexer.GetToken();  // past ']'

            int arrSize = currentSelfFieldArraySizes.count(fieldPath) ? currentSelfFieldArraySizes[fieldPath] : 0;

            int tempSlot = alloc_temp();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = ARRAY_READ;
            node->array_inst.base_index = currentSelfFieldSlots[fieldPath];
            node->array_inst.dynamic_base = true;
            node->array_inst.index_slot = indexSlot;
            node->array_inst.target_index = tempSlot;
            node->array_inst.array_size = arrSize;
            node->array_inst.size_slot = -1;
            node->array_inst.line_no = indexLine;
            append(head, tracker, node);

            lastExprType = TYPE_INT;
            return tempSlot;
        }
        if (!currentSelfFieldSlots.count(fieldPath))
        {
            report_error(token.line_no, "class '" + currentSelfClassName + "' has no field '" + fieldPath + "'");
            lastExprType = TYPE_UNKNOWN;
            return 0;
        }
        lastExprType = currentSelfFieldTypes[fieldPath];
        return currentSelfFieldSlots[fieldPath];
    }
    else if (token.token_type == FLOAT_TYPE || token.token_type == DOUBLE_TYPE || token.token_type == INT_TYPE)
    {
        // explicit cast: float(x), double(x), int(x)
        VarType dstType = (token.token_type == FLOAT_TYPE) ? TYPE_FLOAT :
                          (token.token_type == DOUBLE_TYPE) ? TYPE_DOUBLE : TYPE_INT;
        token = lexer.GetToken(); // '('
        token = lexer.GetToken(); // expression start
        int srcIdx = parse_expression(head, tracker);
        VarType srcType = lastExprType;
        token = lexer.GetToken(); // past ')'

        // allocate destination slot 
        int dstIdx = 0;
        if (dstType == TYPE_FLOAT) dstIdx = alloc_float_slot();
        else if (dstType == TYPE_DOUBLE) dstIdx = alloc_double_slot();
        else dstIdx = alloc_slot();

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = CAST;
        node->cast_inst.src_index = srcIdx;
        node->cast_inst.dst_index = dstIdx;
        node->cast_inst.src_type = srcType;
        node->cast_inst.dst_type = dstType;
        append(head, tracker, node);

        lastExprType = dstType;
        return dstIdx;
    }
    lastExprType = TYPE_UNKNOWN;
    return 0;
}

int coerce_float_double(int slot, VarType fromType, VarType toType,
                                struct InstructionNode*& head, struct InstructionNode*& tracker,
                                int lineNo)
{
    if (fromType == toType) return slot;
    int dst = (toType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_float_slot();
    struct InstructionNode* node = new InstructionNode();
    node->line_no = lineNo;
    node->type = CAST;
    node->cast_inst.src_index = slot;
    node->cast_inst.dst_index = dst;
    node->cast_inst.src_type = fromType;
    node->cast_inst.dst_type = toType;
    append(head, tracker, node);
    return dst; 
}

int parse_term(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_factor(head, tracker);
    VarType leftType = lastExprType;
    while (token.token_type == MULT || token.token_type == DIV || token.token_type == MOD) {
        int opLine = token.line_no;
        if (token.token_type == MOD)
        {
            // a % b = a - (a / b) * b on ints (the result has the sign of a, as in C), built from
            // instructions every back end already runs. b == 0 stops with the division-by-zero error.
            token = lexer.GetToken();
            int right = parse_factor(head, tracker);
            VarType rightType = lastExprType;
            if ((leftType != TYPE_INT && leftType != TYPE_UNKNOWN) || (rightType != TYPE_INT && rightType != TYPE_UNKNOWN))
                report_error(opLine, "'%' needs int operands, got " + typeToString(leftType) + " and " + typeToString(rightType));
            int quotient = alloc_temp(), product = alloc_temp(), rem = alloc_temp();
            emit_int_assign(quotient, left, OPERATOR_DIV, right, opLine, head, tracker);
            emit_int_assign(product, quotient, OPERATOR_MULT, right, opLine, head, tracker);
            emit_int_assign(rem, left, OPERATOR_MINUS, product, opLine, head, tracker);
            left = rem; leftType = TYPE_INT; lastExprType = TYPE_INT;
            continue;
        }
        ArithmeticOperatorType op = (token.token_type == MULT) ? OPERATOR_MULT : OPERATOR_DIV;

        // if (leftType == TYPE_FLOAT)
        // {
        //     token = lexer.GetToken();
        //     int right = parse_factor(head, tracker);
        //     int tmp = alloc_float_slot();
        //     struct InstructionNode* node = new InstructionNode();
        //     node->line_no = token.line_no;
        //     node->type = ASSIGN_F;
        //     node->assign_f_inst.left_hand_side_index = tmp;
        //     node->assign_f_inst.operand1_index = left;
        //     node->assign_f_inst.operand2_index = right;
        //     node->assign_f_inst.op = op;
        //     append(head, tracker, node);
        //     left = tmp; leftType = TYPE_FLOAT; lastExprType = TYPE_FLOAT;
        //     continue;
        // }

        // if (leftType == TYPE_DOUBLE)
        // {
        //     token = lexer.GetToken();
        //     int right = parse_factor(head, tracker);
        //     int tmp = alloc_double_slot();
        //     struct InstructionNode* node = new InstructionNode();
        //     node->line_no = token.line_no;
        //     node->type = ASSIGN_D;
        //     node->assign_d_inst.left_hand_side_index = tmp;
        //     node->assign_d_inst.operand1_index = left;
        //     node->assign_d_inst.operand2_index = right;
        //     node->assign_d_inst.op = op;
        //     append(head, tracker, node);
        //     left = tmp; leftType = TYPE_DOUBLE; lastExprType = TYPE_DOUBLE;
        //     continue;
        // }

        if (leftType == TYPE_FLOAT || leftType == TYPE_DOUBLE)
        {
            token = lexer.GetToken();
            int right = parse_factor(head, tracker);
            VarType rightType = lastExprType;

            if (rightType != TYPE_FLOAT && rightType != TYPE_DOUBLE && rightType != TYPE_UNKNOWN)
            {
                report_error(opLine, "arithmetic requires float/double operands, got " + typeToString(rightType));
            }

            // double wins if either side is double
            VarType opType = (leftType == TYPE_DOUBLE || rightType == TYPE_DOUBLE) ? TYPE_DOUBLE : TYPE_FLOAT;
            int leftOperand = coerce_float_double(left, leftType, opType, head, tracker, opLine);
            int rightOperand = coerce_float_double(right, rightType, opType, head, tracker, opLine);

            int tmp = (opType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_float_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (opType == TYPE_DOUBLE)
            {
                node->type = ASSIGN_D;
                node->assign_d_inst.left_hand_side_index = tmp;
                node->assign_d_inst.operand1_index = leftOperand;
                node->assign_d_inst.operand2_index = rightOperand;
                node->assign_d_inst.op = op;
            }
            else
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = tmp;
                node->assign_f_inst.operand1_index = leftOperand;
                node->assign_f_inst.operand2_index = rightOperand;
                node->assign_f_inst.op = op;
            }
            append(head, tracker, node);
            left = tmp; leftType = opType; lastExprType = opType;
            continue;
        }

        if (leftType != TYPE_UNKNOWN && leftType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(leftType));
        token = lexer.GetToken();
        int right = parse_factor(head, tracker);
        VarType rightType = lastExprType;

        if (rightType != TYPE_UNKNOWN && rightType != TYPE_INT) report_error(opLine, "arithmetic requires int operands, got " + typeToString(rightType));
        int tmp = alloc_temp();

        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);
        left = tmp; leftType = TYPE_INT; lastExprType = TYPE_INT;
    }
    return left;
}

// + and - (and string +)
static int parse_additive(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_term(head, tracker);
    VarType leftType = lastExprType;
    while (token.token_type == PLUS || token.token_type == MINUS) 
    {
        int opLine = token.line_no;
        ArithmeticOperatorType op = (token.token_type == PLUS) ? OPERATOR_PLUS : OPERATOR_MINUS;

        if (op == OPERATOR_PLUS && leftType == TYPE_STRING)
        {
            token = lexer.GetToken();
            int right = parse_term(head, tracker);
            if (lastExprType != TYPE_STRING && lastExprType != TYPE_UNKNOWN)
            {
                report_error(opLine, "cannot concatenate string with " + typeToString(lastExprType));
            }
            int dest = alloc_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            node->type = STRCAT;
            node->strcat_inst.dest_slot = dest;
            node->strcat_inst.left_slot = left;
            node->strcat_inst.right_slot = right;
            append(head, tracker, node);
            left = dest; 
            leftType = TYPE_STRING;
            lastExprType = TYPE_STRING;
            continue;
        }

        // if (leftType == TYPE_FLOAT)
        // {
        //     token = lexer.GetToken();
        //     int right = parse_term(head, tracker);
        //     int tmp = alloc_float_slot();
        //     struct InstructionNode* node = new InstructionNode();
        //     node->line_no = token.line_no;
        //     node->type = ASSIGN_F;
        //     node->assign_f_inst.left_hand_side_index = tmp;
        //     node->assign_f_inst.operand1_index       = left;
        //     node->assign_f_inst.operand2_index       = right;
        //     node->assign_f_inst.op                   = op;
        //     append(head, tracker, node);
        //     left = tmp; leftType = TYPE_FLOAT; lastExprType = TYPE_FLOAT;
        //     continue;
        // }

        // if (leftType == TYPE_DOUBLE)
        // {
        //     token = lexer.GetToken();
        //     int right = parse_term(head, tracker);
        //     int tmp = alloc_double_slot();
        //     struct InstructionNode* node = new InstructionNode();
        //     node->line_no = token.line_no;
        //     node->type = ASSIGN_D;
        //     node->assign_d_inst.left_hand_side_index = tmp;
        //     node->assign_d_inst.operand1_index       = left;
        //     node->assign_d_inst.operand2_index       = right;
        //     node->assign_d_inst.op                   = op;
        //     append(head, tracker, node);
        //     left = tmp; leftType = TYPE_DOUBLE; lastExprType = TYPE_DOUBLE;
        //     continue;
        // }

        if (leftType == TYPE_FLOAT || leftType == TYPE_DOUBLE)
        {
            token = lexer.GetToken();
            int right = parse_term(head, tracker);  // parse_term here, not parse_factor
            VarType rightType = lastExprType;

            if (rightType != TYPE_FLOAT && rightType != TYPE_DOUBLE && rightType != TYPE_UNKNOWN)
            {
                report_error(opLine, "arithmetic requires float/double operands, got " + typeToString(rightType));
            }

            // double wins if either side is double
            VarType opType = (leftType == TYPE_DOUBLE || rightType == TYPE_DOUBLE) ? TYPE_DOUBLE : TYPE_FLOAT;
            // cast each side into opType's array if it isn't there already
            int leftOperand = coerce_float_double(left, leftType, opType, head, tracker, opLine);
            int rightOperand = coerce_float_double(right, rightType, opType, head, tracker, opLine);

            int tmp = (opType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_float_slot();
            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (opType == TYPE_DOUBLE)
            {
                node->type = ASSIGN_D;
                node->assign_d_inst.left_hand_side_index = tmp;
                node->assign_d_inst.operand1_index = leftOperand;
                node->assign_d_inst.operand2_index = rightOperand;
                node->assign_d_inst.op = op;
            }
            else 
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = tmp;
                node->assign_f_inst.operand1_index = leftOperand;
                node->assign_f_inst.operand2_index = rightOperand;
                node->assign_f_inst.op = op;
            }
            append(head, tracker, node);
            left = tmp; leftType = opType; lastExprType = opType;
            continue;
        }

        if (leftType != TYPE_UNKNOWN && leftType != TYPE_INT)
        {
            report_error(opLine, "arithmetic requires int operands, got " + typeToString(leftType));
        }
        token = lexer.GetToken();
        int right = parse_term(head, tracker);
        VarType rightType = lastExprType;
        if (rightType != TYPE_UNKNOWN && rightType != TYPE_INT) 
        {
            report_error(opLine, "arithmetic requires int operands, got " + typeToString(rightType));
        }
        int tmp = alloc_temp();
        struct InstructionNode* node = new InstructionNode();
        node->line_no = token.line_no;
        node->type = ASSIGN;
        node->assign_inst.left_hand_side_index = tmp;
        node->assign_inst.operand1_index       = left;
        node->assign_inst.operand2_index       = right;
        node->assign_inst.op                   = op;
        append(head, tracker, node);
        left = tmp; leftType = TYPE_INT; lastExprType = TYPE_INT;
    }
    return left;
}

// ── Comparisons and and / or / not as values ─────────────────────────────────
// They produce a bool (0 or 1) in an int slot, so they can appear anywhere an expression can:
// nested in parentheses, stored, passed, returned. They are built from CJMP / SCMP and ASSIGN,
// which every back end already runs. A CJMP falls through when its test holds and jumps to its
// target when it does not; and / or skip the right side when the left side decides the result.

static bool comparison_op(TokenType t, ConditionalOperatorType& op)
{
    switch (t)
    {
        case LESS:      op = CONDITION_LESS;          return true;
        case GREATER:   op = CONDITION_GREATER;       return true;
        case LESSEQ:    op = CONDITION_LESS_EQUAL;    return true;
        case GREATEREQ: op = CONDITION_GREATER_EQUAL; return true;
        case EQEQ:      op = CONDITION_EQUAL;         return true;
        case NOTEQUAL:  op = CONDITION_NOTEQUAL;      return true;
        default:        return false;
    }
}

static struct InstructionNode* new_label(int line)
{
    struct InstructionNode* n = new InstructionNode();
    n->line_no = line;
    n->type = NOOP;
    return n;
}

// Test (lhs op rhs): fall through when it holds, jump to failTarget when it does not.
// Strings compare as strings; numbers are converted to a common type first (int < float < double).
static struct InstructionNode* emit_compare(int lhs, VarType lt, ConditionalOperatorType op, int rhs, VarType rt,
                                            struct InstructionNode* failTarget, int line,
                                            struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    struct InstructionNode* n = new InstructionNode();
    n->line_no = line;
    if (lt == TYPE_STRING || rt == TYPE_STRING)
    {
        if (lt != rt && lt != TYPE_UNKNOWN && rt != TYPE_UNKNOWN)
            report_error(line, "cannot compare " + typeToString(lt) + " with " + typeToString(rt));
        n->type = SCMP;
        n->scmp_inst.condition_op   = op;
        n->scmp_inst.operand1_index = lhs;
        n->scmp_inst.operand2_index = rhs;
        n->scmp_inst.target         = failTarget;
    }
    else
    {
        VarType ct = (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) ? TYPE_DOUBLE :
                     (lt == TYPE_FLOAT  || rt == TYPE_FLOAT)  ? TYPE_FLOAT  : TYPE_INT;
        if (ct != TYPE_INT)
        {
            lhs = coerce_float_double(lhs, lt, ct, head, tracker, line);
            rhs = coerce_float_double(rhs, rt, ct, head, tracker, line);
        }
        n->type = CJMP;
        n->cjmp_inst.condition_op   = op;
        n->cjmp_inst.operand1_index = lhs;
        n->cjmp_inst.operand2_index = rhs;
        n->cjmp_inst.operand_type   = ct;
        n->cjmp_inst.target         = failTarget;
    }
    append(head, tracker, n);
    return n;
}

// The instructions of the most recent comparison: result = 0; test; result = 1; done.
// parse_condition uses them to branch on a lone comparison directly (see branch_on_comparison).
static struct {
    int result = -1;
    struct InstructionNode* setFalse = nullptr;
    struct InstructionNode* test = nullptr;
    struct InstructionNode* setTrue = nullptr;
    struct InstructionNode* done = nullptr;
} g_lastComparison;

// If the condition just parsed (value v, last instruction tracker) is exactly one comparison, turn its
// "store true/false" code into a single jump to failTarget: the same code as before comparisons became
// values, and a shape the JIT can compile. Returns false (and changes nothing) otherwise.
bool branch_on_comparison(int v, struct InstructionNode* failTarget, struct InstructionNode* tracker)
{
    if (g_lastComparison.result != v || g_lastComparison.done != tracker) return false;
    g_lastComparison.setFalse->type = NOOP;
    g_lastComparison.setTrue->type = NOOP;
    if (g_lastComparison.test->type == SCMP) g_lastComparison.test->scmp_inst.target = failTarget;
    else                                     g_lastComparison.test->cjmp_inst.target = failTarget;
    g_lastComparison.result = -1;
    return true;
}

// Test whether v is true (non-zero): fall through when its truth equals wantTrue, otherwise jump to
// failTarget. Any number works as a condition, as in C; a string does not.
void emit_truth_test(int v, VarType t, bool wantTrue, struct InstructionNode* failTarget, int line,
                     struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (t == TYPE_STRING)
        report_error(line, "a string is not a condition; compare it instead, e.g. (s == \"yes\")");
    int zero;
    if (t == TYPE_FLOAT)       zero = alloc_float_slot();
    else if (t == TYPE_DOUBLE) zero = alloc_double_slot();
    else                       zero = int_constant(0);
    emit_compare(v, t, wantTrue ? CONDITION_NOTEQUAL : CONDITION_EQUAL, zero, t, failTarget, line, head, tracker);
}

// additive (== != < > <= >=) additive
static int parse_comparison(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_additive(head, tracker);
    VarType lt = lastExprType;
    ConditionalOperatorType op;
    if (!comparison_op(token.token_type, op)) return left;

    int line = token.line_no;
    token = lexer.GetToken();
    int right = parse_additive(head, tracker);
    VarType rt = lastExprType;

    // result = 0; if (left op right) result = 1
    int result = alloc_temp();
    struct InstructionNode* done = new_label(line);
    g_lastComparison.setFalse = emit_int_assign(result, int_constant(0), OPERATOR_NONE, 0, line, head, tracker);
    g_lastComparison.test     = emit_compare(left, lt, op, right, rt, done, line, head, tracker);
    g_lastComparison.setTrue  = emit_int_assign(result, int_constant(1), OPERATOR_NONE, 0, line, head, tracker);
    append(head, tracker, done);
    g_lastComparison.done   = done;
    g_lastComparison.result = result;

    if (comparison_op(token.token_type, op))
        report_error(token.line_no, "comparisons cannot be chained; use 'and', e.g. (a < b and b < c)");
    lastExprType = TYPE_BOOL;
    return result;
}

// not x
static int parse_not(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    if (token.token_type != NOT) return parse_comparison(head, tracker);
    int line = token.line_no;
    token = lexer.GetToken();
    int v = parse_not(head, tracker);
    VarType t = lastExprType;

    // result = 0; if (v is false) result = 1
    int result = alloc_temp();
    struct InstructionNode* done = new_label(line);
    emit_int_assign(result, int_constant(0), OPERATOR_NONE, 0, line, head, tracker);
    emit_truth_test(v, t, false, done, line, head, tracker);
    emit_int_assign(result, int_constant(1), OPERATOR_NONE, 0, line, head, tracker);
    append(head, tracker, done);
    lastExprType = TYPE_BOOL;
    return result;
}

// a and b and ...: stops at the first false operand
static int parse_and(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_not(head, tracker);
    if (token.token_type != AND) return left;
    int line = token.line_no;

    // result = 0; each operand must be true to go on; result = 1 only if all were
    int result = alloc_temp();
    struct InstructionNode* done = new_label(line);
    emit_int_assign(result, int_constant(0), OPERATOR_NONE, 0, line, head, tracker);
    emit_truth_test(left, lastExprType, true, done, line, head, tracker);
    while (token.token_type == AND)
    {
        token = lexer.GetToken();
        int right = parse_not(head, tracker);
        emit_truth_test(right, lastExprType, true, done, line, head, tracker);
    }
    emit_int_assign(result, int_constant(1), OPERATOR_NONE, 0, line, head, tracker);
    append(head, tracker, done);
    lastExprType = TYPE_BOOL;
    return result;
}

// a or b or ...: stops at the first true operand
static int parse_or(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int left = parse_and(head, tracker);
    if (token.token_type != OR) return left;
    int line = token.line_no;

    // result = 1; any true operand jumps to the end; result = 0 only if all were false
    int result = alloc_temp();
    struct InstructionNode* done = new_label(line);
    emit_int_assign(result, int_constant(1), OPERATOR_NONE, 0, line, head, tracker);
    emit_truth_test(left, lastExprType, false, done, line, head, tracker);
    while (token.token_type == OR)
    {
        token = lexer.GetToken();
        int right = parse_and(head, tracker);
        emit_truth_test(right, lastExprType, false, done, line, head, tracker);
    }
    emit_int_assign(result, int_constant(0), OPERATOR_NONE, 0, line, head, tracker);
    append(head, tracker, done);
    lastExprType = TYPE_BOOL;
    return result;
}

// Full expression, lowest precedence first: or < and < not < comparison < + - < * / % < unary -.
int parse_expression(struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    return parse_or(head, tracker);
}
