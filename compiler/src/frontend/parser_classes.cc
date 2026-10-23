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

    if (name_is_declared(varName))
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
        // Point p = Point(3, 4): one value per field, in order
        token = lexer.GetToken();  // the struct name
        if (token.token_type != ID || token.lexeme != structTypeName || lexer.peek(1).token_type != LPAREN)
        {
            report_error(varLine, "a " + structTypeName + " is created with " + structTypeName + "(...), one value per field");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // '('
        token = lexer.GetToken();

        int given = 0;  // values read so far
        for (int i = 0;i < (int)def.fields.size() && token.token_type != RPAREN;i++, given++)
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
        if (token.token_type != RPAREN || given != (int)def.fields.size())
        {
            report_error(varLine, structTypeName + " has " + to_string(def.fields.size()) + " field(s); give one value for each");
            while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
            return;
        }
        token = lexer.GetToken();  // past ')' -> ';'
    }
    // else token = ';'
}

// Generic classes: class Box<T>. Box<int> is a copy of the class compiled with every T in its tokens replaced by
// the int token, named "Box<int>" (as C++ compiles a class template), the first time a declaration needs it.
struct GenericClass
{
    int start, end;  // the class's tokens, from its name to just past its closing '}'
    vector<string> typeParams;  // T, U, ...
};

static map<string, GenericClass> genericClasses;
static string pendingClassCopyName;  // set by instantiate_generic_class for the definition it is about to compile

bool is_generic_class(const string& name) { return genericClasses.count(name) > 0; }

// A type argument's tokens as a type name ("int", "vector<int>", "map<string, int>"), or "" when they are not a
// type a field or parameter can have: a built-in type, or a container of built-in types.
static string type_argument_name(const vector<Token>& arg)
{
    if (arg.size() == 1 && type_of_keyword(arg[0].token_type) != TYPE_UNKNOWN) return arg[0].lexeme;
    ContainerInfo info;
    bool vec = arg.size() == 4 && arg[0].lexeme == "vector" && arg[1].token_type == LESS && arg[3].token_type == GREATER;
    bool map = arg.size() == 6 && arg[0].lexeme == "map" && arg[1].token_type == LESS && arg[3].token_type == COMMA &&
               arg[5].token_type == GREATER;
    if (!vec && !map) return "";
    info.isMap = map;
    info.elem = type_of_keyword(arg[vec ? 2 : 4].token_type);
    if (info.elem == TYPE_UNKNOWN) return "";
    if (map)
    {
        info.key = type_of_keyword(arg[2].token_type);
        if (info.key != TYPE_INT && info.key != TYPE_STRING) return "";
    }
    return container_type_name(info);
}

// A type argument's text for an error message.
static string spelled(const vector<Token>& arg)
{
    string s;
    for (const Token& t : arg)
    {
        s += (t.token_type == LESS) ? "<" : (t.token_type == GREATER) ? ">" : (t.token_type == COMMA) ? ", " : t.lexeme;
    }
    return s;
}

// The copy of generic class `name` for these type arguments (each a token sequence: int, vector<int>, ...).
// Returns the copy's name, e.g. "Box<vector<int>>", or "" after an error.
static string instantiate_generic_class(const string& name, const vector<vector<Token>>& args, int line)
{
    GenericClass& g = genericClasses[name];
    if (args.size() != g.typeParams.size())
    {
        report_error(line, "class '" + name + "' takes " + to_string(g.typeParams.size()) + " type argument(s) but got " +
                           to_string(args.size()));
        return "";
    }
    string copyName = name + "<";
    for (int i = 0; i < (int)args.size(); i++)
    {
        string argName = type_argument_name(args[i]);
        if (argName.empty())
        {
            // An object cannot be a field's or parameter's value yet
            report_error(line, "'" + spelled(args[i]) + "' cannot be a type argument: use int, float, double, bool, string, "
                               "Tensor, vector<T> or map<K, V>");
            return "";
        }
        copyName += (i ? ", " : "") + argName;
    }
    copyName += ">";
    if (classTable.count(copyName)) return copyName;

    // The class's tokens with each type parameter replaced by its argument, appended after the program's tokens
    // and compiled from there; the program's own tokens never change.
    vector<Token> copy;
    int headerEnd = g.start;  // the '>' closing `class Box<T>`, which names the parameters and stays as it is
    while (headerEnd < g.end && lexer.TokenAt(headerEnd).token_type != GREATER) headerEnd++;
    for (int i = g.start; i < g.end; i++)
    {
        Token t = lexer.TokenAt(i);
        int k = -1;
        if (t.token_type == ID && i > headerEnd)
            for (int j = 0; j < (int)g.typeParams.size(); j++)
                if (t.lexeme == g.typeParams[j]) k = j;
        if (k < 0) { copy.push_back(t); continue; }
        for (Token a : args[k])
        {
            a.line_no = t.line_no;  // errors still point into the class
            copy.push_back(a);
        }
    }
    int at = lexer.AppendTokens(copy);
    pendingClassCopyName = copyName;
    compile_with_caller_aside(at, parse_class_definition);
    lexer.TruncateTokens(at);
    return copyName;
}

string parse_generic_class_type()
{
    string name = token.lexeme;
    int line = token.line_no;
    // The type arguments, split at top-level commas: Pair<map<string, int>, int> has two.
    vector<vector<Token>> args(1);
    int depth = 0;
    token = lexer.GetToken();  // '<'
    token = lexer.GetToken();  // first token of the first argument
    while (token.token_type != END_OF_FILE)
    {
        if (token.token_type == GREATER && depth == 0) break;
        if (token.token_type == LESS) depth++;
        if (token.token_type == GREATER) depth--;
        if (token.token_type == COMMA && depth == 0) args.emplace_back();
        else args.back().push_back(token);
        token = lexer.GetToken();
    }
    return instantiate_generic_class(name, args, line);  // token is on the closing '>'
}

void parse_class_definition()
{
    int namePosition = lexer.GetPosition();
    token = lexer.GetToken();  // class name
    string className = token.lexeme;
    int classLine = token.line_no;

    // Compiling one copy of a generic class: its name, e.g. "Box<int>".
    bool instance = !pendingClassCopyName.empty();
    if (instance)
    {
        className = pendingClassCopyName;
        pendingClassCopyName.clear();
    }
    else if (classTable.count(className) || genericClasses.count(className))
    {
        report_error(classLine, "duplicate class definition '" + className + "'");
    }

    if (lexer.peek(1).token_type == LESS)
    {
        token = lexer.GetToken();  // '<'
        vector<string> typeParams;
        token = lexer.GetToken();
        while (token.token_type != GREATER && token.token_type != END_OF_FILE)
        {
            if (token.token_type != COMMA) typeParams.push_back(token.lexeme);
            token = lexer.GetToken();
        }
        if (!instance)
        {
            // Generic: skip the body; each Box<...> a declaration uses compiles its copy.
            while (token.token_type != LBRACE && token.token_type != END_OF_FILE) token = lexer.GetToken();
            int depth = 0;
            while (token.token_type != END_OF_FILE)
            {
                if (token.token_type == LBRACE) depth++;
                if (token.token_type == RBRACE && --depth == 0) break;
                token = lexer.GetToken();
            }
            genericClasses[className] = {namePosition, lexer.GetPosition(), typeParams};
            return;  // token is on the class's '}'
        }
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
            classFieldContainers[className] = classFieldContainers[parentName];
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
            vector<string> paramClasses;
            classTable[className] = def;  // the class names itselfin its methods' types: def add(Vec other) -> Vec

            // The parameters and everything the body declares are gone after the method (all of the name tables)
            enter_block();

            token = lexer.GetToken();  // '('
            token = lexer.GetToken();  // first param or ')'
            while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
            {
                VarType pType = TYPE_UNKNOWN;
                ContainerInfo pContainer;
                string pClass;
                if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) { pType = type_of_keyword(token.token_type); token = lexer.GetToken(); }
                else if (at_container_type() && parse_container_type(pContainer))  // vector<int> xs
                {
                    pType = pContainer.isMap ? TYPE_MAP : TYPE_VECTOR;
                    token = lexer.GetToken();  // past '>' to the name
                }
                else if (lexer.peek(1).token_type != COMMA && lexer.peek(1).token_type != RPAREN && 
                         parse_class_type_name(pClass))  // Point p
                {
                    pType = TYPE_CLASS;
                    token = lexer.GetToken();  // past the type to the name
                }
                if (token.token_type == ID)
                {
                    params.push_back(token.lexeme);
                    paramTypes.push_back(pType);
                    paramClasses.push_back(pClass);
                    if (pType == TYPE_CLASS) varClassType[token.lexeme] = pClass;
                    classMethodParamContainers[className][methodName].push_back(pContainer);
                    if (pType == TYPE_VECTOR || pType == TYPE_MAP) containerTable[token.lexeme] = pContainer;
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
            classMethodParamClasses[className][methodName] = paramClasses;

            token = lexer.GetToken();  // ARROW or '{'
            VarType retType = TYPE_UNKNOWN;
            ContainerInfo retContainer;
            string retClass;
            if (token.token_type == ARROW)
            {
                token = lexer.GetToken();
                if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) retType = type_of_keyword(token.token_type);
                else if (at_container_type() && parse_container_type(retContainer)) // -> vector<int>
                {
                    retType = retContainer.isMap ? TYPE_MAP : TYPE_VECTOR;
                    classMethodReturnContainer[className][methodName] = retContainer;
                }
                else if (parse_class_type_name(retClass))  // -> Point
                {
                    retType = TYPE_CLASS;
                    classMethodReturnClass[className][methodName] = retClass;
                }
                token = lexer.GetToken();  // '{'
            }
            classMethodReturnType[className][methodName] = retType;

            int retIdx = (retType == TYPE_FLOAT) ? alloc_float_slot() : 
                         (retType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            functionReturnIndex[fullName] = retIdx;

            // The object's handle arrives as a hidden first parameter: self.x is a field of that object.
            int selfHandle = alloc_slot();
            classMethodSelfHandle[className][methodName] = selfHandle;

            // save + set parser context
            bool savedInsideFunc = insideFunction;
            int savedRetIdx = currentFuncRetIdx;
            VarType savedRetType = currentFuncRetType;
            ContainerInfo savedRetContainer = currentFuncRetContainer;
            vector<int> savedRetSlots = currentFuncRetSlots;
            vector<VarType> savedRetTypes = currentFuncRetTypes;
            string savedFunctionLabel = currentFunctionLabel;
            string savedSelfClass = currentSelfClassName;
            int savedSelfHandle = currentSelfHandle;
            string savedRetClass = currentFuncRetClass;

            insideFunction = true;
            currentFuncRetIdx = retIdx;
            currentFuncRetType = retType;
            currentFuncRetContainer = retContainer;
            currentFuncRetClass = retClass;

            // A return nested in an if / loop goes through parse_statement, which writes these: without them
            // it wrote into whatever function was being compiled around this class (a generic copy's caller).
            currentFuncRetSlots = {retIdx};
            currentFuncRetTypes = {retType};
            currentFunctionLabel = className + "." + methodName;
            currentSelfClassName = className;
            currentSelfHandle = selfHandle;
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
                    if ((retType == TYPE_VECTOR || retType == TYPE_MAP) && lastExprType == retType && 
                         !same_container(lastExprType, lastExprContainer, retContainer))
                    {
                        report_error(token.line_no, "'" + currentFunctionLabel + "' returns " + container_type_name(retContainer) + 
                                                    ", not " + container_type_name(lastExprContainer));
                    }
                    if (retType == TYPE_CLASS && lastExprType == TYPE_CLASS && lastExprClass != retClass)
                    {
                        report_error(token.line_no, "'" + currentFunctionLabel + "' returns " + retClass + ", not " + lastExprClass);
                    }

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
            currentFuncRetContainer = savedRetContainer;
            currentFuncRetSlots = savedRetSlots;
            currentFuncRetTypes = savedRetTypes;
            currentFunctionLabel = savedFunctionLabel;
            currentSelfClassName = savedSelfClass;
            currentSelfHandle = savedSelfHandle;
            currentFuncRetClass = savedRetClass;

            exit_block();  // remove the parameters and locals

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
        else if (at_container_type())
        {
            // vector<int> ids: a container field; each object starts with a new, empty one
            ContainerInfo ci;
            if (!parse_container_type(ci))
            {
                while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE) token = lexer.GetToken();
                token = lexer.GetToken();
                continue;
            }
            token = lexer.GetToken();  // field name
            ClassFieldInfo fi;
            fi.name = token.lexeme;
            fi.type = ci.isMap ? TYPE_MAP : TYPE_VECTOR;
            fi.struct_type = "";
            fi.array_size = 0;
            def.fields.push_back(fi);
            classFieldContainers[className][fi.name] = ci;
            token = lexer.GetToken();  // ';'
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
                if (classFieldContainers[nestedClass].count(nf.name))
                {
                    classFieldContainers[className][fi.name] = classFieldContainers[nestedClass][nf.name];
                }
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

    if (name_is_declared(varName))
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

    varClassType[varName] = className;
    typeTable[varName] = TYPE_CLASS;
    symbolTable[varName] = alloc_slot();

    token = lexer.GetToken();  // '=', '(' or ';'
    if (token.token_type == EQUAL)
    {
        // Point p = Point(1, 2), Point p = q, Point p = make(): p refers to the value's object
        parse_object_value_into(symbolTable[varName], className, varName, varLine, head, tracker);
    }
    else if (token.token_type == LPAREN)
    {
        // Point p(1, 2): the same as Point p = Point(1, 2)
        int value = parse_constructor_expression(className, varLine, head, tracker);
        emit_object_store(symbolTable[varName], value, true, varLine, head, tracker);
    }
    else if (token.token_type == SEMICOLON)
    {
        // Point p: a new object, its init run if it takes no arguments
        int value = emit_object_new(className, varLine, head, tracker);
        if (classMethodTable.count(className) && classMethodTable[className].count("init") && classMethodParams[className]["init"].empty())
        {
            emit_method_call(className, "init", value, {}, {}, varLine, head, tracker);
        }
        emit_object_store(symbolTable[varName], value, true, varLine, head, tracker);
    }
    // token = ';'
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
    int argLine = token.line_no;
    CallArgs args = parse_call_arguments(head, tracker);
    vector<ContainerInfo>& wantContainers = classMethodParamContainers[lookupClass][methodName];
    vector<VarType>& wantTypes = classMethodParamTypes[lookupClass][methodName];
    vector<string>& wantClasses = classMethodParamClasses[lookupClass][methodName];
    string callee = "'" + className + "." + methodName + "'";
    for (int i = 0; i < (int)args.slots.size(); i++)
    {
        if (i < (int)wantContainers.size() && wantContainers[i].elem != TYPE_UNKNOWN && 
            !same_container(args.types[i], args.containers[i], wantContainers[i]))
        {
            report_error(argLine, "argument " + to_string(i + 1) + " of " + callee + " expects " + 
                                  container_type_name(wantContainers[i]) + " but got " + 
                                  value_type_name(args.types[i], args.containers[i], args.classes[i]));
        }
        else if (i < (int)wantTypes.size())
        {
            check_object_argument(args, i, wantTypes[i], i < (int)wantClasses.size() ? wantClasses[i] : "", callee, argLine);
        }
    }

    if (args.slots.size() != classMethodParams[lookupClass][methodName].size())
    {
        report_error(argLine, callee + " takes "+ to_string(classMethodParams[lookupClass][methodName].size()) + 
                              " argument(s) but got " + to_string(args.slots.size()));
    }
    int result = emit_method_call(lookupClass, methodName, symbolTable[varName], args.slots, args.types, argLine,
                                  head, tracker);
    release_object_temps(args.temps, argLine, head, tracker);
    return result;
}
