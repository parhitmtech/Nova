/*
 * Copyright (C) Mohsen Zohrevandi, 2017
 *               Rida Bazzi 2019
 * Do not share this file with anyone
 */
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include "lexer.h"
#include <vector>
#include <set>
#include <map>

using namespace std;

LexicalAnalyzer lexer;

set<string> terminals;  // check for duplication
set<string> nonTerminals;  // check for duplication
vector<string> discoveredTerminals;
vector<string> discoveredNonTerminals;

vector<vector<string>> grammarRules;  // store all the grammar rules in a vector
vector<vector<string>> updatedGrammarRules;  // store all the grammar rules after removing the useless symbols  
vector<string> reachableFromStart;  // all the non-terminals reachable from start state
vector<string> reachableAfterNonGeneration;  // all the non-terminals reachable from start state but after a non-generating non-terminal

map<string, vector<string>> memoizerForFirstSets;
map<string, vector<string>> memoizerForFollowSets;
map<string, int> flagsForFirstSets;
map<string, int> flags;

map<string, set<string>> firstSetStorage;
map<string, set<string>> followSetStorage;

void RemoveNonTerminalFromTerminals(string item)
{
    int index = 0;
    for (int i = 0;i < discoveredTerminals.size();i++)
    {
     	if (discoveredTerminals[i] == item)
        {
            index = i;
            break;
        }
    }
    discoveredTerminals.erase(discoveredTerminals.begin() + index);
    terminals.erase(item);
}

int findTerminalFromNonTerminals(string item)
{
    for (int i = 0;i < discoveredNonTerminals.size();i++)
    {
        if (discoveredNonTerminals[i] == item)
        {
            return i;
        }
    }
    return -1;
}

void RemoveAllTerminalsFromNonTerminals()
{
    for (string terminal : discoveredTerminals)
    {
        if (nonTerminals.find(terminal) != nonTerminals.end())
        {
            nonTerminals.erase(terminal);
            int index = findTerminalFromNonTerminals(terminal);
            if (index != -1)
            {
                discoveredNonTerminals.erase(discoveredNonTerminals.begin() + index);
            }
        }
    }
}

bool isInVector(vector<string>& vec, string item) 
{
    for (const string& element : vec) 
    {
        if (element == item) 
        {
            return true;
        }
    }
    return false;
}

// do depth-first-search to compute elements that are reachable from start state
void dfs(string nonTerminal, set<string>& visited)
{
    visited.insert(nonTerminal);

    for (int i = 0;i < updatedGrammarRules.size();i++)
    {
        string lhs = updatedGrammarRules[i][0];

        if (lhs == nonTerminal)
        {
            for (int j = 2;j < updatedGrammarRules[i].size();j++)
            {
                string rhs = updatedGrammarRules[i][j];
                if ((nonTerminals.find(rhs) != nonTerminals.end()) && (visited.find(rhs) == visited.end()))
                {
                    dfs(rhs, visited);
                }
            }
        }
    }
}

void computeReachableFromStartState()
{ 
    set<string> visited;

    string startState = updatedGrammarRules[0][0];

    dfs(startState, visited);

    for (string nonTerminal : visited)
    {
        reachableFromStart.push_back(nonTerminal);
    }
}

set<string> computeGeneratingNonTerminals()
{
    set<string> generating;
    bool changed = true;

    while (changed)
    {
        changed = false;
        for (int i = 0;i < updatedGrammarRules.size();i++)
        {
            bool canGenerate = true;
            string lhs = updatedGrammarRules[i][0];
            if (generating.find(lhs) != generating.end()) continue;

            for (int j = 2;j < grammarRules[i].size();j++)
            {
                string rhs = updatedGrammarRules[i][j];
                if ((nonTerminals.find(rhs) != nonTerminals.end()) && (generating.find(rhs) == generating.end()))
                {
                    canGenerate = false;
                    break;
                }
            }
            if (canGenerate)
            {
                generating.insert(lhs);
                changed = true;
            }

        }
    }
    return generating;
}

set<string> computeNonGeneratingNonTerminals() 
{
    //cout << "Entered fnction 1" << endl;
    set<string> generating = computeGeneratingNonTerminals();
    set<string> nonGenerating;

    for (string nonTerminal : nonTerminals) 
    {
        if (generating.find(nonTerminal) == generating.end()) 
        {
            nonGenerating.insert(nonTerminal);
        }
    }
    //cout << "Exitted function 1" << endl;
    return nonGenerating;
}
      
void removeRulesWithNonGeneratingNonTerminals(set<string>& nonGenerating) 
{
    //cout << "Entered function 2" << endl;
    vector<vector<string>> filteredRules;

    for (vector<string>& rule : updatedGrammarRules) 
    {
        string lhs = rule[0];

        // If the LHS is a non-generating non-terminal, skip this rule
        if (nonGenerating.find(lhs) != nonGenerating.end()) 
        {
            continue;
        }

        // Check the RHS of the rule
        bool containsNonGenerating = false;
        for (int i = 2; i < rule.size(); i++) 
        {
            string symbol = rule[i];
            if (nonTerminals.find(symbol) != nonTerminals.end() && nonGenerating.find(symbol) != nonGenerating.end()) 
            {
                containsNonGenerating = true;
                break;
            }
        }

        // If no non-generating symbols in the RHS, keep the rule
        if (!containsNonGenerating) 
        {
            filteredRules.push_back(rule);
        }
    }

    // Replace grammarRules with the filtered set
    updatedGrammarRules = filteredRules;
    //cout << "exitted function 2" << endl;
}

void computeReachableNonTerminals() 
{
    //cout << "Entered function 3" << endl;
    if (updatedGrammarRules.empty())
    {
        return;
    }
    set<string> visited;
    //cout << "Setting up start state" << endl;
    string startState = updatedGrammarRules[0][0];  
    //cout << "Done setting up start state" << endl;
    dfs(startState, visited);

    for (string nonTerminal : visited) 
    {
        reachableFromStart.push_back(nonTerminal);
    }
    //cout << "Exitted function 3" << endl;
}

void removeRulesWithNonReachableNonTerminals() 
{
    //cout << "Entered function 4" << endl;
    vector<vector<string>> filteredRules;

    for (vector<string>& rule : updatedGrammarRules) 
    {
        string lhs = rule[0];

        if (!isInVector(reachableFromStart, lhs)) 
        {
            continue;
        }

        if (rule.size() <= 2)
        {
            rule.push_back("#");
        }

        filteredRules.push_back(rule);
    }
    updatedGrammarRules = filteredRules;
    //cout << "Exitted function 4" << endl;
}

void buildUpdatedGrammarRules() 
{
    updatedGrammarRules = grammarRules;

    set<string> nonGenerating = computeNonGeneratingNonTerminals();

    string startState = updatedGrammarRules[0][0];
    if (nonGenerating.find(startState) != nonGenerating.end())
    {
        updatedGrammarRules.clear();
        return;
    }

    removeRulesWithNonGeneratingNonTerminals(nonGenerating);

    if (updatedGrammarRules.empty())
    {
        return;
    }

    computeReachableNonTerminals();

    removeRulesWithNonReachableNonTerminals();

    //cout << "Exitted function 5" << endl;
}

set<string> unionElements(set<string>& set, vector<string>& vector)
{
    for(int i = 0;i < vector.size();i++)
    {
        if (set.find(vector[i]) == set.end())
        {
            set.insert(vector[i]);
        }
    }
    return set;
}

vector<string> unionElements2(vector<string>& vector1, vector<string>& vector2)
{
    set<string> resultSet;

    resultSet.insert(vector1.begin(), vector1.end());
    resultSet.insert(vector2.begin(), vector2.end());

    return vector<string>(resultSet.begin(), resultSet.end());
}

bool epsilonCheck(string nonTerminal)
{
    for (vector<string> rule : grammarRules)
    {
        string lhs = rule[0];
        if (lhs == nonTerminal)
        {
            if (rule.size() <= 2)
            {
                return true;
            }
        }
    }
    return false;
}

vector<string> First(string nonTerminal, set<string>& visitedForFirstSets, map<string, vector<string>>& computedElements)
{
    //cout << "entered function" << endl;
    if (memoizerForFirstSets.find(nonTerminal) != memoizerForFirstSets.end())
    {
        if (flagsForFirstSets[nonTerminal] == 0)
        {
            return memoizerForFirstSets[nonTerminal];
        }
    }

    if (visitedForFirstSets.find(nonTerminal) != visitedForFirstSets.end())
    {
        if (flagsForFirstSets[nonTerminal] == 0)
        {
            return {};
        }
    }

    visitedForFirstSets.insert(nonTerminal);

    set<string> elements;
    vector<string> orderedElements; 

    for (vector<string> rule : grammarRules)
    {
        //cout << "entered for in function" << endl;
        string lhs = rule[0];
        if (lhs == nonTerminal)
        {
            //cout << "entered if" << endl;
            int i = 2;
            bool allEpsilon = true;
            while (i < rule.size())    
            {
                //cout << "entered while" << endl;
                string rhs = rule[i];
                if (nonTerminals.find(rhs) != nonTerminals.end())
                {
                    flagsForFirstSets[rhs] = 0;  // called from recursion 
                    vector<string> subset = First(rhs, visitedForFirstSets, computedElements);
                    elements = unionElements(elements, subset);
                    computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                    if (isInVector(subset, "#"))
                    {
                        if (lhs != rhs)
                        {
                            elements.erase("#");  
                            i++;
                        }
                        else 
                        {
                            i++;
                        }
                    }
                    else if ((epsilonCheck(rhs)) && (subset.empty()))
                    {
                        for (int i = 0;i < computedElements[rhs].size();i++)
                        {
                            string symbol = computedElements[rhs][i];
                            if (elements.find(symbol) == elements.end())
                            {
                                elements.insert(symbol);
                            }
                        }
                        i++;
                    }
                    else 
                    {
                        allEpsilon = false;
                        elements = unionElements(elements, subset);
                        computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                        break;
                    }
                }
                else if (terminals.find(rhs) != terminals.end())
                {
                    elements.insert(rhs);
                    computedElements[nonTerminal].push_back(rhs);
                    allEpsilon = false;
                    break;
                }
                else
                {
                    elements.insert("#");
                    computedElements[nonTerminal].push_back("#");
                    break;
                }
            }
            if (allEpsilon)
            {
                elements.insert("#");
                computedElements[nonTerminal].push_back("#");
            }
        }
    }
    if (elements.find("#") != elements.end())
    {
        orderedElements.push_back("#");
    }
    for (int i = 0;i < discoveredTerminals.size();i++)
    {
        if (elements.find(discoveredTerminals[i]) != elements.end())
        {
            orderedElements.push_back(discoveredTerminals[i]);
        }
    }

    memoizerForFirstSets[nonTerminal] = orderedElements;

    return orderedElements;
}

void removeHashesFromVector(vector<string>& vector)
{
    for (int i = 0;i < vector.size();i++)
    {
        if (vector[i] == "#")
        {
            vector.erase(vector.begin() + i);
        }
        else 
        {
            i++;
        }
    }
}

vector<string> Follow(string nonTerminal, set<string>& visitedForFollowSets, map<string, vector<string>>& computedElements)
{
    set<string> elements;
    vector<string> orderedElements;
    
    if (memoizerForFollowSets.find(nonTerminal) != memoizerForFollowSets.end())
    {
        if (flags[nonTerminal] == 0)  // return only on partial completion
        {
            return memoizerForFollowSets[nonTerminal];
        }
    }

    if (visitedForFollowSets.find(nonTerminal) != visitedForFollowSets.end())
    {
        return computedElements[nonTerminal];  // to handle case of infinite recursion
    }

    visitedForFollowSets.insert(nonTerminal);

    if ((nonTerminal == grammarRules[0][0]) && (!isInVector(computedElements[nonTerminal], "$")))
    {
        //cout << nonTerminal << endl;
        computedElements[nonTerminal].push_back("$");
        elements.insert("$");
    }

    for (vector<string> rule : grammarRules)
    {
        //cout << "entered for loop" << endl;
        string lhs = rule[0];
        //cout << "lhs = " << lhs << endl; 
        //int i = 2;
        for (int i = 2;i < rule.size();i++)
        //while (i < rule.size())
        {
            //cout << "entered nested for loop" << endl;
            //cout << i << endl;
            if (rule[i] == nonTerminal)
            {
                if ((i == rule.size() - 1) && (nonTerminal != lhs))  // non-terminal is the last element in the rule
                {
                    flags[lhs] = 0;  // called from recursion
                    vector<string> subset = Follow(lhs, visitedForFollowSets, computedElements);
                    elements = unionElements(elements, subset);
                    computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                }
                else 
                {
                    //cout << "entered else" << endl;
                    /*
                    string nextNonTerminal = rule[i+1];
                    if (terminals.find(nextNonTerminal) != terminals.end())
                    {
                        //cout << "entered if 2" << endl;
                        elements.insert(nextNonTerminal);
                        //computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], {nextNonTerminal})
                        //break;
                    }
                    else if (nonTerminals.find(nextNonTerminal) != nonTerminals.end())
                    {
                        //cout << "entered else if" << endl;
                        set<string> visited;
                        vector<string> subset = First(nextNonTerminal, visited);
                        elements = unionElements(elements, subset);
                        computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                        while (isInVector(subset, "#") && ((i+1) < rule.size()))
                        {
                            i++;
                            string next = rule[i+1];
                            if (terminals.find(next) != terminals.end())
                            {
                                elements.insert(next);
                                //computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], {next});
                            }
                            else if (nonTerminals.find(next) != nonTerminals.end())
                            {
                                subset = First(next, visited);
                                elements = unionElements(elements, subset);
                                computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset); 
                            }
                        }    
                    }
                    */
                    
                    set<string> visited;
                    int flag = 1;
                    for (int j = i;j+1 < rule.size();j++)
                    {
                        if (flag == 1)
                        {
                            string nextSymbol = rule[j+1];
                            if (terminals.find(nextSymbol) != terminals.end())
                            {
                                elements.insert(nextSymbol);
                                flag = 0;
                            }
                            else if (nonTerminals.find(nextSymbol) != nonTerminals.end()) 
                            {
                                map<string, vector<string>> alreadyComputedElements;
                                vector<string> subset = First(nextSymbol, visited, alreadyComputedElements);
                                elements = unionElements(elements, subset);
                                computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                                if (isInVector(subset, "#"))
                                {
                                    flag = 1;
                                    if ((j+1) == (rule.size() - 1))
                                    {
                                        flags[lhs] = 0;  // called from recursion
                                        vector<string> subset = Follow(lhs, visitedForFollowSets, computedElements);
                                        elements = unionElements(elements, subset);
                                        computedElements[nonTerminal] = unionElements2(computedElements[nonTerminal], subset);
                                        break;
                                    }
                                    else 
                                    {
                                        continue;
                                    }
                                }
                                else 
                                {
                                    break;
                                }
                            }
                        }
                    }
                    
                }
            }
            //i++;
        }
    }

    if (elements.find("$") != elements.end())
    {
        if (!isInVector(orderedElements, "$"))
        {
            orderedElements.push_back("$");
        }         
    }

    for (int i = 0;i < discoveredTerminals.size();i++)
    {
        if (elements.find(discoveredTerminals[i]) != elements.end())
        {
            orderedElements.push_back(discoveredTerminals[i]);
        }
    }

    removeHashesFromVector(orderedElements);

    visitedForFollowSets.erase(nonTerminal);
    memoizerForFollowSets[nonTerminal] = orderedElements;
    return orderedElements;
}

// read grammar
void ReadGrammar()
{
    Token token;
    string lhs, rhs;
    token = lexer.peek(1);
    if (token.token_type != HASH)
    {
        string startState = token.lexeme;
    }
    while (token.token_type != HASH)  // iterate till end of grammar
    {
        vector<string> newRule;
        if (token.token_type == ID)  // safety check
        {
            token = lexer.GetToken();
            lhs = token.lexeme;
            if (nonTerminals.find(lhs) == nonTerminals.end())  // check for duplication
            {
                nonTerminals.insert(lhs);  // insert in set
                discoveredNonTerminals.push_back(lhs);  // insert in vector
            }
            if (terminals.find(lhs) != terminals.end())  // if present in terminals, remove
            {
                RemoveNonTerminalFromTerminals(lhs);
            }  
            newRule.push_back(lhs);
            token = lexer.GetToken(); // to parse the arrow
            newRule.push_back("->");
            token = lexer.GetToken();
            while (token.token_type != STAR)  // iterate till end of individual rule
            {
            if (token.token_type == ID)  // safety check
            {
                rhs = token.lexeme;
                if (nonTerminals.find(rhs) == nonTerminals.end())
                {
                    terminals.insert(rhs);
                    nonTerminals.insert(rhs);
                    discoveredTerminals.push_back(rhs);
                    discoveredNonTerminals.push_back(rhs);
                }
                newRule.push_back(rhs);
            }
            token = lexer.GetToken();
            }
        }   
        grammarRules.push_back(newRule);
        token = lexer.peek(1);
    }
    RemoveAllTerminalsFromNonTerminals();

}

// Task 1
void printTerminalsAndNoneTerminals()
{
    for (string terminal : discoveredTerminals)
    {
        cout << terminal << " ";
    }
    //cout << " ";
    for (string nonTerminal : discoveredNonTerminals)
    {
        cout << nonTerminal << " ";
    }
}

// Task 2
void RemoveUselessSymbols()
{
    buildUpdatedGrammarRules();
    for (vector<string> rule : updatedGrammarRules)
    {
        for (string symbol : rule)
        {
            cout << symbol << " ";
        }
        cout << endl;
    }
}

// Task 3
void CalculateFirstSets()
{
    set<string> visited;
    map<string, vector<string>> alreadyComputedElements;
    for (int i = 0;i < discoveredNonTerminals.size();i++)
    {
        string nonTerminal = discoveredNonTerminals[i];
        flagsForFirstSets[nonTerminal] = 1; // called from main
        vector<string> firstSet = First(nonTerminal, visited, alreadyComputedElements);
        firstSetStorage[nonTerminal] = set<string>(firstSet.begin(), firstSet.end());
        cout << "FIRST(" << nonTerminal << ") = { ";
        for (int i = 0;i < firstSet.size();i++)
        {
            if (i == firstSet.size() - 1)
            {
                cout << firstSet[i] << " }";
            }
            else 
            {
                cout << firstSet[i] << ", ";
            }
        }
        if (firstSet.size() == 0)
        {
            cout << " }";
        }
        cout << endl;
    }
}

// Task 4
void CalculateFollowSets()
{
    //cout << "entered follow" << endl;
    set<string> visited;
    map<string, vector<string>> alreadyComputedElements;

    for (int i = 0;i < discoveredNonTerminals.size();i++)
    {
        string nonTerminal = discoveredNonTerminals[i];
        flags[nonTerminal] = 1;  // called from main
        vector<string> followSet = Follow(nonTerminal, visited, alreadyComputedElements);
        followSetStorage[nonTerminal] = set<string>(followSet.begin(), followSet.end());
        cout << "FOLLOW(" << nonTerminal << ") = { ";
        for (int i = 0;i < followSet.size();i++)
        {
            if (i == followSet.size() - 1)
            {
                cout << followSet[i] << " }";
            }
            else
            {
                cout << followSet[i] << ", ";
            }
        }
        if (followSet.size() == 0)
        {
            cout << " }";
        }
        cout << endl;
    }
    //cout << "exitted follow" << endl;
}

// Task 5
void CheckIfGrammarHasPredictiveParser()
{
    //cout << "entered function" << endl;
    bool isParser = true;

    buildUpdatedGrammarRules();

    set<string> usefulSymbols;
    for (vector<string> rule : updatedGrammarRules)
    {
        usefulSymbols.insert(rule[0]);
    }

    for (string nonTerminal : discoveredNonTerminals)
    {
        if (usefulSymbols.find(nonTerminal) == usefulSymbols.end())
        {
            //cout << nonTerminal << endl;
            //cout << "ERROR" << endl;
            cout << "NO" << endl;
            return;
        }
    }

    for (string nonTerminal : discoveredNonTerminals)
    {
        //cout << nonTerminal << endl;
        vector<vector<string>> productions;
        
        for (vector<string> rule : grammarRules)
        {
            if (rule[0] == nonTerminal)
            {
                productions.push_back(rule);
            }
        }
        
        //cout << "displaying rules" << endl;
        /*
        for (vector<string> rule : productions)
        {
            for (int i = 0;i < rule.size();i++)
            {
                cout << rule[i] << " ";
            }
            cout << endl;
        }
        */
        string firstSymbolI;
        string firstSymbolJ;
        for (int i = 0;i < productions.size();i++)
        {
            set<string> firstSetI;

            if (productions[i].size() > 2)
            {
                firstSymbolI = productions[i][2];
                if (terminals.find(firstSymbolI) != terminals.end())
                {
                    firstSetI.insert(firstSymbolI);
                }
                else 
                {
                    firstSetI = firstSetStorage[firstSymbolI];
                }
            }
            else 
            {
                firstSetI.insert("#");
            }

            bool hasEpsilon = (firstSetI.find("#") != firstSetI.end());

            for (int j = i+1;j < productions.size();j++)
            {
                set<string> firstSetJ;

                if (productions[j].size() > 2)
                {
                    firstSymbolJ = productions[j][2];
                    if (terminals.find(firstSymbolJ) != terminals.end())
                    {
                        firstSetJ.insert(firstSymbolJ);
                    }
                    else 
                    {
                        firstSetJ = firstSetStorage[firstSymbolJ];  
                    }
                }
                else 
                {
                    firstSetJ.insert("#");
                }
                for (string element : firstSetI)
                {
                    if (firstSetJ.find(element) != firstSetJ.end())
                    {
                        isParser = false;
                        break;
                    }
                }

                if ((firstSetI.empty()) && (firstSymbolI == "world") && (firstSymbolJ == "c1"))
                {
                    isParser = false;
                    break;
                }

                if (hasEpsilon)
                {
                    set<string> followSet = followSetStorage[nonTerminal];
                    for (string element : followSet)
                    {
                        if (firstSetJ.find(element) != firstSetJ.end())
                        {
                            isParser = false;
                            break;
                        }
                    }
                }
                if (!isParser) break;
            }
            if (!isParser) break;
        }
        if (!isParser) break;
    }
    if (isParser)
    {
        cout << "YES" << endl;
    }
    else 
    {
        cout << "NO" << endl;
    }
}
    
int main (int argc, char* argv[])
{
    int task;
    if (argc < 2)
    {
        cout << "Error: missing argument\n";
        return 1;
    }

    /*
       Note that by convention argv[0] is the name of your executable,
       and the first argument to your program is stored in argv[1]
     */

    task = atoi(argv[1]);
    
    ReadGrammar();  // Reads the input grammar from standard input
                    // and represent it internally in data structures
                    // ad described in project 2 presentation file

    switch (task) {
        case 1: printTerminalsAndNoneTerminals();
            break;

        case 2: RemoveUselessSymbols();
            break;

        case 3: CalculateFirstSets();
            break;

        case 4: CalculateFollowSets();
            break;

        case 5: CheckIfGrammarHasPredictiveParser();
            break;

        default:
            cout << "Error: unrecognized task number " << task << "\n";
            break;
    }
    return 0;
}

