// The diagnostics engine (see diagnostics.h): recording, then printing as text with the source line, or as JSON.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "diagnostics.h"

#ifdef _WIN32
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace std;

DiagnosticEngine diagnostics;

static const vector<DiagInfo> infos = {
#define DIAG(name, code, severity, title) {code, Severity::severity, title},
#include "diagnostic_codes.def"
#undef DIAG
};

const DiagInfo& diag_info(DiagCode code) { return infos[(size_t)code]; }
const vector<DiagInfo>& all_diag_infos() { return infos; }

// Layout and syntax errors (E01, E02): the first on a line is reported and the rest of that line's are not, since
// they mostly follow from it. Errors about meaning (names, types) are all reported: two on a line are two mistakes.
static bool is_syntax(const DiagInfo& info)
{
    return info.code[0] == 'E' && info.code[1] == '0' && (info.code[2] == '1' || info.code[2] == '2');
}

bool DiagnosticEngine::line_has_error(SourceLoc loc) const
{
    PresumedLoc p = sourceManager.presumed(loc);
    for (const auto& l : errorLines) if (l.second == p.line && l.first == p.file) return true;
    return false;
}

Diagnostic& DiagnosticEngine::report(DiagCode code, SourceLoc loc, SourceRange range, string message)
{
    const DiagInfo& info = diag_info(code);
    scratch = Diagnostic{info.severity, code};
    if (info.severity == Severity::Warning && !warnings) return scratch;
    if (info.severity == Severity::Error)
    {
        if (limit_reached())
        {
            limitHit = true;
            return scratch;
        }
        if (loc && is_syntax(info))
        {
            if (line_has_error(loc)) return scratch;
            for (const SourceRange& r : quiet) if (loc >= r.begin && loc < r.end) return scratch;
        }
    }
    Diagnostic d{info.severity, code, loc};
    d.range = range.begin ? range : SourceRange{loc, loc + 1};
    if (d.range.end <= d.range.begin) d.range.end = d.range.begin + 1;
    d.message = std::move(message);
    if (info.severity == Severity::Error)
    {
        errors++;
        if (loc)
        {
            PresumedLoc p = sourceManager.presumed(loc);
            errorLines.push_back({p.file, p.line});
        }
    }
    diags.push_back(std::move(d));
    return diags.back();
}

Diagnostic& DiagnosticEngine::report_at(DiagCode code, const string& file, int line, string message)
{
    const DiagInfo& info = diag_info(code);
    scratch = Diagnostic{info.severity, code};
    if (info.severity == Severity::Error && limit_reached())
    {
        limitHit = true;
        return scratch;
    }
    Diagnostic d{info.severity, code};
    d.message = std::move(message);
    d.written.file = file;
    d.written.line = line;
    d.written.column = line > 0 ? 1 : 0;
    if (info.severity == Severity::Error)
    {
        errors++;
        if (line > 0) errorLines.push_back({file, line});
    }
    diags.push_back(std::move(d));
    return diags.back();
}

void DiagnosticEngine::add_note(Diagnostic& d, SourceLoc loc, uint32_t length, string message)
{
    Diagnostic n{Severity::Note, d.code, loc};
    n.range = SourceRange{loc, loc + (length ? length : 1)};
    n.message = std::move(message);
    d.notes.push_back(std::move(n));
}

void DiagnosticEngine::clear()
{
    diags.clear();
    errorLines.clear();
    quiet.clear();
    errors = 0;
    limitHit = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Text
// ─────────────────────────────────────────────────────────────────────────────

bool stderr_supports_color()
{
    const char* noColor = getenv("NO_COLOR");
    if (noColor && *noColor) return false;
    const char* term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return false;
#ifdef _WIN32
    if (!_isatty(_fileno(stderr))) return false;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */) != 0;
#else
    return isatty(fileno(stderr));
#endif
}

namespace
{
    struct Style
    {
        bool on;
        const char* bold() const { return on ? "\x1b[1m" : ""; }
        const char* reset() const { return on ? "\x1b[0m" : ""; }
        const char* severity(Severity s) const
        {
            if (!on) return "";
            return s == Severity::Error ? "\x1b[1;31m" : s == Severity::Warning ? "\x1b[1;35m" : "\x1b[1;36m";
        }
        const char* caret() const { return on ? "\x1b[1;32m" : ""; }
        const char* gutter() const { return on ? "\x1b[34m" : ""; }
    };

    const char* severity_word(Severity s)
    {
        return s == Severity::Error ? "error" : s == Severity::Warning ? "warning" : "note";
    }

    // "runtime error" for an error found while the program ran (an R code)
    string severity_text(const Diagnostic& d)
    {
        bool runtime = d.severity == Severity::Error && diag_info(d.code).code[0] == 'R';
        return runtime ? "runtime error" : severity_word(d.severity);
    }

    // The columns a person sees for the bytes of text: a UTF-8 character is one column however many bytes it takes
    int display_width(string_view text)
    {
        int w = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) w++;
        return w;
    }

    // The source line under a diagnostic, with its range underlined and a caret at its location:
    //     6 |     print(square("four"))
    //       |                  ^~~~~~
    void print_snippet(FILE* out, const Diagnostic& d, const Style& st)
    {
        SourceLoc lineStart = 0;
        string_view text = sourceManager.line_text(d.loc, &lineStart);
        if (lineStart == 0) return;
        PresumedLoc p = sourceManager.presumed(d.loc);
        string line(text);
        for (char& c : line) if (c == '\t') c = ' ';  // a tab would move the caret: show it as one space
        string number = to_string(p.line);
        string pad(number.size() < 4 ? 4 : number.size(), ' ');
        string num = string(pad.size() - number.size(), ' ') + number;

        SourceLoc lineEnd = lineStart + (SourceLoc)text.size();
        SourceLoc b = max(d.range.begin, lineStart), e = min(max(d.range.end, d.loc + 1), lineEnd);
        if (d.loc < lineStart || d.loc > lineEnd) return;
        if (b > d.loc) b = d.loc;
        int startCol = display_width(text.substr(0, b - lineStart));
        int caretCol = display_width(text.substr(0, d.loc - lineStart));
        int endCol = max(display_width(text.substr(0, e - lineStart)), caretCol + 1);

        string marks(endCol, ' ');
        for (int i = startCol; i < endCol; i++) marks[i] = '~';
        marks[caretCol] = '^';
        marks.erase(0, startCol);

        fprintf(out, "%s%s |%s %s\n", st.gutter(), num.c_str(), st.reset(), line.c_str());
        fprintf(out, "%s%s |%s %s%s%s%s\n", st.gutter(), pad.c_str(), st.reset(), string(startCol, ' ').c_str(),
                st.caret(), marks.c_str(), st.reset());
    }

    void print_one(FILE* out, const Diagnostic& d, const Style& st, bool withCode)
    {
        const DiagInfo& info = diag_info(d.code);
        string where;
        if (d.loc)
        {
            PresumedLoc p = sourceManager.presumed(d.loc);
            where = p.file + ":" + to_string(p.line) + ":" + to_string(p.column) + ": ";
        }
        else if (!d.written.file.empty())
        {
            where = d.written.file + (d.written.line > 0 ? ":" + to_string(d.written.line) : "") + ": ";
        }
        else
        {
            where = "nova: ";
        }
        string code = withCode ? string("[") + info.code + "]" : "";
        fprintf(out, "%s%s%s%s%s%s%s: %s%s\n", st.bold(), where.c_str(), st.severity(d.severity),
                severity_text(d).c_str(), code.c_str(), st.reset(), st.bold(), d.message.c_str(), st.reset());
        if (d.loc) print_snippet(out, d, st);
    }

    vector<const Diagnostic*> in_source_order(const vector<Diagnostic>& diags)
    {
        vector<const Diagnostic*> order;
        for (const Diagnostic& d : diags) order.push_back(&d);
        stable_sort(order.begin(), order.end(), [](const Diagnostic* a, const Diagnostic* b) {
            return (a->loc ? a->loc : 0) < (b->loc ? b->loc : 0);
        });
        return order;
    }
}

void DiagnosticEngine::print_text(FILE* out, bool color) const
{
    Style st{color};
    int warningCount = 0;
    for (const Diagnostic* d : in_source_order(diags))
    {
        print_one(out, *d, st, true);
        for (const Diagnostic& n : d->notes) print_one(out, n, st, false);
        if (d->severity == Severity::Warning) warningCount++;
    }
    if (limitHit)
    {
        fprintf(out, "%snova: %snote%s%s: stopped after %d errors; --error-limit=0 shows them all%s\n", st.bold(),
                st.severity(Severity::Note), st.reset(), st.bold(), errors, st.reset());
    }
    if (errors + warningCount > 1)
    {
        string summary;
        if (errors) summary = to_string(errors) + (errors == 1 ? " error" : " errors");
        if (warningCount)
        {
            summary += (summary.empty() ? "" : " and ") + to_string(warningCount) +
                       (warningCount == 1 ? " warning" : " warnings");
        }
        fprintf(out, "%s.\n", summary.c_str());
    }
    fflush(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
    // A JSON string. Bytes that are not valid UTF-8 (a stray byte in a source file) become U+FFFD, so the document
    // is always valid JSON.
    string json_string(const string& s)
    {
        string out = "\"";
        size_t i = 0;
        while (i < s.size())
        {
            unsigned char c = (unsigned char)s[i];
            if (c < 0x80)
            {
                switch (c)
                {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20)
                        {
                            char buf[8];
                            snprintf(buf, sizeof buf, "\\u%04x", c);
                            out += buf;
                        }
                        else out += (char)c;
                }
                i++;
                continue;
            }
            int len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            bool valid = len > 0 && i + len <= s.size();
            for (int k = 1; valid && k < len; k++) valid = ((unsigned char)s[i + k] & 0xC0) == 0x80;
            if (valid)
            {
                out.append(s, i, len);
                i += len;
            }
            else
            {
                out += "\xEF\xBF\xBD";
                i++;
            }
        }
        return out + "\"";
    }

    string json_position(int line, int column)
    {
        return "{\"line\":" + to_string(line) + ",\"column\":" + to_string(column) + "}";
    }

    // "file", "range" (end exclusive) and "caret"; positions are 1-based, columns in bytes
    string json_location(const Diagnostic& d)
    {
        if (d.loc)
        {
            PresumedLoc b = sourceManager.presumed(d.range.begin), e = sourceManager.presumed(d.range.end);
            PresumedLoc c = sourceManager.presumed(d.loc);
            if (e.file != b.file) e = b;
            return "\"file\":" + json_string(c.file) + ",\"range\":{\"start\":" + json_position(b.line, b.column) +
                   ",\"end\":" + json_position(e.line, e.column) + "},\"caret\":" + json_position(c.line, c.column);
        }
        if (!d.written.file.empty() && d.written.line > 0)
        {
            string p = json_position(d.written.line, 1);
            return "\"file\":" + json_string(d.written.file) + ",\"range\":{\"start\":" + p + ",\"end\":" + p +
                   "},\"caret\":" + p;
        }
        string file = d.written.file.empty() ? "null" : json_string(d.written.file);
        return "\"file\":" + file + ",\"range\":null,\"caret\":null";
    }

    string json_diagnostic(const Diagnostic& d)
    {
        const DiagInfo& info = diag_info(d.code);
        string s = "{\"severity\":\"" + string(severity_word(d.severity)) + "\"";
        if (d.severity != Severity::Note)
        {
            s += ",\"code\":\"" + string(info.code) + "\",\"title\":" + json_string(info.title);
        }
        s += ",\"message\":" + json_string(d.message) + "," + json_location(d);
        if (d.severity != Severity::Note)
        {
            s += ",\"notes\":[";
            for (size_t i = 0; i < d.notes.size(); i++) s += (i ? "," : "") + json_diagnostic(d.notes[i]);
            s += "],\"fixes\":[";
            for (size_t i = 0; i < d.fixes.size(); i++)
            {
                const FixIt& f = d.fixes[i];
                PresumedLoc b = sourceManager.presumed(f.range.begin), e = sourceManager.presumed(f.range.end);
                s += string(i ? "," : "") + "{\"range\":{\"start\":" + json_position(b.line, b.column) + ",\"end\":" +
                     json_position(e.line, e.column) + "},\"replacement\":" + json_string(f.replacement) + "}";
            }
            s += "]";
        }
        return s + "}";
    }
}

void DiagnosticEngine::print_json(FILE* out) const
{
    int warningCount = 0;
    string s = "{\"version\":1,\"diagnostics\":[";
    bool first = true;
    for (const Diagnostic* d : in_source_order(diags))
    {
        s += (first ? "" : ",") + json_diagnostic(*d);
        first = false;
        if (d->severity == Severity::Warning) warningCount++;
    }
    s += "],\"errors\":" + to_string(errors) + ",\"warnings\":" + to_string(warningCount) +
         ",\"truncated\":" + (limitHit ? "true" : "false") + "}\n";
    fputs(s.c_str(), out);
    fflush(out);
}
