#include <cstdlib>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <iostream>
#include <ctype.h>
#include <string>
#include <map>
#include "compiler.h"
#include "lexer.h"

using namespace std;

map<string, int> symbolTable;
LexicalAnalyzer lexer;
Token token;

void parse_input_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_assignment_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_assignment_for_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_output_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_if_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_while_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_for_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);
void parse_switch_statement(struct InstructionNode *& head, struct InstructionNode *& tracker);

void parse_input_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    struct InstructionNode * newNode = new InstructionNode();
    newNode->type = IN;
    token = lexer.GetToken();
    newNode->input_inst.var_index = symbolTable[token.lexeme];

    if (head == nullptr)
    {
        head = newNode;
        tracker = newNode;
    }
    else 
    {
        tracker->next = newNode;
        tracker = newNode;
    }
}

void parse_assignment_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    //cout << "entered assignment statements" << endl;
    string lhs = token.lexeme;
    struct InstructionNode * newNode = new InstructionNode();
    token = lexer.GetToken();
    if (token.token_type == EQUAL)
    {
        //cout << "parsed equal" << endl;
        newNode->type = ASSIGN;
        newNode->assign_inst.left_hand_side_index = symbolTable[lhs];
        token = lexer.GetToken();
        if (token.token_type == NUM) // if (var = int)
        {
            //cout << "NUM" << endl;
            mem[next_available] = stoi(token.lexeme);
            newNode->assign_inst.operand1_index = next_available;
            next_available++;

            token = lexer.GetToken();
            if (token.token_type != SEMICOLON)
            {
                if (token.token_type == PLUS || token.token_type == MINUS || token.token_type == MULT || token.token_type == DIV)
                {
                    if (token.token_type == PLUS)
                    {
                        newNode->assign_inst.op = OPERATOR_PLUS;
                    }
                    else if (token.token_type == MINUS)
                    {
                        newNode->assign_inst.op = OPERATOR_MINUS;
                    }
                    else if (token.token_type == MULT)
                    {
                        newNode->assign_inst.op = OPERATOR_MULT;
                    }
                    else if (token.token_type == DIV)
                    {
                        newNode->assign_inst.op = OPERATOR_DIV;
                    }
                }
                token = lexer.GetToken();
                if (token.token_type == NUM)  // if (var = int + int)
                {
                    mem[next_available] = stoi(token.lexeme);
                    newNode->assign_inst.operand2_index = next_available;
                    next_available++;
                }
                else if (token.token_type == ID)  // if (var = int + var)
                {
                    newNode->assign_inst.operand2_index = symbolTable[token.lexeme];
                }
            }
            else  // if (var = int)
            {
                newNode->assign_inst.op = OPERATOR_NONE;
                //newNode->assign_inst.operand2_index = -1;
            }
        }
        else if (token.token_type == ID) // if (var = var)
        {
            //cout << "ID" << endl;
            newNode->assign_inst.operand1_index = symbolTable[token.lexeme];

            token = lexer.GetToken();
            if (token.token_type != SEMICOLON)
            {
                if (token.token_type == PLUS || token.token_type == MINUS || token.token_type == MULT || token.token_type == DIV)
                {
                    if (token.token_type == PLUS)
                    {
                        newNode->assign_inst.op = OPERATOR_PLUS;
                    }
                    else if (token.token_type == MINUS)
                    {
                        newNode->assign_inst.op = OPERATOR_MINUS;
                    }
                    else if (token.token_type == MULT)
                    {
                        newNode->assign_inst.op = OPERATOR_MULT;
                    }
                    else if (token.token_type == DIV)
                    {
                        newNode->assign_inst.op = OPERATOR_DIV;
                    }
                }
                token = lexer.GetToken();

                if (token.token_type == NUM)  // if (var = var + int)
                {
                    mem[next_available] = stoi(token.lexeme);
                    newNode->assign_inst.operand2_index = next_available;
                    next_available++;
                }
                else if (token.token_type == ID)  // if (var = var + var)
                {
                    newNode->assign_inst.operand2_index = symbolTable[token.lexeme];
                }
            }
            else 
            {
                newNode->assign_inst.op = OPERATOR_NONE;
                //newNode->assign_inst.operand2_index = -1;
            }
        }
        if (head == nullptr)
        {
            head = newNode;
            tracker = newNode;
        }
        else 
        {
            tracker->next = newNode;
            tracker = newNode;
        }
    }
    //cout << "exitted assignment" << endl;
}

void parse_output_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    //cout << "Entered OUTPUT area" << endl;
    struct InstructionNode * newNode = new InstructionNode();
    newNode->type = OUT;
    token = lexer.GetToken();
    newNode->output_inst.var_index = symbolTable[token.lexeme];

    if (head == nullptr)
    {
        head = newNode;
        tracker = newNode;
    }
    else 
    {
        tracker->next = newNode;
        tracker = newNode;
    }
    //cout << "Exitted OUTPUT area" << endl;
}

void parse_if_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    struct InstructionNode * newNode = new InstructionNode();
    newNode->type = CJMP;

    token = lexer.GetToken();  // parse the left-hand side of the condition
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        newNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    }
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        newNode->cjmp_inst.operand1_index = next_available;
        next_available++;
    }

    token = lexer.GetToken(); // parse the condition operator
    if (token.token_type == GREATER)
    {
        newNode->cjmp_inst.condition_op = CONDITION_GREATER;
    }
    else if (token.token_type == LESS)
    {
        newNode->cjmp_inst.condition_op = CONDITION_LESS;
    }
    else if (token.token_type == NOTEQUAL)
    {
        newNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
    }

    token = lexer.GetToken();  // parse the right-hand side of condition

    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        newNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    }
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        newNode->cjmp_inst.operand2_index = next_available;
        next_available++;
    }

    struct InstructionNode * noOpNode = new InstructionNode();
    noOpNode->type = NOOP;

    newNode->cjmp_inst.target = noOpNode;

    if (head == nullptr)
    {
        head = newNode;
        tracker = newNode;
    }
    else 
    {
        tracker->next = newNode;
        tracker = newNode;
    }

    token = lexer.GetToken();  // to parse the '{'
    token = lexer.GetToken();

    struct InstructionNode * lastInIfBlock = tracker;

    while (token.token_type != RBRACE)
    {
        if (token.token_type == INPUT)
        {
            parse_input_statement(head, tracker);
        }
        else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
        {
            parse_assignment_statement(head, tracker);
        }
        else if (token.token_type == IF)
        {
            parse_if_statement(head, tracker);
        }

        else if (token.token_type == WHILE)
        {
            parse_while_statement(head, tracker);
        }
        else if (token.token_type == FOR)
        {
            parse_for_statement(head, tracker);
        }
        else if (token.token_type == OUTPUT)
        {
            parse_output_statement(head, tracker);
        }
        else if (token.token_type == SWITCH)
        {
            parse_switch_statement(head, tracker);
        }
        token = lexer.GetToken();
    }

    tracker->next = noOpNode;
    tracker = noOpNode;
}

void parse_while_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    struct InstructionNode * newNode = new InstructionNode();
    newNode->type = CJMP;

    token = lexer.GetToken();  // parse the left-hand side of the condition
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        newNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    }
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        newNode->cjmp_inst.operand1_index = next_available;
        next_available++;
    }

    token = lexer.GetToken(); // parse the condition operator
    if (token.token_type == GREATER)
    {
        newNode->cjmp_inst.condition_op = CONDITION_GREATER;
    }
    else if (token.token_type == LESS)
    {
        newNode->cjmp_inst.condition_op = CONDITION_LESS;
    }
    else if (token.token_type == NOTEQUAL)
    {
        newNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
    }

    token = lexer.GetToken();  // parse the right-hand side of condition

    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        newNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    }
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        newNode->cjmp_inst.operand2_index = next_available;
        next_available++;
    }

    struct InstructionNode * noOpNode = new InstructionNode();
    noOpNode->type = NOOP;

    newNode->cjmp_inst.target = noOpNode;

    if (head == nullptr)
    {
        head = newNode;
        tracker = newNode;
    }
    else 
    {
        tracker->next = newNode;
        tracker = newNode;
    }

    struct InstructionNode * whileCondition = newNode;

    token = lexer.GetToken();  // parse the '{'
    token = lexer.GetToken();

    while (token.token_type != RBRACE)
    {
        if (token.token_type == INPUT)
        {
            parse_input_statement(head, tracker);
        }
        else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
        {
            parse_assignment_statement(head, tracker);
        }
        else if (token.token_type == IF)
        {
            parse_if_statement(head, tracker);
        }
        else if (token.token_type == WHILE)
        {
            parse_while_statement(head, tracker);
        }
        else if (token.token_type == FOR)
        {
            parse_for_statement(head, tracker);
        }
        else if (token.token_type == OUTPUT)
        {
            parse_output_statement(head, tracker);
        }
        else if (token.token_type == SWITCH)
        {
            parse_switch_statement(head, tracker);
        }
        token = lexer.GetToken();
    }

    struct InstructionNode * jmpNode = new InstructionNode();
    jmpNode->type = JMP;
    jmpNode->jmp_inst.target = whileCondition;

    tracker->next = jmpNode;
    jmpNode->next = noOpNode;
    tracker = noOpNode;
}

void parse_for_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    //cout << "Entered FOR area" << endl;
    token = lexer.GetToken();  // parse '('
    
    token = lexer.GetToken();  // parse the iterating variable
    //cout << token.lexeme << endl;
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        //cout << "Entered IF for first assignment" << endl;
        parse_assignment_statement(head, tracker);  // parse "i = 0;"
    }

    struct InstructionNode * forConditionNode = new InstructionNode();
    forConditionNode->type = CJMP;

    token = lexer.GetToken();  // parse the iterating variable again for storing looping condition
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        //cout << "Entered IF to parse looping condition" << endl;
        forConditionNode->cjmp_inst.operand1_index = symbolTable[token.lexeme];
    }

    else 
    {
        mem[next_available] = stoi(token.lexeme);
        forConditionNode->cjmp_inst.operand1_index = next_available;
        next_available++;
    }
    token = lexer.GetToken();  // parse the conditional operator
    if (token.token_type == GREATER)
    {
        forConditionNode->cjmp_inst.condition_op = CONDITION_GREATER;
    }
    else if (token.token_type == LESS)
    {
        forConditionNode->cjmp_inst.condition_op = CONDITION_LESS;
    }
    else if (token.token_type == NOTEQUAL)
    {
        forConditionNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;
    }

    token = lexer.GetToken();  // parse the variable or num in the rhs of condition
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        forConditionNode->cjmp_inst.operand2_index = symbolTable[token.lexeme];
    }
    else if (token.token_type == NUM)
    {
        mem[next_available] = stoi(token.lexeme);
        forConditionNode->cjmp_inst.operand2_index = next_available;
        next_available++;
    }

    struct InstructionNode * noOpNode = new InstructionNode();
    noOpNode->type = NOOP;
    forConditionNode->cjmp_inst.target = noOpNode;

    if (head == nullptr)
    {
        head = forConditionNode;
        tracker = forConditionNode;
    }

    else
    {
        tracker->next = forConditionNode;
        tracker = forConditionNode;
    }

    token = lexer.GetToken();  // parse the semicolon
    token = lexer.GetToken();   // parse the iterating variable again for increment operation
    struct InstructionNode * updateNode = nullptr;
    struct InstructionNode * updateTracker = nullptr;
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        //cout << "Entered IF to parse the updating assignment" << endl;
        parse_assignment_statement(updateNode, updateTracker);

        token = lexer.GetToken();  // ')'
        token = lexer.GetToken();  // parse '{'
        token = lexer.GetToken();

        while (token.token_type != RBRACE)
        {
            //cout << "ENTERED WHILE LOOP" << endl;
            if (token.token_type == INPUT)
            {
                parse_input_statement(head, tracker);
            }
            else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
            {
                parse_assignment_statement(head, tracker);
            }
            else if (token.token_type == IF)
            {
                parse_if_statement(head, tracker);
            }
            else if (token.token_type == WHILE)
            {
                parse_while_statement(head, tracker);
            }
            else if (token.token_type == FOR)
            {
                parse_for_statement(head, tracker);
            }
            else if (token.token_type == OUTPUT)
            {
                parse_output_statement(head, tracker);
            }
            else if (token.token_type == SWITCH)
            {
                parse_switch_statement(head, tracker);
            }
            token = lexer.GetToken();
        } 

        struct InstructionNode * jmpNode = new InstructionNode();
        jmpNode->type = JMP;
        jmpNode->jmp_inst.target = forConditionNode;

        // we increment the iterating variable before checking the looping condition
        tracker->next = updateNode;  

        // we link the increment tracker to the jump node so that it takes us to the 'forConditionNode'
        updateTracker->next = jmpNode;

        // since jmpNode is the last node to be executed in the logic to parse 'for', we set 'noOpNode' as its next node
        jmpNode->next = noOpNode;

        // we set the tracker to the last node in the sequence, which is 'noOpNode'
        tracker = noOpNode;
    }
    //cout << "exitted FOR area" << endl;
}


void parse_switch_statement(struct InstructionNode *& head, struct InstructionNode *& tracker)
{
    //cout << "entered function" << endl;
    token = lexer.GetToken();
    if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
    {
        //cout << "entered if statement" << endl;
        int memoryIndex = symbolTable[token.lexeme];  // extract the index from the symbol table
        int switchValue = mem[memoryIndex];  // extract the value of the variable from the memory

        token = lexer.GetToken();  // parse the '{'

        struct InstructionNode * noOpNode = new InstructionNode();
        noOpNode->type = NOOP;

        struct InstructionNode * firstCaseNode = nullptr;
        struct InstructionNode * lastCaseNode = nullptr;

        token = lexer.GetToken();
        while ((token.token_type == CASE) || (token.lexeme == "DEFAULT"))
        {
            //cout << "entered while loop" << endl;
            if (token.token_type == CASE)
            {
                token = lexer.GetToken();  // parse the case value
                if (token.token_type == NUM)
                {
                    // create a conditional jump node for this case
                    struct InstructionNode * caseNode = new InstructionNode();
                    caseNode->type = CJMP;
                    caseNode->cjmp_inst.operand1_index = memoryIndex;
                    caseNode->cjmp_inst.operand2_index = next_available;

                    mem[next_available] = stoi(token.lexeme);
                    next_available++;

                    caseNode->cjmp_inst.condition_op = CONDITION_NOTEQUAL;

                    if (firstCaseNode == nullptr)
                    {
                        firstCaseNode = caseNode;
                        lastCaseNode = caseNode;
                    }
                    else 
                    {
                        lastCaseNode->next = caseNode;
                        lastCaseNode = caseNode;
                    }

                    if (head == nullptr)
                    {
                        head = caseNode;
                        tracker = caseNode;
                    }
                    else 
                    {
                        tracker->next = caseNode;
                        tracker = caseNode;
                    }

                    token = lexer.GetToken();  // parse the ':'
                    token = lexer.GetToken();  // parse the '{'
                    token = lexer.GetToken();

                    struct InstructionNode * bodyHead = nullptr;
                    struct InstructionNode * bodyTracker = nullptr;

                    while (token.token_type != RBRACE)
                    {
                        //cout << "inside inner while loop" << endl;
                        if (token.token_type == INPUT)
                        {
                            parse_input_statement(bodyHead, bodyTracker);
                        }
                        else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
                        {
                            parse_assignment_statement(bodyHead, bodyTracker);
                        }
                        else if (token.token_type == IF)
                        {
                            parse_if_statement(bodyHead, bodyTracker);
                        }
                        else if (token.token_type == WHILE)
                        {
                            parse_while_statement(bodyHead, bodyTracker);
                        }
                        else if (token.token_type == FOR)
                        {
                            parse_for_statement(bodyHead, bodyTracker);
                        }
                        else if (token.token_type == OUTPUT)
                        {
                            //cout << "flag" << endl;
                            parse_output_statement(bodyHead, bodyTracker);
                        }
                        else if (token.token_type == SWITCH)
                        {
                            parse_switch_statement(bodyHead, bodyTracker);
                        }
                        token = lexer.GetToken();
                    }

                    caseNode->cjmp_inst.target = bodyHead;

                    struct InstructionNode * jmpNode = new InstructionNode();
                    jmpNode->type = JMP;
                    jmpNode->jmp_inst.target = noOpNode;
                    bodyTracker->next = jmpNode;
                }
            }
            else if ((token.lexeme == "DEFAULT")) 
            {
                struct InstructionNode * defaultCaseNode = new InstructionNode();
                defaultCaseNode->type = JMP;

                if (firstCaseNode == nullptr)
                {
                    firstCaseNode = defaultCaseNode;
                    lastCaseNode = defaultCaseNode;
                }

                else 
                {
                    lastCaseNode->next = defaultCaseNode;
                    lastCaseNode = defaultCaseNode;
                }

                if (head == nullptr)
                {
                    head = defaultCaseNode;
                    tracker = defaultCaseNode;
                }

                else
                {
                    tracker->next = defaultCaseNode;
                    tracker = defaultCaseNode;
                }

                struct InstructionNode * bodyHead = nullptr;
                struct InstructionNode * bodyTracker = nullptr;

                token = lexer.GetToken();  // parse the ':'
                token = lexer.GetToken();  // parse the '{'

                token = lexer.GetToken();
                while (token.token_type != RBRACE)
                {
                    if (token.token_type == INPUT)
                    {
                        parse_input_statement(bodyHead, bodyTracker);
                    }
                    else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
                    {
                        parse_assignment_statement(bodyHead, bodyTracker);
                    }
                    else if (token.token_type == IF)
                    {
                        parse_if_statement(bodyHead, bodyTracker);
                    }
                    else if (token.token_type == WHILE)
                    {
                        parse_while_statement(bodyHead, bodyTracker);
                    }
                    else if (token.token_type == FOR)
                    {
                        parse_for_statement(bodyHead, bodyTracker);
                    }
                    else if (token.token_type == OUTPUT)
                    {
                        //cout << "flag" << endl;
                        parse_output_statement(bodyHead, bodyTracker);
                    }
                    else if (token.token_type == SWITCH)
                    {
                        parse_switch_statement(bodyHead, bodyTracker);
                    }
                    token = lexer.GetToken();
                }

                defaultCaseNode->jmp_inst.target = bodyHead;

                struct InstructionNode * jmpNode = new InstructionNode();
                jmpNode->type = JMP;
                jmpNode->jmp_inst.target = noOpNode;
                bodyTracker->next = jmpNode;
            }
            token = lexer.GetToken();
        }
        if (lastCaseNode)
        {
            lastCaseNode->next = noOpNode;
        }
        tracker->next = noOpNode;
        tracker = noOpNode;
    }
}

struct InstructionNode * parse_generate_intermediate_representation()
{
    token = lexer.GetToken();
    while (token.token_type != SEMICOLON)
    {
        if (token.token_type == ID)
        {
            mem[next_available] = 0;
            symbolTable[token.lexeme] = next_available; 
            next_available++;
        }
        token = lexer.GetToken();
    }

    token = lexer.GetToken(); // to parse the '{'

    struct InstructionNode * head = nullptr;
    struct InstructionNode * tracker = nullptr;

    token = lexer.GetToken();

    while (token.token_type != RBRACE)
    {
        if (token.token_type == INPUT) 
        {
            parse_input_statement(head, tracker);
        }
        else if ((token.token_type == ID) && (symbolTable.find(token.lexeme) != symbolTable.end()))
        {
            parse_assignment_statement(head, tracker);
        }
        else if (token.token_type == IF)
        {
            parse_if_statement(head, tracker); 
        }
        else if (token.token_type == WHILE)
        {
            parse_while_statement(head, tracker);
        }
        else if (token.token_type == FOR)
        {
            parse_for_statement(head, tracker);
        }
        else if (token.token_type == OUTPUT) 
        {
            parse_output_statement(head, tracker);
        }
        else if (token.token_type == SWITCH)
        {
            parse_switch_statement(head, tracker);
        }
        token = lexer.GetToken();
    }

    token = lexer.GetToken();
    while ((token.token_type != END_OF_FILE) && (token.token_type == NUM))
    {
        inputs.push_back(stoi(token.lexeme));
        token = lexer.GetToken();
    }

    if (head == nullptr) {
        struct InstructionNode * noOpNode = new InstructionNode();
        noOpNode->type = NOOP;
        head = noOpNode;
        tracker = noOpNode;
    }

    return head;
}
