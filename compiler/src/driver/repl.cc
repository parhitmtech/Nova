// The REPL (--repl). Each entry is compiled as a program of its own: the definitions entered so far (def, class,
// struct, orbit), then an ignite block that declares again the variables earlier entries made, followed by the
// entry's statements. Lowering gives those variables the slots they already have (lower.h's ReplVariable), and
// memory and the object, container and tensor heaps live on between entries, so each entry sees what the ones before
// it left.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "check.h"
#include "lexer.h"
#include "lower.h"
#include "parse_tree.h"
#include "runtime.h"

using namespace std;

namespace
{
    int lines_in(const string& s)
    {
        int n = 0;
        for (char c : s) n += c == '\n';
        return n;
    }

    // "Error at line N: ..." with N counted from the entry's first line (first: the program's line it starts on)
    string from_entry(const string& error, int first)
    {
        const string at = "Error at line ";
        size_t colon = error.find(':');
        if (error.compare(0, at.size(), at) != 0 || colon == string::npos) return error;
        int line = atoi(error.c_str() + at.size());
        if (line < first) return error;
        return at + to_string(line - first + 1) + error.substr(colon);
    }

    // Compiles and runs one program. On an error (printed) nothing runs and the session is as it was.
    bool run(const string& source, int first, vector<ReplVariable>* session)
    {
        set<string> imported;
        string processed = preprocess_import(source, ".", imported);
        vector<LexToken> tokens = scan_program(processed);
        ParseTree tree = build_parse_tree(tokens);
        vector<string> errors = tree.errors;
        CheckResult checked;
        if (errors.empty())
        {
            checked = check_tree(tree);
            errors = checked.errors;
        }
        if (errors.empty())
        {
            uint32_t n = unlowered(tree, checked);
            if (n != NoNode) errors.push_back("Error at line " + to_string(tree.token(n).line) +
                                              ": the compiler cannot lower this yet");
        }
        for (const string& e : errors) fprintf(stderr, "%s\n", from_entry(e, first).c_str());
        if (!errors.empty()) return false;
        InstructionNode* program = lower_tree(tree, checked, session);
        input_replay_index = -1;  // live stdin
        execute_program(program);
        return true;
    }
}

void run_repl()
{
    printf("\n");
    printf("  NovaComp v2.0 REPL\n");
    printf("  Type statements one at a time.\n");
    printf("  Type 'exit' or 'quit' to quit.\n");
    printf("\n");
    fflush(stdout);

    string definitions;             // the def, class, struct and orbit entries so far
    vector<ReplVariable> session;   // the variables entries have declared
    string accumulated = "";        // the entry being read
    bool in_block = false;          // a line ended with ':': keep reading until a blank line (like Python)

    while (true)
    {
        printf(in_block ? "... " : ">>> ");
        fflush(stdout);

        string line;
        if (!getline(cin, line)) break;  // EOF

        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != string::npos) trimmed = trimmed.substr(0, end + 1);

        if (trimmed == "exit" || trimmed == "quit")
        {
            printf("\n  Bye!\n\n");
            break;
        }

        accumulated += line + "\n";

        // A line ending in ':' opens a block: keep reading until an empty line ends it.
        string code = trimmed.substr(0, trimmed.find('#'));
        size_t codeEnd = code.find_last_not_of(" \t");
        if (codeEnd != string::npos && code[codeEnd] == ':') in_block = true;
        if (in_block)
        {
            if (!trimmed.empty()) continue;
            in_block = false;
        }
        if (accumulated.find_first_not_of(" \t\r\n") == string::npos)
        {
            accumulated = "";
            continue;
        }

        // The ignite block's start: the session's variables, declared again
        string restore = "ignite:\n";
        for (const ReplVariable& v : session) restore += "    " + v.declaration + "\n";

        bool hasIgnite = accumulated.find("ignite") != string::npos;
        bool hasTopLevel = hasIgnite || accumulated.find("def ") != string::npos ||
                           accumulated.find("class ") != string::npos || accumulated.find("struct ") != string::npos ||
                           accumulated.find("orbit ") != string::npos;
        if (!hasTopLevel)  // statements: they run in the ignite block, after the session's variables
        {
            string source = definitions + restore;
            int first = lines_in(source) + 1;
            istringstream lines(accumulated);
            string l;
            while (getline(lines, l)) source += "    " + l + "\n";
            run(source, first, &session);
        }
        else if (!hasIgnite)  // definitions: kept for later entries once they compile
        {
            string source = definitions + accumulated;
            int first = lines_in(definitions) + 1;
            source += "\n" + restore + "    pass\n";
            if (run(source, first, &session)) definitions += accumulated + "\n";
        }
        else  // a whole program with its own ignite block: the definitions so far, but not the session's variables
        {
            run(definitions + accumulated, lines_in(definitions) + 1, nullptr);
        }

        accumulated = "";
        in_block = false;
    }
}
