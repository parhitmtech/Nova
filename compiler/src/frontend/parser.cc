// Recursive-descent parser entry points: function definitions, REPL input, whole-program parse.
// The rest of the parser is in parser_*.cc (see parser_internal.h).
#include "parser_internal.h"

bool currentFuncReturnPending = false;
bool currentFuncSelfCalled = false;

// Generic functions: def f(x, k) with a parameter that has no type. The body is compiled once per combination
// of argument types f is called with, each copy named like "f<double, int>" (as C++ compiles a template).
struct GenericFunction
{
    int position;  // lexer position of the function's name, where its definition is read again
    int paramCount;
};
static map<string, GenericFunction> genericFunctions;
static string pendingInstanceName;  // set by instantiate_generic_function for the definition ...
static vector<VarType> pendingInstanceTypes;  // ... it is about to compile

bool is_generic_function(const string& name) { return genericFunctions.count(name) > 0; }
int generic_parameter_count(const string& name) { return genericFunctions[name].paramCount; }

// With token on a function's name: the number of parameters, and whether one of them has no type.
static bool has_untyped_parameter(int& count)
{
    count = 0;
    bool untyped = false, afterType = false;
    for (int k = 2; ;k++)  // peek(1) is '('
    {
        Token t = lexer.peek(k);
        if (t.token_type == RPAREN || t.token_type == END_OF_FILE) break;
        if (t.token_type == ID)
        {
            count++;
            if (!afterType) untyped = true;
        }
        afterType = (type_of_keyword(t.token_type) != TYPE_UNKNOWN);
    }
    return untyped;
}

// The first 'return value' of a function without '-> type' (currentFuncReturnPending): that value's type becomes
// the function's return type, and the return slot moves to the matching slot space (fmem / dmem for float / double).
void infer_return_type(const vector<VarType>& types, int line)
{
    currentFuncReturnPending = false;
    string name = currentFunctionLabel;
    VarType rt = types.empty() ? TYPE_UNKNOWN : types[0];
    if (types.size() > 1)
    {
        report_error(line, "function '" + name + "' returns several values: write their types after '->'");
    }
    else if (currentFuncSelfCalled && (rt == TYPE_FLOAT || rt == TYPE_DOUBLE || rt == TYPE_TENSOR || rt == TYPE_STRING))
    {
        report_error(line, "function '" + name + "' calls itself before its first return, so its return type must be "
                           "written: add '-> " + typeToString(rt) + "'");
    }
    if (rt == TYPE_FLOAT) currentFuncRetIdx = alloc_float_slot();
    else if (rt == TYPE_DOUBLE) currentFuncRetIdx = alloc_double_slot();
    currentFuncRetType = rt;
    currentFuncRetSlots = {currentFuncRetIdx};
    currentFuncRetTypes = {rt};
    functionReturnTypesList[name] = currentFuncRetTypes;
    functionReturnSlots[name] = currentFuncRetSlots;
    functionReturnType[name] = rt;
    functionReturnIndex[name] = currentFuncRetIdx;
}

void parse_function_definition()
{
    int defPosition = lexer.GetPosition();  // the name: a generic function's definition is read again from here
    token = lexer.GetToken();
    string funcName = token.lexeme;
    int funcLine = token.line_no;

    // Compiling one copy of a generic function: its name and the types of its untyped parameters.
    bool instance = !pendingInstanceName.empty();
    vector<VarType> instanceTypes = pendingInstanceTypes;
    if (instance)
    {
        funcName = pendingInstanceName;
        pendingInstanceName.clear();
    }
    else
    {
        if (functionTable.count(funcName) || genericFunctions.count(funcName))
        {
            report_error(funcLine, "duplicate function definition '" + funcName + "'");
        }
        int paramCount = 0;
        if (has_untyped_parameter(paramCount))
        {
            // Generic: remember where it is and skip the body. Each call compiles the copy it needs.
            genericFunctions[funcName] = {defPosition, paramCount};
            while (token.token_type != LBRACE && token.token_type != END_OF_FILE) token = lexer.GetToken();
            int depth = 0;
            while (token.token_type != END_OF_FILE)
            {
                if (token.token_type == LBRACE) depth++;
                if (token.token_type == RBRACE && --depth == 0) break;
                token = lexer.GetToken();
            }
            return;  // token is on the body's '}', as after a compiled definition
        }
    }

    int slotBase = next_available;
    vector<string>  params;
    vector<VarType> paramTypes;

    // The parameters and everything the body declares are gone after the function (all of the name tables, 
    // so an object or array declared in one function does not clash with one in the next)
    enter_block();

    token = lexer.GetToken(); token = lexer.GetToken();
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE) {
        VarType paramType = TYPE_UNKNOWN;
        if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) { paramType = type_of_keyword(token.token_type); token = lexer.GetToken(); }
        if (token.token_type == ID) 
        {
            if (paramType == TYPE_UNKNOWN && instance && params.size() < instanceTypes.size())
            {
                paramType = instanceTypes[params.size()];  // an untyped parameter takes its argument's type
            }
            params.push_back(token.lexeme);
            paramTypes.push_back(paramType);
            int pSlot = (paramType == TYPE_FLOAT) ? alloc_float_slot() :
                        (paramType == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            if (paramType == TYPE_FLOAT) floatSymbolTable[token.lexeme]  = pSlot;
            else if (paramType == TYPE_DOUBLE) doubleSymbolTable[token.lexeme] = pSlot;
            else symbolTable[token.lexeme] = pSlot;
            typeTable[token.lexeme]   = paramType;
        }
        token = lexer.GetToken();
        if (token.token_type == COMMA) token = lexer.GetToken();
    }

    functionParams[funcName]     = params;
    functionParamTypes[funcName] = paramTypes;

    // persist param slots before symbol table is restored at end of body
    {
        vector<int> pslots;
        for (int i = 0;i < (int)params.size();i++)
        {
            if (paramTypes[i] == TYPE_FLOAT) pslots.push_back(floatSymbolTable[params[i]]);
            else if (paramTypes[i] == TYPE_DOUBLE) pslots.push_back(doubleSymbolTable[params[i]]);
            else pslots.push_back(symbolTable[params[i]]); 
        }
        functionParamSlots[funcName] = pslots;
    }

    token = lexer.GetToken();  // ARROW or '{'
    bool returnTypeKnown = (token.token_type == ARROW);  // without '-> type', the first 'return value' sets it 
                                                         // (infer_return_type)
    vector<VarType> retTypes;
    vector<int> retSlots;
    if (token.token_type == ARROW) 
    {
        token = lexer.GetToken();  // first return type
        while (true)
        {
            VarType rt = TYPE_UNKNOWN;
            if (type_of_keyword(token.token_type) != TYPE_UNKNOWN) rt = type_of_keyword(token.token_type);
            else report_error(token.line_no, "expected type after '->' or ','");
            retTypes.push_back(rt);
            int rs = (rt == TYPE_FLOAT) ? alloc_float_slot() : 
                     (rt == TYPE_DOUBLE) ? alloc_double_slot() : alloc_slot();
            retSlots.push_back(rs);
            token = lexer.GetToken();  // ',' or '{'
            if (token.token_type != COMMA) break;
            token = lexer.GetToken();  // next return type
        }
        // token = '{'
    }
    else
    {
        retTypes.push_back(TYPE_UNKNOWN);
        retSlots.push_back(alloc_slot());
    }
    functionReturnTypesList[funcName] = retTypes;
    functionReturnSlots[funcName] = retSlots;
    functionReturnType[funcName] = retTypes[0];  // backward compatible
    functionReturnIndex[funcName] = retSlots[0];  // backward compatible
    int retIdx = retSlots[0];
    VarType retType = retTypes[0];
    token = lexer.GetToken();

    struct InstructionNode* head = nullptr, *tracker = nullptr;
    insideFunction     = true;
    currentFunctionLabel = funcName;
    currentFuncRetIdx  = retSlots[0];
    currentFuncRetType = retTypes[0];
    currentFuncRetSlots = retSlots;
    currentFuncRetTypes = retTypes;
    currentFuncReturnPending = !returnTypeKnown;
    currentFuncSelfCalled = false;      
    functionTable[funcName] = nullptr;

    while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
    {
        parse_statement(head, tracker);  // a 'return' too (see parse_statement)
        token = lexer.GetToken();
    }

    // The return slot and type, as the body left them (a function without '-> type' gets them at its first return)
    retIdx = currentFuncRetIdx;
    retType = currentFuncRetType;

    insideFunction = false; 
    currentFunctionLabel.clear();
    currentFuncReturnPending = false;
    currentFuncRetIdx = -1; 
    currentFuncRetType = TYPE_UNKNOWN;
    currentFuncRetSlots.clear();
    currentFuncRetTypes.clear();

    exit_block();  // remove the parameters and locals

    struct InstructionNode* fallbackRet = new InstructionNode();
    fallbackRet->line_no = token.line_no;
    fallbackRet->type = RET; 
    fallbackRet->ret_inst.ret_val_index = retIdx;
    fallbackRet->ret_inst.ret_type = retType;
    append(head, tracker, fallbackRet);
    functionTable[funcName] = head;

    int slotCount = next_available - slotBase;
    functionSlotRange[funcName] = {slotBase, slotCount};

    struct InstructionNode* fixNode = head;
    while (fixNode != nullptr) 
    {
        if (fixNode->type == CALL && fixNode->call_inst.function_head == nullptr) 
        {
            fixNode->call_inst.function_head   = head;
            fixNode->call_inst.func_slot_base  = slotBase;
            fixNode->call_inst.func_slot_count = slotCount;
        }
        fixNode = fixNode->next;
    }
}

// The copy of generic function `name` for these argument types, compiled the first time a call needs it.
// Returns the copy's name, e.g. "scale<double, double>".
string instantiate_generic_function(const string& name, const vector<VarType>& argTypes)
{
    string copyName = name + "<";
    for (int i = 0; i < (int)argTypes.size(); i++) copyName += (i ? ", " : "") + typeToString(argTypes[i]);
    copyName += ">";
    if (functionTable.count(copyName)) return copyName;  // compiled already, or being compiled (a recursive call)

    // The copy is compiled in the middle of the caller: set the caller's state aside and bring it back after.
    Token savedToken = token;
    int savedPosition = lexer.GetPosition();
    bool savedInside = insideFunction;
    string savedLabel = currentFunctionLabel;
    int savedRetIdx = currentFuncRetIdx;
    VarType savedRetType = currentFuncRetType;
    vector<int> savedRetSlots = currentFuncRetSlots;
    vector<VarType> savedRetTypes = currentFuncRetTypes;
    bool savedPending = currentFuncReturnPending, savedSelfCalled = currentFuncSelfCalled;
    string savedSelfClass = currentSelfClassName;  // a generic function is not a method, even if called from one
    map<string, int> savedSelfSlots = currentSelfFieldSlots;
    map<string, VarType> savedSelfTypes = currentSelfFieldTypes;
    map<string, int> savedSelfArraySizes = currentSelfFieldArraySizes;
    currentSelfClassName.clear();
    currentSelfFieldSlots.clear();
    currentSelfFieldTypes.clear();
    currentSelfFieldArraySizes.clear();
    set_variables_aside();
    set_loops_aside();

    pendingInstanceName = copyName;
    pendingInstanceTypes = argTypes;
    lexer.SetPosition(genericFunctions[name].position);
    parse_function_definition();

    restore_loops();
    restore_variables();
    currentSelfClassName = savedSelfClass;
    currentSelfFieldSlots = savedSelfSlots;
    currentSelfFieldTypes = savedSelfTypes;
    currentSelfFieldArraySizes = savedSelfArraySizes;
    currentFuncReturnPending = savedPending;
    currentFuncSelfCalled = savedSelfCalled;
    currentFuncRetTypes = savedRetTypes;
    currentFuncRetSlots = savedRetSlots;
    currentFuncRetType = savedRetType;
    currentFuncRetIdx = savedRetIdx;
    currentFunctionLabel = savedLabel;
    insideFunction = savedInside;
    lexer.SetPosition(savedPosition);
    token = savedToken;
    return copyName;
}

// ─────────────────────────────────────────────────────────────────────────────
//  parse_repl_input  —  called by run_repl() in compiler.cc for each REPL entry
//
//  Reinitializes the lexer from the accumulated string, then parses either:
//    - one or more function definitions (def ...)
//    - a braced block  { statements }
//    - bare statements (no braces) until EOF
//
//  Symbol table, function table, and mem[] persist across calls so variables
//  and functions defined earlier in the REPL session remain available.
// ─────────────────────────────────────────────────────────────────────────────
struct InstructionNode* parse_repl_input(const std::string& s)
{
    // clear only per-parse error state — do NOT clear symbolTable / functionTable
    errorList.clear();

    lexer.ReinitializeFromString(s + "\n");

    token = lexer.GetToken();

    // function definitions
    while (token.token_type == STRUCT || token.token_type == DEF || token.token_type == CLASS)
    {
        if (token.token_type == STRUCT) parse_struct_definition();
        else if (token.token_type == CLASS) parse_class_definition();
        else parse_function_definition();
        token = lexer.GetToken();
    }

    struct InstructionNode* head    = nullptr;
    struct InstructionNode* tracker = nullptr;

    if (token.token_type == LBRACE)
    {
        // braced block: { statements }
        token = lexer.GetToken();
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
    }
    else if (token.token_type != END_OF_FILE)
    {
        // bare statement(s) without braces
        while (token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
    }

    if (!errorList.empty())
    {
        for (const string& err : errorList)
            cerr << err << "\n";
        // return a no-op so the REPL loop can continue
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        return noOpNode;
    }

    if (head == nullptr)
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP;
        noOpNode->next = nullptr;
        head = noOpNode;
    }

    return head;
}

struct InstructionNode* parse_generate_intermediate_representation()
{
    // Report unclosed / mismatched brackets (e.g. a truncated file) before parsing, with their line.
    if (!check_balanced_parens())
    {
        for (const string& err : errorList) fprintf(stderr, "%s\n", err.c_str());
        exit(1);
    }
    token = lexer.GetToken();
    while (token.token_type == STRUCT || token.token_type == DEF || token.token_type == CLASS)
    {
        if (token.token_type == STRUCT) parse_struct_definition();
        else if (token.token_type == CLASS) parse_class_definition();
        else parse_function_definition();
        token = lexer.GetToken();
    }

    // The entry point: one 'ignite { ... }' block after the definitions (Nova is one-pass, so
    // everything it calls must already be defined). A file with only 'def main()' gets
    // "ignite { main() ; }" appended by the lexer.
    struct InstructionNode* head = nullptr, *tracker = nullptr;
    if (token.token_type == END_OF_FILE)
    {
        report_error(token.line_no, "no entry point: add an 'ignite { ... }' block at the end of the file (or define 'def main()')");
    }
    else if (token.token_type != IGNITE)
    {
        report_error(token.line_no, "expected a definition or the 'ignite' block, got '" + token.lexeme + "'");
    }
    else
    {
        token = lexer.GetToken();  // consume 'ignite' -> '{'
        if (token.token_type != LBRACE)
            report_error(token.line_no, "expected '{' after 'ignite'");
        token = lexer.GetToken();  // advance to first statement in body
        while (token.token_type != RBRACE && token.token_type != END_OF_FILE)
        {
            parse_statement(head, tracker);
            token = lexer.GetToken();
        }
        token = lexer.GetToken();  // past the closing '}'
        if (token.token_type == IGNITE)
            report_error(token.line_no, "a file can have only one 'ignite' block");
        else if (token.token_type != END_OF_FILE)
            report_error(token.line_no, "the 'ignite' block must come last: move '" + token.lexeme + "' above it");
    }
    
    if (head == nullptr) 
    {
        struct InstructionNode* noOpNode = new InstructionNode();
        noOpNode->line_no = token.line_no;
        noOpNode->type = NOOP; head = tracker = noOpNode;
    }
    if (!errorList.empty()) 
    {
        for (const string& err : errorList) cerr << err << "\n";
        exit(1);
    }
    return head;
}
