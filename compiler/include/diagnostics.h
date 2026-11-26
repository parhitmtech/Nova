#ifndef NOVA_DIAGNOSTICS_H
#define NOVA_DIAGNOSTICS_H
// Diagnostics: every error and warning the compiler reports goes through one engine, as Clang's DiagnosticsEngine
// does. A diagnostic has a stable code (diagnostic_codes.def), a severity, the source range it is about, a message,
// and optionally notes (other places that explain it) and fixes (an edit that would correct it, for an editor's
// quick fix). The compiler records diagnostics as it finds them and prints them when it stops, in source order:
//
//   program.nova:6:18: error[E0502]: argument 1 of 'square' expects int but got string
//       6 |     print(square("four"))
//         |                  ^~~~~~
//
// or, with --diagnostics=json, as one JSON document that the language server and other tools read.
//
// A location is a SourceLoc (source.h). Text the compiler has not loaded as a buffer (an 'orbit' line, a missing
// file) has a written-out file and line instead.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "source.h"

enum class Severity : uint8_t { Error, Warning, Note };

enum class DiagCode : uint16_t
{
#define DIAG(name, code, severity, title) name,
#include "diagnostic_codes.def"
#undef DIAG
};

struct DiagInfo
{
    const char* code;   // "E0502"
    Severity severity;
    const char* title;  // "argument of the wrong type"
};
const DiagInfo& diag_info(DiagCode code);
const std::vector<DiagInfo>& all_diag_infos();  // every code, in the order of diagnostic_codes.def

struct SourceRange
{
    SourceLoc begin = 0;
    SourceLoc end = 0;  // one past the last character
};

struct FixIt
{
    SourceRange range;         // the text to replace (empty: insert at begin)
    std::string replacement;
};

struct Diagnostic
{
    Diagnostic(Severity severity = Severity::Error, DiagCode code = DiagCode(0), SourceLoc loc = 0)
        : severity(severity), code(code), loc(loc) {}

    Severity severity;
    DiagCode code;
    SourceLoc loc = 0;         // where the caret goes
    SourceRange range;         // what is underlined; at least loc's character
    std::string message;
    PresumedLoc written;       // the location when there is no SourceLoc (file "" when there is none at all)
    std::vector<Diagnostic> notes;
    std::vector<FixIt> fixes;
};

class DiagnosticEngine
{
    public:
        // Reports a diagnostic at loc, underlining range. It returns the diagnostic so a note or a fix can be added
        // to it at once. It is not kept (and the returned one is a scratch copy) when the error limit was reached,
        // when warnings are off, or when the same line already has an error (only the first error of a line is
        // reported: the ones after it are usually its consequences).
        Diagnostic& report(DiagCode code, SourceLoc loc, SourceRange range, std::string message);
        Diagnostic& report(DiagCode code, SourceLoc loc, uint32_t length, std::string message)
        {
            return report(code, loc, SourceRange{loc, loc + length}, std::move(message));
        }
        // A diagnostic with a written-out location (no loaded buffer); line 0: none at all
        Diagnostic& report_at(DiagCode code, const std::string& file, int line, std::string message);
        static void add_note(Diagnostic& d, SourceLoc loc, uint32_t length, std::string message);

        // Syntax errors (E01, E02) inside range are not reported: the scanner marks the text after a bracket that was
        // never closed, where everything the parser could say follows from the one error already reported.
        void add_quiet_range(SourceRange range) { quiet.push_back(range); }

        bool has_errors() const { return errors > 0; }
        int error_count() const { return errors; }
        bool limit_reached() const { return errorLimit > 0 && errors >= errorLimit; }
        bool line_has_error(SourceLoc loc) const;
        const std::vector<Diagnostic>& all() const { return diags; }

        void set_error_limit(int limit) { errorLimit = limit; }  // 0: no limit
        void set_warnings(bool on) { warnings = on; }

        // Prints every diagnostic kept, in source order. color: ANSI colors (for a terminal)
        void print_text(FILE* out, bool color) const;
        void print_json(FILE* out) const;
        // How print() prints: as set from the command line, so a runtime error comes out as the compile errors did
        void set_output(bool asJson, bool withColor) { json = asJson; color = withColor; }
        void print(FILE* out) const { if (json) print_json(out); else print_text(out, color); }
        void clear();

    private:
        std::vector<Diagnostic> diags;
        std::vector<std::pair<std::string, int>> errorLines;  // (file, line) of each error kept
        std::vector<SourceRange> quiet;
        Diagnostic scratch;
        int errors = 0;
        int errorLimit = 20;
        bool warnings = true;
        bool limitHit = false;  // an error was dropped because of the limit
        bool json = false, color = false;
};

extern DiagnosticEngine diagnostics;

// Whether stderr is a terminal that shows colors (and NO_COLOR is not set); on Windows it also turns the console's
// escape sequences on.
bool stderr_supports_color();

#endif  // NOVA_DIAGNOSTICS_H
