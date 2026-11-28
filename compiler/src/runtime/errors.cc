// Runtime errors: a running program that cannot go on stops with a diagnostic (diagnostics.h) at the expression that
// failed, as a compile error does, and the calls that led to it as notes.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stack>
#include <string>
#include <vector>
#include "compiler.h"
#include "diagnostics.h"
#include "runtime.h"
#include "debugger.h"

using namespace std;

const InstructionNode* runtime_pc = nullptr;

namespace
{
    // Where an instruction's expression is shown. A call's or an index's token is its '(' or '[': the caret goes back
    // to the name before it (half_of(10, 0), v[i]), which is underlined.
    SourceRange shown_range(const InstructionNode* in)
    {
        SourceLoc b = in->loc, e = in->loc + (in->loc_length ? in->loc_length : 1);
        char c = *sourceManager.text_at(in->loc);
        if (c == '(' || c == '[')
        {
            SourceLoc start = 0;
            sourceManager.line_text(in->loc, &start);
            SourceLoc p = in->loc;
            while (p > start)
            {
                char q = *sourceManager.text_at(p - 1);
                if (!(isalnum((unsigned char)q) || q == '_' || q == '.')) break;
                p--;
            }
            if (p < in->loc)
            {
                b = p;
                e = in->loc;
            }
        }
        return {b, e};
    }

    // A note at an instruction: its exact expression when lowering recorded it, else nothing
    void note_at(Diagnostic& d, const InstructionNode* in, const string& message)
    {
        if (!in || !in->loc) return;
        SourceRange r = shown_range(in);
        DiagnosticEngine::add_note(d, r.begin, r.end - r.begin, message);
    }
}

[[noreturn]] void runtime_failure(DiagCode code, int line, const string& message)
{
    fflush(stdout);  // what the program printed comes first
    // The step debugger stops at the error first, so its variables and call stack can be seen
    if (debug_mode) debugger_runtime_error(string(diag_info(code).code) + ": " + message);
    diagnostics.clear();  // warnings were printed before the program ran
    diagnostics.set_error_limit(0);
    const InstructionNode* pc = runtime_pc;
    Diagnostic* d;
    if (pc && pc->loc && pc->line_no == line)
    {
        SourceRange r = shown_range(pc);
        d = &diagnostics.report(code, r.begin, r, message);
    }
    else
    {
        // the line is known but not the expression: the line of the file being run
        d = &diagnostics.report_at(code, program_file, line, message);
    }

    // The calls that are running, the latest first. A call site repeated in a row (recursion) is one note with its
    // count; past 10 notes, the ones between the first 8 and the last 2 are left out.
    vector<pair<const InstructionNode*, size_t>> sites;
    for (stack<CallFrame> calls = callStack; !calls.empty(); calls.pop())
    {
        const InstructionNode* site = calls.top().callSite;
        if (!sites.empty() && sites.back().first == site) sites.back().second++;
        else sites.push_back({site, 1});
    }
    for (size_t i = 0; i < sites.size(); i++)
    {
        if (sites.size() > 10 && i >= 8 && i + 2 < sites.size())
        {
            if (i == 8)
            {
                Diagnostic more{Severity::Note, code};
                more.message = "... " + to_string(sites.size() - 10) + " more calls ...";
                d->notes.push_back(more);
            }
            continue;
        }
        size_t n = sites[i].second;
        note_at(*d, sites[i].first, n == 1 ? "called from here" : "called from here " + to_string(n) +
                                                                   " times in a row (recursion)");
    }
    diagnostics.print(stderr);
    exit(1);
}
