// Reading sources and expanding `import` (relative files and the stdlib under NOVA_HOME). Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "diagnostics.h"
#include "lexer.h"
#include "compiler.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"

using namespace std;

string read_file(const string& path) 
{
    ifstream f(path, ios::binary);
    if (!f.is_open()) return "";  // the caller reports it: it knows the line that asked for the file
    ostringstream ss;
    ss << f.rdbuf();
    string content = ss.str();
    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        content = content.substr(3);
    }
    return content;
}

string get_stdlib_path()
{
    const char* nova_home = getenv("NOVA_HOME");
    if (!nova_home || !*nova_home) return "";  // the caller reports it
    string path = string(nova_home);
    // normalize: strip trailing slash if present
    if (!path.empty() && (path.back() == '/' || path.back() == '\\'))
    {
        path.pop_back();
    }
    return path + "/stdlib";
}

// An imported file's 'ignite:' block only runs when that file itself is the program, so it is
// removed before the file is inlined: the 'ignite:' line (at column 0) and the indented, blank or
// comment lines after it. The removed lines are left empty so the line count does not change.
static string strip_ignite_blocks(const string& src)
{
    istringstream in(src);
    string line, out;
    bool inBlock = false;
    while (getline(in, line))
    {
        size_t first = line.find_first_not_of(" \t\r");
        bool blankOrComment = (first == string::npos || line[first] == '#');
        if (inBlock)
        {
            if (blankOrComment || first > 0) { out += "\n"; continue; }
            inBlock = false;
        }
        if (first == 0 && line.compare(0, 6, "ignite") == 0)
        {
            size_t k = 6;
            while (k < line.size() && line[k] == ' ') k++;
            if (k < line.size() && line[k] == ':')
            {
                size_t rest = line.find_first_not_of(" \t\r", k + 1);
                inBlock = (rest == string::npos || line[rest] == '#');  // otherwise a one-line block
                out += "\n";
                continue;
            }
        }
        out += line + "\n";
    }
    return out;
}

static bool file_exists(const string& path)
{
    ifstream f(path);
    return f.good();
}

// A file name in a "#@line" directive: in quotes, and without a quote or line break of its own
static string quoted(const string& path)
{
    string s = "\"";
    for (char c : path) if (c != '"' && c != '\n' && c != '\r') s += c;
    return s + "\"";
}

// An error in an 'orbit' line: the line is dropped and preprocessing goes on, so every bad line is reported
static void module_error(DiagCode code, const string& file, int line, const string& msg)
{
    diagnostics.report_at(code, file, line, msg);
}

string preprocess_import(const string& src, const string& base_dir, set<string>& already_imported, const string& file)
{
    string result;
    istringstream stream(src);
    string line;
    int lineNo = 0;  // line number in src of the line being processed

    while (getline(stream, line))
    {
        lineNo++;
        // trim leading whitespace for matching
        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);

        // orbit NAME brings in a module. A dotted name is a path: orbit utils.metrics is utils/metrics.nova
        // next to this file; if there is none, NAME.nova in the standard library (orbit novatorch.nn).
        // hf and sol are built in, so "orbit hf" / "orbit sol" only say that the program uses them.
        if (trimmed.compare(0, 6, "orbit ") == 0)
        {
            string name = trimmed.substr(6);
            size_t nameEnd = name.find_first_of(" \t\r#");
            if (nameEnd != string::npos) name = name.substr(0, nameEnd);
            if (name.empty())
            {
                module_error(DiagCode::OrbitName, file, lineNo, "'orbit' needs a module name, e.g. orbit novatorch.nn");
                result += "\n";
                continue;
            }
            if (name == "hf" || name == "sol") { result += "\n"; continue; }

            string localName = name;
            replace(localName.begin(), localName.end(), '.', '/');
            string localPath = base_dir + "/" + localName + ".nova";
            string stdlib = get_stdlib_path();
            string stdlibPath = stdlib + "/" + name + ".nova";
            string full_path = file_exists(localPath) ? localPath : stdlibPath;
            if (!file_exists(full_path))
            {
                if (stdlib.empty())
                {
                    module_error(DiagCode::NovaHomeNotSet, file, lineNo, "cannot find module '" + name +
                                 "' (looked for " + localPath + "); NOVA_HOME, the Nova installation that holds the "
                                 "standard library, is not set");
                }
                else
                {
                    module_error(DiagCode::ModuleNotFound, file, lineNo, "cannot find module '" + name +
                                 "' (looked for " + localPath + " and " + stdlibPath + ")");
                }
                result += "\n";
                continue;
            }

            if (already_imported.count(full_path)) { result += "\n"; continue; } // module is loaded once
            already_imported.insert(full_path);

            string module_dir = full_path.substr(0, full_path.find_last_of("/\\"));
            string inlined = preprocess_import(strip_ignite_blocks(read_file(full_path)), module_dir, already_imported,
                                               full_path);
            result += "#@line 1 " + quoted(full_path) + "\n" + inlined + "#@line " + to_string(lineNo + 1) + " " +
                      quoted(file) + "\n";
            continue;
        }
        if (trimmed.compare(0, 7, "import ") == 0)
        {
            module_error(DiagCode::ImportKeyword, file, lineNo,
                         "use 'orbit NAME' to bring in a module (e.g. orbit novatorch.nn), not import");
            result += "\n";
            continue;
        }

        // not an import line - keep it as it is
        result += line + "\n";
    }

    return result;
}
