/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Structs and classes: definitions, instantiation, method calls. Split out of parser.cc.
#include "parser_internal.h"

void parse_struct_definition()
{
    token = lexer.GetToken();
    string structName = token.lexeme;
    int structLine = token.line_no;

    if (structTable.count(structName))
    {
        report_error(structLine, "duplicate struct definition '" + structName + "'");
    }

    StructDef def;
    def.name = structName;

    token = lexer.GetToken();  // '{'
    token = lexer.GetToken();  // first field or '}'

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        VarType fieldType = TYPE_UNKNOWN;
        string fieldStructType = "";

        if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) { fieldType = type_of_keyword(token.token_type); token = lexer.GetToken(); }
        else if (token.token_type == ID && structTable.count(token.lexeme))
        {
            fieldStructType = token.lexeme;
            token = lexer.GetToken();  // field name
        }
        else
        {
            report_error(token.line_no, "unknown field type '" + token.lexeme + "'");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            {
                token = lexer.GetToken();
            }
            token = lexer.GetToken();
            continue;
        }

        string fieldName = token.lexeme;

        if (!fieldStructType.empty())
        {
            // nested struct - flatten with prefix fieldName.subfield
            for (auto& nestedField : structTable[fieldStructType].fields)
            {
                FieldInfo fi;
                fi.name = fieldName + "." + nestedField.name;
                fi.type = nestedField.type;
                fi.struct_type = nestedField.struct_type;
                def.fields.push_back(fi);
            }
        }
        else
        {
            FieldInfo fi;
            fi.name = fieldName;
            fi.type = fieldType;
            fi.struct_type = "";
            def.fields.push_back(fi);
        }

        token = lexer.GetToken();  // ';'
        token = lexer.GetToken();  // next field or '}'
    }
    // token = '}'
    structTable[structName] = def;
}

void parse_struct_instantiation(const string& structTypeName, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = struct type name (already identified by caller)
    token = lexer.GetToken();  // variable name
    string varName = token.lexeme;
    int varLine = token.line_no;

    if (varStructType.count(varName))
    {
        report_error(varLine, "redeclaration of struct variable '" + varName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
        {
            token = lexer.GetToken();
        }
        return; 
    }

    StructDef& def = structTable[structTypeName];
    varStructType[varName] = structTypeName;

    // allocate one slot per field in the correct memory array
    map<string, int> fieldSlots;
    map<string, VarType> fieldTypes;

    for (auto& field : def.fields)
    {
        int slot = 0;
        if (field.type == TYPE_FLOAT) slot = alloc_float_slot();
        else if (field.type == TYPE_DOUBLE) slot = alloc_double_slot();
        else slot = alloc_slot();
        fieldSlots[field.name] = slot;
        fieldTypes[field.name] = field.type;
    }

    structFieldSlots[varName] = fieldSlots;
    structFieldTypes[varName] = fieldTypes;

    token = lexer.GetToken();  // '=' or ';'

    if (token.token_type == EQUAL)
    {
        // literal init: Point p = {3.14, 2.71} ;
        token = lexer.GetToken();  // '{'
        token = lexer.GetToken();  // first value

        for (int i = 0;i < (int)def.fields.size();i++)
        {
            auto& field = def.fields[i];
            int resultIdx = parse_expression(head, tracker);
            int slot = fieldSlots[field.name];

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (field.type == TYPE_FLOAT)
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = slot;
                node->assign_f_inst.operand1_index = resultIdx;
                node->assign_f_inst.op = OPERATOR_NONE;
            }
            else if (field.type == TYPE_DOUBLE)
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

            if (token.token_type == COMMA) token = lexer.GetToken();
        }
        // token = '}'
        token = lexer.GetToken();  // ';'
    }
    // else token = ';'
}

void parse_class_definition()
{
    token = lexer.GetToken();  // class name
    string className = token.lexeme;
    int classLine = token.line_no;

    if (classTable.count(className))
    {
        report_error(classLine, "duplicate class definition '" + className + "'");
    }

    ClassDef def;
    def.name = className;
    def.parent = "";

    token = lexer.GetToken();  // EXTENDS or '{'

    if (token.token_type == EXTENDS)
    {
        token = lexer.GetToken();  // parent name
        string parentName = token.lexeme;
        if (!classTable.count(parentName))
        {
            report_error(token.line_no, "unknown parent class '" + parentName + "'");
        }
        else
        {
            def.parent = parentName;
            for (auto& f : classTable[parentName].fields)
            {
                def.fields.push_back(f);
            }
        }
        token = lexer.GetToken();  // '{'
    }

    token = lexer.GetToken();  // first member of '}'

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        if (token.token_type == DEF)
        {
            token = lexer.GetToken();  // method name
            string methodName = token.lexeme;
            def.methods.push_back(methodName);

            string fullName = className + "::" + methodName;
            vector<string> params;
            vector<VarType> paramTypes;

            // save symbol tables — restored after body so method-scoped names don't leak
            map<string, int>     savedSymbolTable       = symbolTable;
            map<string, VarType> savedTypeTable         = typeTable;
            map<string, int>     savedFloatSymbolTable  = floatSymbolTable;
            map<string, int>     savedDoubleSymbolTable = doubleSymbolTable;

            token = lexer.GetToken();  // '('
            token = lexer.GetToken();  // first param or ')'
            while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
            {
                VarType pType = TYPE_UNKNOWN;
                if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) { pType = type_of_keyword(token.token_type); token = lexer.GetToken(); }
                if (token.token_type == ID)
                {
                    params.push_back(token.lexeme);
                    paramTypes.push_back(pType);
                    int pSlot = (pType == TYPE_FLOAT) ? alloc_float_slot() : 
                                (pType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
                    if (pType == TYPE_FLOAT) floatSymbolTable[token.lexeme] = pSlot;
                    else if (pType == TYPE_DOUBLE) doubleSymbolTable[token.lexeme] = pSlot;
                    else symbolTable[token.lexeme] = pSlot;
                    typeTable[token.lexeme] = pType;
                    classMethodParamSlots[className][methodName].push_back(pSlot);
                }
                token = lexer.GetToken();
                if (token.token_type == COMMA) token = lexer.GetToken();
            }
            classMethodParams[className][methodName] = params;
            classMethodParamTypes[className][methodName] = paramTypes;

            token = lexer.GetToken();  // ARROW or '{'
            VarType retType = TYPE_UNKNOWN;
            if (token.token_type == ARROW)
            {
                token = lexer.GetToken();
                if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) retType = type_of_keyword(token.token_type);
                else if (token.token_type == ID && classTable.count(token.lexeme)) retType = TYPE_CLASS;
                token = lexer.GetToken();  // '{'
            }
            classMethodReturnType[className][methodName] = retType;

            int retIdx = (retType == TYPE_FLOAT) ? alloc_float_slot() : 
                         (retType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            functionReturnIndex[fullName] = retIdx;

            // allocate self field slots for this method
            map<string, int> selfSlots;
            map<string, VarType> selfTypes;
            map<string, int> arraySizes;
            for (auto& f : def.fields)
            {
                int s = 0;
                if (f.array_size > 0)
                {
                    s = alloc_slot();  // ONE pointer slot, not N slots
                    arraySizes[f.name] = f.array_size;
                }
                else if (f.type == TYPE_FLOAT) s = alloc_float_slot();
                else if (f.type == TYPE_DOUBLE) s = alloc_double_slot();
                else { s = alloc_slot(); g_selfSlots.insert(s); }
                selfSlots[f.name] = s;
                selfTypes[f.name] = f.type; 
            }
            classMethodSelfSlots[className][methodName] = selfSlots;
            classMethodSelfTypes[className][methodName] = selfTypes;
            classMethodArraySizes[className][methodName] = arraySizes;

            // save + set parser context
            bool savedInsideFunc = insideFunction;
            int savedRetIdx = currentFuncRetIdx;
            VarType savedRetType = currentFuncRetType;
            string savedFunctionLabel = currentFunctionLabel;
            string savedSelfClass = currentSelfClassName;
            map<string, int> savedSelfSlots = currentSelfFieldSlots;
            map<string, VarType> savedSelfTypes = currentSelfFieldTypes;
            map<string, int> savedSelfArraySizes = currentSelfFieldArraySizes;

            insideFunction = true;
            currentFuncRetIdx = retIdx;
            currentFuncRetType = retType;
            currentFunctionLabel = className + "." + methodName;
            currentSelfClassName = className;
            currentSelfFieldSlots = selfSlots;
            currentSelfFieldTypes = selfTypes;
            currentSelfFieldArraySizes = arraySizes;
            classTable[className] = def;

            token = lexer.GetToken();  // first token in body
            struct InstructionNode* mhead = nullptr;
            struct InstructionNode* mtracker = nullptr;

            while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
            {
                if (token.token_type == RETURN)
                {
                    token = lexer.GetToken();
                    int resultIdx = parse_expression(mhead, mtracker);
                    check_return_type(lastExprType, retType, token.line_no);

                    struct InstructionNode* an = new InstructionNode();
                    an->line_no = token.line_no;
                    if (retType == TYPE_FLOAT)
                    {
                        an->type = ASSIGN_F;
                        an->assign_f_inst.left_hand_side_index = retIdx;
                        an->assign_f_inst.operand1_index = resultIdx;
                        an->assign_f_inst.op = OPERATOR_NONE;
                    }
                    else if (retType == TYPE_DOUBLE)
                    {
                        an->type = ASSIGN_D;
                        an->assign_d_inst.left_hand_side_index = retIdx;
                        an->assign_d_inst.operand1_index = resultIdx;
                        an->assign_d_inst.op = OPERATOR_NONE;
                    }
                    else
                    {
                        an->type = ASSIGN;
                        an->assign_inst.left_hand_side_index = retIdx;
                        an->assign_inst.operand1_index = resultIdx;
                        an->assign_inst.op = OPERATOR_NONE;
                    }
                    append(mhead, mtracker, an);

                    struct InstructionNode* rn = new InstructionNode();
                    rn->line_no = token.line_no;
                    rn->type = RET;
                    rn->ret_inst.ret_val_index = retIdx;
                    rn->ret_inst.ret_type = retType;
                    append(mhead, mtracker, rn);
                    token = lexer.GetToken();  // advance past ';'
                }
                else
                {
                    parse_statement(mhead, mtracker);
                    token = lexer.GetToken();
                }
            }

            // restore context
            insideFunction = savedInsideFunc;
            currentFuncRetIdx = savedRetIdx;
            currentFuncRetType = savedRetType;
            currentFunctionLabel = savedFunctionLabel;
            currentSelfClassName = savedSelfClass;
            currentSelfFieldSlots = savedSelfSlots;
            currentSelfFieldTypes = savedSelfTypes;
            currentSelfFieldArraySizes = savedSelfArraySizes;

            symbolTable       = savedSymbolTable;
            typeTable         = savedTypeTable;
            floatSymbolTable  = savedFloatSymbolTable;
            doubleSymbolTable = savedDoubleSymbolTable;

            struct InstructionNode* fb = new InstructionNode();
            fb->line_no = token.line_no;
            fb->type = RET;
            fb->ret_inst.ret_val_index = retIdx;
            fb->ret_inst.ret_type = retType;
            append(mhead, mtracker, fb);

            classMethodTable[className][methodName] = mhead;
            token = lexer.GetToken();  // advance past method body '}'
        }
        else if (type_of_keyword(token.token_type) != TYPE_UNKNOWN)
        {
            VarType fieldType = TYPE_UNKNOWN;
            if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) fieldType = type_of_keyword(token.token_type);

            token = lexer.GetToken();  // field name
            ClassFieldInfo fi;
            fi.name = token.lexeme;
            fi.type = fieldType;
            fi.struct_type = "";
            fi.array_size = 0;

            token = lexer.GetToken();  // ';' or '['
            if (token.token_type == LBRAC)
            {
                token = lexer.GetToken();  // size number
                fi.array_size = stoi(token.lexeme);
                token = lexer.GetToken();  // ']'
                token = lexer.GetToken();  // ';'
            }
            def.fields.push_back(fi);
            token = lexer.GetToken();  // next member or '}'
        }
        else if (token.token_type == ID && classTable.count(token.lexeme))
        {
            // nested class type field: ClassName fieldName ;
            string nestedClass = token.lexeme;
            token = lexer.GetToken();  // field name
            string fieldName = token.lexeme;
            for (auto& nf : classTable[nestedClass].fields)
            {
                ClassFieldInfo fi;
                fi.name = fieldName + "." + nf.name;
                fi.type = nf.type;
                fi.struct_type = nf.struct_type;
                def.fields.push_back(fi);
            }
            token = lexer.GetToken();  // ';'
            token = lexer.GetToken();  // next member or '}'
        }
        else
        {
            report_error(token.line_no, "unexpected token in class body: '" + token.lexeme + "'");
            while (token.token_type != SEMICOLON && token.token_type != RBRACE && token.token_type != END_OF_FILE)
                token = lexer.GetToken();
            if (token.token_type == SEMICOLON) token = lexer.GetToken();
        }
    }
    // token = '}'
    classTable[className] = def;
}

void parse_class_instantiation(const string& className, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = class type name (already consumed by caller's peek detection)
    token = lexer.GetToken();  // variable name
    string varName = token.lexeme;
    int varLine = token.line_no;

    if (varClassType.count(varName))
    {
        report_error(varLine, "redeclaration of class variable '" + varName + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        return;
    }

    if (!classTable.count(className))
    {
        report_error(varLine, "unknown class '" + className + "'");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        return;
    }

    ClassDef& def = classTable[className];
    varClassType[varName] = className;
    typeTable[varName] = TYPE_CLASS;
    symbolTable[varName] = alloc_slot();

    map<string, int> fieldSlots;
    map<string, VarType> fieldTypes;
    for (auto& field : def.fields)
    {
        int slot = 0;
        if (field.array_size > 0)
        {
            slot = next_available;
            for (int i = 0;i < field.array_size; i++) alloc_slot();
        }
        else if (field.type == TYPE_FLOAT) slot = alloc_float_slot();
        else if (field.type == TYPE_DOUBLE) slot = alloc_double_slot();
        else { slot = alloc_slot(); g_fieldSlots.insert(slot); }
        fieldSlots[field.name] = slot;
        fieldTypes[field.name] = field.type;
    }
    classFieldSlots[varName] = fieldSlots;
    classFieldTypes[varName] = fieldTypes;

    token = lexer.GetToken();  // '=' or ';'
    if (token.token_type == LPAREN)
    {
        // constructor call: MyClass obj(arg1, arg2) ; 
        token = lexer.GetToken();  // past '('
        vector<int> argIndices;
        vector<VarType> argTypes;
        while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
        {
            int argIdx = parse_expression(head, tracker);
            argIndices.push_back(argIdx);
            argTypes.push_back(lastExprType);
            if (token.token_type == COMMA)
            {
                token = lexer.GetToken();
            }
        }
        token = lexer.GetToken();  // past ')'

        if (!classMethodTable.count(className) || !classMethodTable[className].count("init"))
        {
            report_error(varLine, "class '" + className + "' has no constructor 'init'");
        }
        else
        {
            string fullName = className + "::init";
            map<string, int>& selfSlots = classMethodSelfSlots[className]["init"];
            map<string, VarType>& selfTypes = classMethodSelfTypes[className]["init"];
            map<string, int>& initArrSizes = classMethodArraySizes[className]["init"];
            vector<string>& params = classMethodParams[className]["init"];

            // copy caller field slots into self slots
            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName)) continue;
                int callerSlot = classFieldSlots[varName][fieldName];
                // Array field: store caller's base index as literal pointer
                if (initArrSizes.count(fieldName))
                {
                    int ptrSlot = alloc_slot();
                    mem[ptrSlot] = callerSlot;
                    struct InstructionNode* copyIn = new InstructionNode();
                    copyIn->line_no = token.line_no;
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index       = ptrSlot;
                    copyIn->assign_inst.op                   = OPERATOR_NONE;
                    append(head, tracker, copyIn);
                    continue;
                }
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyIn = new InstructionNode();
                copyIn->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyIn->type = ASSIGN_F;
                    copyIn->assign_f_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_f_inst.operand1_index = callerSlot;
                    copyIn->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyIn->type = ASSIGN_D;
                    copyIn->assign_d_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_d_inst.operand1_index = callerSlot;
                    copyIn->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index = callerSlot;
                    copyIn->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyIn);
            }

            // emit CALL
            struct InstructionNode* callNode = new InstructionNode();
            callNode->line_no = token.line_no;
            callNode->type = CALL;
            callNode->call_inst.function_head = classMethodTable[className]["init"];
            callNode->call_inst.ret_val_index = functionReturnIndex[fullName];
            callNode->call_inst.ret_type = functionReturnType.count(fullName) ? functionReturnType[fullName] : TYPE_UNKNOWN;
            callNode->call_inst.func_slot_base = 0;
            callNode->call_inst.func_slot_count = 0;
            callNode->call_inst.num_params = (int)params.size();
            callNode->call_inst.param_types = nullptr;
            callNode->call_inst.arg_types = nullptr;
            callNode->call_inst.all_ret_slots = nullptr;
            callNode->call_inst.num_ret_slots = 0;
            if (!params.empty())
            {
                int* pSlots = new int[params.size()];
                int* aSlots = new int[params.size()];
                vector<int>& storedParamSlots = classMethodParamSlots[className]["init"];
                for (int i = 0;i < (int)params.size();i++)
                {
                    pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
                    aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
                }
                callNode->call_inst.param_slots = pSlots;
                callNode->call_inst.arg_val_slots = aSlots;
            }
            else
            {
                callNode->call_inst.param_slots = nullptr;
                callNode->call_inst.arg_val_slots = nullptr;
            }
            append(head, tracker, callNode);

            // copy self slots back to caller field slots
            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                // Array fields write directly to caller via dynamic_base — no copy-back needed
                if (initArrSizes.count(fieldName)) continue;
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyBack = new InstructionNode();
                copyBack->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyBack->type = ASSIGN_F;
                    copyBack->assign_f_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_f_inst.operand1_index = selfSlot;
                    copyBack->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyBack->type = ASSIGN_D;
                    copyBack->assign_d_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_d_inst.operand1_index = selfSlot;
                    copyBack->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyBack->type = ASSIGN;
                    copyBack->assign_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_inst.operand1_index = selfSlot;
                    copyBack->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyBack);
            }
        }
        // token = ';'
    }
    else if (token.token_type == EQUAL)
    {
        token = lexer.GetToken();  // '{'
        token = lexer.GetToken();  // first value

        for (int i = 0; i < (int)def.fields.size(); i++)
        {
            auto& field = def.fields[i];
            int resultIdx = parse_expression(head, tracker);
            int slot = fieldSlots[field.name];

            struct InstructionNode* node = new InstructionNode();
            node->line_no = token.line_no;
            if (field.type == TYPE_FLOAT)
            {
                node->type = ASSIGN_F;
                node->assign_f_inst.left_hand_side_index = slot;
                node->assign_f_inst.operand1_index = resultIdx;
                node->assign_f_inst.op = OPERATOR_NONE;
            }
            else if (field.type == TYPE_DOUBLE)
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

            if (token.token_type == COMMA) token = lexer.GetToken();
        }
        // token = '}'
        token = lexer.GetToken();  // ';'
    }
    else if (token.token_type == SEMICOLON)
    {
        // auto-call no-arg init() if it exists
        if (classMethodTable.count(className) && classMethodTable[className].count("init") && classMethodParams[className]["init"].empty())
        {
            string fullName = className + "::init";
            map<string, int>& selfSlots = classMethodSelfSlots[className]["init"];
            map<string, VarType>& selfTypes = classMethodSelfTypes[className]["init"];

            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyIn = new InstructionNode();
                copyIn->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyIn->type = ASSIGN_F;
                    copyIn->assign_f_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_f_inst.operand1_index = callerSlot;
                    copyIn->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyIn->type = ASSIGN_D;
                    copyIn->assign_d_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_d_inst.operand1_index = callerSlot;
                    copyIn->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyIn->type = ASSIGN;
                    copyIn->assign_inst.left_hand_side_index = selfSlot;
                    copyIn->assign_inst.operand1_index = callerSlot;
                    copyIn->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyIn);
            }

            struct InstructionNode* callNode = new InstructionNode();
            callNode->line_no = token.line_no;
            callNode->type = CALL;
            callNode->call_inst.function_head = classMethodTable[className]["init"];
            callNode->call_inst.ret_val_index = functionReturnIndex[fullName];
            callNode->call_inst.ret_type = functionReturnType.count(fullName) ? functionReturnType[fullName] : TYPE_UNKNOWN;
            callNode->call_inst.func_slot_base = 0;
            callNode->call_inst.func_slot_count = 0;
            callNode->call_inst.num_params = 0;
            callNode->call_inst.param_slots = nullptr;
            callNode->call_inst.arg_val_slots = nullptr;
            callNode->call_inst.param_types = nullptr;
            callNode->call_inst.arg_types = nullptr;
            callNode->call_inst.all_ret_slots = nullptr;
            callNode->call_inst.num_ret_slots = 0;
            append(head, tracker, callNode);

            for (auto& kv : selfSlots)
            {
                const string& fieldName = kv.first;
                int selfSlot = kv.second;
                if (!classFieldSlots[varName].count(fieldName))
                {
                    continue;
                }
                int callerSlot = classFieldSlots[varName][fieldName];
                VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
                struct InstructionNode* copyBack = new InstructionNode();
                copyBack->line_no = token.line_no;
                if (fType == TYPE_FLOAT)
                {
                    copyBack->type = ASSIGN_F;
                    copyBack->assign_f_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_f_inst.operand1_index = selfSlot;
                    copyBack->assign_f_inst.op = OPERATOR_NONE;
                }
                else if (fType == TYPE_DOUBLE)
                {
                    copyBack->type = ASSIGN_D;
                    copyBack->assign_d_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_d_inst.operand1_index = selfSlot;
                    copyBack->assign_d_inst.op = OPERATOR_NONE;
                }
                else
                {
                    copyBack->type = ASSIGN;
                    copyBack->assign_inst.left_hand_side_index = callerSlot;
                    copyBack->assign_inst.operand1_index = selfSlot;
                    copyBack->assign_inst.op = OPERATOR_NONE;
                }
                append(head, tracker, copyBack);
            }
        }
    }
    // else token = ';'
}

int parse_method_call(const string& varName, const string& methodName, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    // ON ENTRY: token = '('
    string className = varClassType[varName];

    // walk inheritance chain to find the method (supports inherited + overridden methods)
    string lookupClass = className;
    while (true)
    {
        if (classMethodTable.count(lookupClass) && classMethodTable[lookupClass].count(methodName))
        {
            break;
        }
        if (classTable.count(lookupClass) && !classTable[lookupClass].parent.empty())
        {
            lookupClass = classTable[lookupClass].parent;
        }
        else
        {
            report_error(token.line_no, "class '" + className + "' has no method '" + methodName + "'");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            {
                token = lexer.GetToken();
            }
            lastExprType = TYPE_UNKNOWN;
            return 0;
        }
    }

    token = lexer.GetToken();  // past '('
    // parse arguments
    vector<int>     argIndices;
    vector<VarType> argTypes;
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
    {
        int argIdx = parse_expression(head, tracker);
        argIndices.push_back(argIdx);
        argTypes.push_back(lastExprType);
        if (token.token_type == COMMA) token = lexer.GetToken();
    }
    token = lexer.GetToken();  // past ')'

    // Copy caller's field values into the method's self slots (pass self by copy)
    map<string, int>& selfSlots = classMethodSelfSlots[lookupClass][methodName];
    map<string, VarType>& selfTypes = classMethodSelfTypes[lookupClass][methodName];
    map<string, int>& arrSizes = classMethodArraySizes[lookupClass][methodName];
    for (auto& kv : selfSlots)
    {
        const string& fieldName = kv.first;
        int selfSlot = kv.second;
        if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(fieldName)) continue;
        int callerSlot = classFieldSlots[varName][fieldName];
        // Array field: store the caller's base index as a literal pointer
        if (arrSizes.count(fieldName))
        {
            int ptrSlot = alloc_slot();
            mem[ptrSlot] = callerSlot;
            struct InstructionNode* copyIn = new InstructionNode();
            copyIn->line_no = token.line_no;
            copyIn->type = ASSIGN;
            copyIn->assign_inst.left_hand_side_index = selfSlot;
            copyIn->assign_inst.operand1_index       = ptrSlot;
            copyIn->assign_inst.op                   = OPERATOR_NONE;
            append(head, tracker, copyIn);
            continue;
        }
        VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
        struct InstructionNode* copyIn = new InstructionNode();
        copyIn->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            copyIn->type = ASSIGN_F;
            copyIn->assign_f_inst.left_hand_side_index = selfSlot;
            copyIn->assign_f_inst.operand1_index = callerSlot;
            copyIn->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
        {
            copyIn->type = ASSIGN_D;
            copyIn->assign_d_inst.left_hand_side_index = selfSlot;
            copyIn->assign_d_inst.operand1_index = callerSlot;
            copyIn->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            copyIn->type = ASSIGN;
            copyIn->assign_inst.left_hand_side_index = selfSlot;
            copyIn->assign_inst.operand1_index = callerSlot;
            copyIn->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, copyIn);
    }

    // Emit CALL node
    string fullName = lookupClass + "::" + methodName;
    vector<string>& params = classMethodParams[lookupClass][methodName];

    struct InstructionNode* callNode = new InstructionNode();
    callNode->line_no = token.line_no;
    callNode->type = CALL;
    callNode->call_inst.function_head   = classMethodTable[lookupClass][methodName];
    callNode->call_inst.ret_val_index   = functionReturnIndex[fullName];
    callNode->call_inst.ret_type = (classMethodReturnType.count(lookupClass) && classMethodReturnType[lookupClass].count(methodName))
                                    ? classMethodReturnType[lookupClass][methodName] : TYPE_UNKNOWN;
    callNode->call_inst.func_slot_base  = 0;
    callNode->call_inst.func_slot_count = 0;
    callNode->call_inst.num_params      = (int)params.size();
    callNode->call_inst.all_ret_slots = nullptr;
    callNode->call_inst.num_ret_slots = 0;
    callNode->call_inst.ret_slot_types = nullptr;
    if (!params.empty())
    {
        int* pSlots = new int[params.size()];
        int* aSlots = new int[params.size()];
        vector<int>& storedParamSlots = classMethodParamSlots[lookupClass][methodName];
        for (int i = 0; i < (int)params.size(); i++)
        {
            pSlots[i] = (i < (int)storedParamSlots.size()) ? storedParamSlots[i] : 0;
            aSlots[i] = (i < (int)argIndices.size()) ? argIndices[i] : 0;
        }
        VarType* ptypes = new VarType[params.size()];
        vector<VarType>& storedParamTypes = classMethodParamTypes[lookupClass][methodName];
        for (int i = 0; i < (int)params.size(); i++)
            ptypes[i] = (i < (int)storedParamTypes.size()) ? storedParamTypes[i] : TYPE_UNKNOWN;
        for (int i = 0; i < (int)argTypes.size() && i < (int)storedParamTypes.size(); i++)
        {
            if (!types_compatible(argTypes[i], storedParamTypes[i]))
                report_error(token.line_no, "argument " + to_string(i + 1) + " of '" + lookupClass + "." + methodName +
                             "' expects " + typeToString(storedParamTypes[i]) + " but got " + typeToString(argTypes[i]));
        }

        VarType* atypes = new VarType[params.size()];
        for (int i = 0; i < (int)params.size(); i++)  // params.size(): the array's length
            atypes[i] = (i < (int)argTypes.size()) ? argTypes[i] : TYPE_UNKNOWN;

        callNode->call_inst.param_types   = ptypes;
        callNode->call_inst.arg_types     = atypes;
        callNode->call_inst.param_slots   = pSlots;
        callNode->call_inst.arg_val_slots = aSlots;
    }
    else
    {
        callNode->call_inst.param_slots   = nullptr;
        callNode->call_inst.arg_val_slots = nullptr;
    }
    append(head, tracker, callNode);

    // Copy self slots back to caller's field slots (method may mutate self)
    for (auto& kv : selfSlots)
    {
        const string& fieldName = kv.first;
        int selfSlot = kv.second;
        if (!classFieldSlots.count(varName) || !classFieldSlots[varName].count(fieldName)) continue;
        // Array fields write directly to caller via dynamic_base — no copy-back needed
        if (arrSizes.count(fieldName)) continue;
        int callerSlot = classFieldSlots[varName][fieldName];
        VarType fType = selfTypes.count(fieldName) ? selfTypes[fieldName] : TYPE_INT;
        struct InstructionNode* copyBack = new InstructionNode();
        copyBack->line_no = token.line_no;
        if (fType == TYPE_FLOAT)
        {
            copyBack->type = ASSIGN_F;
            copyBack->assign_f_inst.left_hand_side_index = callerSlot;
            copyBack->assign_f_inst.operand1_index = selfSlot;
            copyBack->assign_f_inst.op = OPERATOR_NONE;
        }
        else if (fType == TYPE_DOUBLE)
        {
            copyBack->type = ASSIGN_D;
            copyBack->assign_d_inst.left_hand_side_index = callerSlot;
            copyBack->assign_d_inst.operand1_index = selfSlot;
            copyBack->assign_d_inst.op = OPERATOR_NONE;
        }
        else
        {
            copyBack->type = ASSIGN;
            copyBack->assign_inst.left_hand_side_index = callerSlot;
            copyBack->assign_inst.operand1_index = selfSlot;
            copyBack->assign_inst.op = OPERATOR_NONE;
        }
        append(head, tracker, copyBack);
    }

    // Copy return value to a fresh slot
    VarType mRetType = (classMethodReturnType.count(lookupClass) && classMethodReturnType[lookupClass].count(methodName))
                       ? classMethodReturnType[lookupClass][methodName] : TYPE_UNKNOWN;
    int freshSlot = (mRetType == TYPE_FLOAT) ? alloc_float_slot() : 
                    (mRetType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_temp();
    struct InstructionNode* copyRet = new InstructionNode();
    copyRet->line_no = token.line_no;
    if (mRetType == TYPE_FLOAT)
    {
        copyRet->type = ASSIGN_F;
        copyRet->assign_f_inst.left_hand_side_index = freshSlot;
        copyRet->assign_f_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_f_inst.op = OPERATOR_NONE;
    }
    else if (mRetType == TYPE_DOUBLE)
    {
        copyRet->type = ASSIGN_D;
        copyRet->assign_d_inst.left_hand_side_index = freshSlot;
        copyRet->assign_d_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_d_inst.op = OPERATOR_NONE;
    }
    else
    {
        copyRet->type = ASSIGN;
        copyRet->assign_inst.left_hand_side_index = freshSlot;
        copyRet->assign_inst.operand1_index = functionReturnIndex[fullName];
        copyRet->assign_inst.op = OPERATOR_NONE;
    }
    append(head, tracker, copyRet);
    lastExprType = mRetType;  // the call's value has the method's declared return type
    return freshSlot;
}
