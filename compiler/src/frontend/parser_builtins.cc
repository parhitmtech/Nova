// Built-in calls: tensor_* operations, hf.* (HuggingFace) and sol.* (SOL cluster). Split out of parser.cc.
#include <cstring>
#include "parser_internal.h"
#include "tensor_ops.h"

static bool is_number_type(VarType t)
{
    return t == TYPE_INT || t == TYPE_FLOAT || t == TYPE_DOUBLE || t == TYPE_BOOL;
}

// One tensor built-in call: check each argument's kind against the op's signature, emit the TENSOR_CALL
// and set lastExprType to its result (Tensor, double, int or nothing for void ops). Used for calls
// (tensor_add(a, b)) and for operators and members (a + b, t[i, j], t.T). 'name' is what type errors call
// it, already quoted: "'tensor_add'" or "'+'". argNames[i], if present, names a plain-variable argument.
int emit_tensor_op(TensorOp op, const string& name, const vector<int>& args, const vector<VarType>& types,
                   const vector<string>& argNames, int line, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    int numArgs = (int)args.size();
    if (numArgs > 8)
    {
        report_error(line, name + " takes at most 8 arguments");
        numArgs = 8;
    }

    // Argument kinds: a Tensor where the op takes a tensor, a number where it takes a size, index, or scalar.
    {
        const char* sig = tensor_op_signature(op);
        int len = (int)strlen(sig);
        bool repeat = (len > 0 && sig[len - 1] == '*');
        int fixed = repeat ? len - 1 : len;
        for (int i = 0;i < numArgs; i++)
        {
            char want = (i < fixed) ? sig[i] : (repeat ? sig[fixed - 1] : 0);
            VarType t = types[i];
            if (want == 0 || t == TYPE_UNKNOWN) continue;
            string argName = (i < (int)argNames.size()) ? argNames[i] : "";
            string what = argName.empty() ? "got " + typeToString(t)
                                          : "but '" + argName + "' is " + typeToString(t);
            string where = "argument " + to_string(i + 1) + " of " + name;
            if (want == 'T' && t != TYPE_TENSOR)
            {
                report_error(line, where + " must be a Tensor, " + what);
            } 
            else if (want == 'n' && !is_number_type(t))
            {
                report_error(line, where + " must be a number, " + what);
            }
            else if (want == 's' && t != TYPE_STRING)
            {
                report_error(line, where + " must be a string, " + what);
            }
        }
    }

    int resultSlot = -1;
    bool isVoid = TENSOR_VOID_OPS.count(op) > 0;
    bool isDouble = TENSOR_DOUBLE_OPS.count(op) > 0;
    if (!isVoid) { resultSlot = isDouble ? alloc_double_slot() : alloc_slot(); }

    struct InstructionNode* node = new InstructionNode();
    node->line_no = line;
    node->type = TENSOR_CALL;
    node->tensor_call_inst.op = op;
    node->tensor_call_inst.result_slot = resultSlot;
    node->tensor_call_inst.num_args = numArgs;
    for (int i = 0;i < numArgs; i++)
    {
        node->tensor_call_inst.arg_slots[i] = args[i];
        node->tensor_call_inst.arg_types[i] = types[i];
    }
    append(head, tracker, node);

    bool isInt = TENSOR_INT_OPS.count(op) > 0;
    lastExprType = isVoid ? TYPE_UNKNOWN
                        : isDouble ? TYPE_DOUBLE
                        : isInt ? TYPE_INT
                        : TYPE_TENSOR;
    if (!isVoid && !isDouble && !isInt) g_tensorSlots.insert(resultSlot);
    return isVoid ? alloc_slot() : resultSlot;
} 

int parse_tensor_call(const string& name, struct InstructionNode*& head, struct InstructionNode*& tracker)
{
    TensorOp op = TENSOR_OP_MAP.at(name);
    token = lexer.GetToken();  // consume '(' → token is now first arg (or ')')

    // collect arguments
    int argSlots[8]; VarType argTypes[8]; string argNames[8]; int numArgs = 0;
    while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
    {
        if (numArgs == 8)
        {
            report_error(token.line_no, "'" + name + "' takes at most 8 arguments");
            while (token.token_type != RPAREN && token.token_type != END_OF_FILE) token = lexer.GetToken();
            break;
        }
        // a plain variable argument is named in type errors
        TokenType after = lexer.peek(1).token_type;
        argNames[numArgs] = (token.token_type == ID && (after == COMMA || after == RPAREN)) ? token.lexeme : "";
        argSlots[numArgs] = parse_expression(head, tracker);
        argTypes[numArgs] = lastExprType;
        numArgs++;
        if (token.token_type == COMMA) token = lexer.GetToken();  // skip ',' -> next arg
    }
    // token is now ')'
    token = lexer.GetToken();  // consume ')'

    // Arguments counts for the ops that take one argument per dimension.
    // Checked here so a wrong count is a compile error, not a garbage read.
    {
        int lo = -1, hi = -1;
        const int R = TENSOR_MAX_RANK;
        switch (op)
        {
            case TEN_ZEROS: case TEN_ONES: case TEN_RANDN: lo = 1; hi = R; break;
            case TEN_GET:                                  lo = 2, hi = R + 1; break;
            case TEN_SET:                                  lo = 3; hi = R + 2; break;
            case TEN_RESHAPE:                              lo = 2; hi = R + 1; break;
            default: break;
        }
        if (lo >= 0 && (numArgs < lo || numArgs > hi))
        {
            report_error(token.line_no, "'" + name + "' takes " + to_string(lo) + " to " + to_string(hi) + 
                         " arguments, got " + to_string(numArgs));
        }
    }

    vector<int> args(argSlots, argSlots + numArgs);
    vector<VarType> types(argTypes, argTypes + numArgs);
    vector<string> names(argNames, argNames + numArgs);
    return emit_tensor_op(op, "'" + name + "'", args, types, names, token.line_no, head, tracker);
}

// ── HuggingFace built-in calls: hf.infer / hf.generate / hf.classify / hf.set_token ──
int parse_hf_call(const string& method,
                          struct InstructionNode*& head,
                          struct InstructionNode*& tracker)
{
    // token is '(' on entry; leaves token on the character after ')'
    token = lexer.GetToken(); // consume '('

    HFOp op;
    if      (method == "infer")     op = HF_OP_INFER;
    else if (method == "generate")  op = HF_OP_GENERATE;
    else if (method == "classify")  op = HF_OP_CLASSIFY;
    else if (method == "set_token") op = HF_OP_SET_TOKEN;
    else if (method == "dataset")   op = HF_OP_DATASET;
    else {
        report_error(token.line_no, "unknown hf method: hf." + method + "()");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        lastExprType = TYPE_UNKNOWN;
        return alloc_slot();
    }

    // First arg: model name or API token string
    int model_slot = -1;
    if (token.token_type != RPAREN)
        model_slot = parse_expression(head, tracker);

    int input_slot = -1;
    int max_tokens = 100;

    if (op != HF_OP_SET_TOKEN) {
        if (token.token_type == COMMA) {
            token = lexer.GetToken(); // consume ','
            // Could be keyword arg: max_tokens=N  or a positional input expression
            if (token.token_type == ID && token.lexeme == "max_tokens"
                && lexer.peek(1).token_type == EQUAL) {
                token = lexer.GetToken(); // consume 'max_tokens'
                token = lexer.GetToken(); // consume '='
                if (token.token_type == NUM) {
                    max_tokens = stoi(token.lexeme);
                    token = lexer.GetToken(); // consume number
                }
            } else {
                input_slot = parse_expression(head, tracker);
            }
        }

        // Third arg (optional): max_tokens=N or positional number
        if (token.token_type == COMMA) {
            token = lexer.GetToken(); // consume ','
            if (token.token_type == ID && token.lexeme == "max_tokens"
                && lexer.peek(1).token_type == EQUAL) {
                token = lexer.GetToken(); // consume 'max_tokens'
                token = lexer.GetToken(); // consume '='
                if (token.token_type == NUM) {
                    max_tokens = stoi(token.lexeme);
                    token = lexer.GetToken(); // consume number
                }
            } else if (token.token_type == NUM) {
                max_tokens = stoi(token.lexeme);
                token = lexer.GetToken(); // consume number
            }
        }
    }

    if (token.token_type == RPAREN) token = lexer.GetToken(); // consume ')'

    bool isVoid    = (op == HF_OP_SET_TOKEN);
    int result_slot = isVoid ? -1 : alloc_slot();

    auto* node = new InstructionNode();
    node->line_no           = token.line_no;
    node->type              = HF_INFER;
    node->hf_inst.op         = op;
    node->hf_inst.model_slot = model_slot;
    node->hf_inst.input_slot = input_slot;
    node->hf_inst.result_slot= result_slot;
    node->hf_inst.max_tokens = max_tokens;
    node->next = nullptr;
    append(head, tracker, node);

    lastExprType = isVoid ? TYPE_UNKNOWN : TYPE_STRING;
    return isVoid ? alloc_slot() : result_slot;
}

// ── SOL cluster built-in calls ───────────────────────────────────────────────
int parse_sol_call(const string& method,
                          struct InstructionNode*& head,
                          struct InstructionNode*& tracker)
{
    // token is '(' on entry; leaves token on the character after ')'
    token = lexer.GetToken(); // consume '('

    SolOp op;
    if      (method == "finetune") op = SOL_OP_FINETUNE;
    else if (method == "wait")     op = SOL_OP_WAIT;
    else if (method == "predict")  op = SOL_OP_PREDICT;
    else if (method == "save")     op = SOL_OP_SAVE;
    else if (method == "set_key")  op = SOL_OP_SET_KEY;
    else {
        report_error(token.line_no, "unknown sol method: sol." + method + "()");
        while (token.token_type != SEMICOLON && token.token_type != END_OF_FILE)
            token = lexer.GetToken();
        lastExprType = TYPE_UNKNOWN;
        return alloc_slot();
    }

    // Shared fields with defaults
    int model_slot = -1, task_slot = -1, dataset_slot = -1;
    int job_slot_in = -1; // job handle input (for wait/predict/save)
    int input_slot  = -1; // for predict
    int path_slot   = -1; // for set_key / save
    int epochs = 3, batch_size = 16;
    float lr = 2e-5f;

    if (op == SOL_OP_FINETUNE)
    {
        // Keyword argument list: model=..., task=..., dataset=..., epochs=..., lr=..., batch_size=...
        while (token.token_type != RPAREN && token.token_type != END_OF_FILE)
        {
            if (token.token_type == ID && lexer.peek(1).token_type == EQUAL)
            {
                string kw = token.lexeme;
                token = lexer.GetToken(); // consume keyword name
                token = lexer.GetToken(); // consume '='
                if (kw == "model")
                    model_slot = parse_expression(head, tracker);
                else if (kw == "task")
                    task_slot = parse_expression(head, tracker);
                else if (kw == "dataset")
                    dataset_slot = parse_expression(head, tracker);
                else if (kw == "epochs" && token.token_type == NUM)
                    { epochs = stoi(token.lexeme); token = lexer.GetToken(); }
                else if (kw == "batch_size" && token.token_type == NUM)
                    { batch_size = stoi(token.lexeme); token = lexer.GetToken(); }
                else if (kw == "lr")
                    {
                        if (token.token_type == FLOAT_LITERAL || token.token_type == NUM)
                            { lr = stof(token.lexeme); token = lexer.GetToken(); }
                    }
                else
                    parse_expression(head, tracker); // consume unknown kwarg value
            }
            if (token.token_type == COMMA) token = lexer.GetToken();
        }
    }
    else if (op == SOL_OP_WAIT)
    {
        // sol.wait(job)
        if (token.token_type != RPAREN)
            job_slot_in = parse_expression(head, tracker);
    }
    else if (op == SOL_OP_PREDICT)
    {
        // sol.predict(job, input_text)
        if (token.token_type != RPAREN)
            job_slot_in = parse_expression(head, tracker);
        if (token.token_type == COMMA) {
            token = lexer.GetToken();
            input_slot = parse_expression(head, tracker);
        }
    }
    else if (op == SOL_OP_SAVE)
    {
        // sol.save(job, path)
        if (token.token_type != RPAREN)
            job_slot_in = parse_expression(head, tracker);
        if (token.token_type == COMMA) {
            token = lexer.GetToken();
            path_slot = parse_expression(head, tracker);
        }
    }
    else if (op == SOL_OP_SET_KEY)
    {
        // sol.set_key(path)
        if (token.token_type != RPAREN)
            path_slot = parse_expression(head, tracker);
    }

    if (token.token_type == RPAREN) token = lexer.GetToken(); // consume ')'

    // For finetune the job handle is an output slot; for others it's an input slot
    bool returnsJob    = (op == SOL_OP_FINETUNE);
    bool returnsString = (op == SOL_OP_PREDICT);

    int job_out_slot   = returnsJob    ? alloc_slot() : -1;
    int result_slot    = returnsString ? alloc_slot() : -1;
    int effective_job  = returnsJob ? job_out_slot : job_slot_in;

    auto* node = new InstructionNode();
    node->line_no              = token.line_no;
    node->type                 = SOL_CALL;
    node->sol_inst.op          = op;
    node->sol_inst.job_slot    = effective_job;
    node->sol_inst.model_slot  = model_slot;
    node->sol_inst.task_slot   = task_slot;
    node->sol_inst.dataset_slot= dataset_slot;
    node->sol_inst.epochs      = epochs;
    node->sol_inst.batch_size  = batch_size;
    node->sol_inst.lr          = lr;
    node->sol_inst.input_slot  = input_slot;
    node->sol_inst.result_slot = result_slot;
    node->sol_inst.path_slot   = path_slot;
    node->next = nullptr;
    append(head, tracker, node);

    if (returnsJob)    { lastExprType = TYPE_STRING; return job_out_slot; }
    if (returnsString) { lastExprType = TYPE_STRING; return result_slot; }
    lastExprType = TYPE_UNKNOWN;
    return alloc_slot();
}
