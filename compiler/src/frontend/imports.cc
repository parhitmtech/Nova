/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
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
#include "lexer.h"
#include "compiler.h"
#include "bytecode.h"
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
    ifstream f(path);
    if (!f.is_open()) {
        cerr << "Error: cannot open file '" << path << "'\n";
        return "";
    }
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
    if (!nova_home)
    {
        cerr << "Error: NOVA_HOME environment variable is not set.\n";
        cerr << "       Set it to your Nova install directory (e.g. export NOVA_HOME=/usr/local/nova)\n";
        return "";
    }
    string path = string(nova_home);
    // normalize: strip trailing slash if present
    if (!path.empty() && (path.back() == '/' || path.back() == '\\'))
    {
        path.pop_back();
    }
    return path + "/stdlib";
}

// An imported file's 'ignite { ... }' block only runs when that file itself is the program, so it is
// removed before the file is inlined. Strings and comments are skipped; newlines inside the block are
// kept so the line count does not change.
static string strip_ignite_blocks(const string& src)
{
    string out;
    size_t i = 0, n = src.size();
    auto is_ident = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    while (i < n)
    {
        char c = src[i];
        if (c == '"')  // string literal: copy as is
        {
            size_t j = i + 1;
            while (j < n && src[j] != '"' && src[j] != '\n') j += (src[j] == '\\') ? 2 : 1;
            j = min(j + 1, n);
            out.append(src, i, j - i);
            i = j;
            continue;
        }
        if (c == '#' || (c == '/' && i + 1 < n && src[i + 1] == '/'))  // comment: copy to end of line
        {
            size_t j = src.find('\n', i);
            if (j == string::npos) j = n;
            out.append(src, i, j - i);
            i = j;
            continue;
        }
        if (src.compare(i, 6, "ignite") == 0 && (i == 0 || !is_ident(src[i - 1])) &&
            (i + 6 >= n || !is_ident(src[i + 6])))
        {
            size_t j = i + 6;
            while (j < n && isspace((unsigned char)src[j])) j++;
            if (j < n && src[j] == '{')
            {
                // skip to the matching '}', keeping newlines
                int depth = 0;
                for (size_t k = i; k < j; k++) if (src[k] == '\n') out += '\n';
                for (; j < n; j++)
                {
                    if (src[j] == '\n') out += '\n';
                    if (src[j] == '"')  // braces inside strings do not count
                    {
                        j++;
                        while (j < n && src[j] != '"' && src[j] != '\n') j += (src[j] == '\\') ? 2 : 1;
                        continue;
                    }
                    if (src[j] == '#' || (src[j] == '/' && j + 1 < n && src[j + 1] == '/'))  // nor in comments
                    {
                        while (j + 1 < n && src[j + 1] != '\n') j++;
                        continue;
                    }
                    if (src[j] == '{') depth++;
                    else if (src[j] == '}' && --depth == 0) { j++; break; }
                }
                i = j;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

string preprocess_import(const string& src, const string& base_dir, set<string>& already_imported)
{
    string result;
    istringstream stream(src);
    string line;

    while (getline(stream, line))
    {
        // trim leading whitespace for matching
        string trimmed = line;
        size_t start = trimmed.find_first_not_of(" \t");
        if (start != string::npos) trimmed = trimmed.substr(start);

        // mathc: import "file.nova";
        if (trimmed.size() > 8 && trimmed.substr(0, 7) == "import " && trimmed[7] == '"')
        {
            size_t close = trimmed.find('"', 8);
            if (close != string::npos)
            {
                string filename = trimmed.substr(8, close - 8);
                string full_path = base_dir + "/" + filename;

                if (already_imported.count(full_path))
                {
                    // already included, skip silently
                    continue;
                }
                already_imported.insert(full_path);

                string file_src = read_file(full_path);
                if (file_src.empty()) continue;

                // get the directory of the imported file for nested imports
                string imported_dir = full_path;
                size_t last_slash = imported_dir.find_last_of("/\\");
                if (last_slash != string::npos)
                {
                    imported_dir = imported_dir.substr(0, last_slash);
                }
                else
                {
                    imported_dir = ".";
                }

                string inlined = preprocess_import(strip_ignite_blocks(file_src), imported_dir, already_imported);
                result += inlined + "\n";
                continue;
            }
        }

        // match: import <package> ;
        if (trimmed.size() > 8 && trimmed.substr(0, 7) == "import " && trimmed[7] == '<')
        {
            size_t close = trimmed.find('>', 8);
            if (close != string::npos)
            {
                string pkg_name = trimmed.substr(8, close - 8);
                string stdlib_dir = get_stdlib_path();
                if (stdlib_dir.empty()) continue;

                string full_path = stdlib_dir + "/" + pkg_name + ".nova";

                if (already_imported.count(full_path))
                {
                    continue;
                }
                already_imported.insert(full_path);

                string file_src = read_file(full_path);
                if (file_src.empty()) continue;

                string inlined = preprocess_import(strip_ignite_blocks(file_src), stdlib_dir, already_imported);
                result += inlined + "\n";
                continue;
            }
        }

        // match: import hf  (HuggingFace built-in — hf.xxx() always available)
        if (trimmed.size() >= 9 && trimmed.substr(0, 9) == "import hf" &&
            (trimmed.size() == 9 || trimmed[9] == ' ' || trimmed[9] == ';'))
        {
            continue;
        }

        // match: import sol  (SOL cluster built-in — sol.xxx() always available)
        if (trimmed.size() >= 10 && trimmed.substr(0, 10) == "import sol" &&
            (trimmed.size() == 10 || trimmed[10] == ' ' || trimmed[10] == ';'))
        {
            continue;
        }

        // not an import line - keep it as it is
        result += line + "\n";
    }

    return result;
}
