// A libFuzzer target for the front end: any bytes go through the scanner, the parser and (when they parse) the check
// pass, and the diagnostics are printed both ways. The front end runs on every keystroke in an editor, so whatever
// the bytes, it must not crash, hang, or read out of bounds: AddressSanitizer and UndefinedBehaviorSanitizer turn any
// of those into a failure. Built and run by the Makefile in this directory (make -C compiler/fuzz run).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "check.h"
#include "diagnostics.h"
#include "lexer.h"
#include "parse_tree.h"

static FILE* sink()
{
    static FILE* f = fopen("/dev/null", "w");
    return f;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    diagnostics.clear();
    diagnostics.set_error_limit(20);
    sourceManager.clear();
    std::string source(reinterpret_cast<const char*>(data), size);
    std::vector<LexToken> tokens = scan_program(source, "fuzz.nova");
    ParseTree tree = build_parse_tree(tokens);
    if (!diagnostics.has_errors()) check_tree(tree);
    else tree.range(tree.root());  // the tree is whole even with errors in it
    diagnostics.print_text(sink(), true);
    diagnostics.print_json(sink());
    return 0;
}
