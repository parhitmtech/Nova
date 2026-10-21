/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// HuggingFace Inference API helpers (JSON escaping, response parsing). Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "integrations.h"

using namespace std;

// ── HF helpers ────────────────────────────────────────────────────────────────

string hf_escape_json(const string& s)
{
    string out;
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

string hf_extract_text(const string& json)
{
    if (json.empty()) return "[HF Error: empty response — check token or network]";
    // Detect HTML error page (proxy, firewall, or wrong endpoint)
    if (json.find("<!DOCTYPE") != string::npos || json.find("<html") != string::npos)
        return "[HF Error: received HTML instead of JSON — check EC2 outbound access to api-inference.huggingface.co]";

    auto find_field = [&](const string& field) -> string {
        string key = "\"" + field + "\"";
        size_t pos = json.find(key);
        if (pos == string::npos) return "";
        pos = json.find(':', pos + key.size());
        if (pos == string::npos) return "";
        pos = json.find('"', pos + 1);
        if (pos == string::npos) return "";
        size_t end = pos + 1;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == '\\') end++;
            end++;
        }
        return json.substr(pos + 1, end - pos - 1);
    };
    // Check for JSON error field first
    string err = find_field("error");
    if (!err.empty()) return "[HF Error: " + err + "]";
    string r = find_field("generated_text");
    if (!r.empty()) return r;
    r = find_field("label");
    if (!r.empty()) return r;
    r = find_field("answer");
    if (!r.empty()) return r;
    return "[HF Error: unexpected response: " + json.substr(0, 120) + "]";
}
