#ifndef NOVA_SOURCE_H
#define NOVA_SOURCE_H
// Source text and locations. Every text the compiler reads is one buffer in the SourceManager, placed at its own
// range of a single 32-bit offset space, so one SourceLoc (a uint32_t) names any character of any buffer, as
// Clang's SourceLocation does. Lines and columns are worked out only when a location is printed.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef uint32_t SourceLoc;  // 0: no location

// Where a location is, as a person reads it: the file, and the line and column (1-based; the column counts bytes).
// The file and line follow "#@line N "file"" directives: the import preprocessor writes them where it inlines a
// module, so a module's lines keep their own file and numbers.
struct PresumedLoc
{
    std::string file;
    int line = 0;
    int column = 0;
};

class SourceManager
{
    public:
        // Adds a buffer (it keeps its own copy of text) and returns its first location. The text is followed by a
        // '\0', so a scanner can stop on it without checking the length.
        SourceLoc add_buffer(const std::string& name, std::string text);
        const char* text_at(SourceLoc loc) const;  // the character at loc, and the rest of its buffer after it
        // A "#@line N" directive at loc: the line after the one holding loc is line N of file (the file so far when
        // file is empty).
        void add_line_directive(SourceLoc loc, int line, const std::string& file = "");
        PresumedLoc presumed(SourceLoc loc) const;
        std::string buffer_name(SourceLoc loc) const;  // the name loc's buffer was added with (its main file)
        // The physical line that holds loc, without its line break, and the location of its first character
        std::string_view line_text(SourceLoc loc, SourceLoc* lineStart = nullptr) const;
        void clear();  // forgets every buffer (a language server or a fuzzer reuses one SourceManager)

    private:
        struct Directive
        {
            int physical;      // the first physical line it applies to
            int line;          // that line's number
            std::string file;
        };
        struct Buffer
        {
            std::string name;
            std::string text;
            SourceLoc start;
            std::vector<uint32_t> lineStarts;   // offset of each line's first character
            std::vector<Directive> directives;  // in the order of their lines
        };
        const Buffer* buffer_of(SourceLoc loc) const;
        std::vector<std::unique_ptr<Buffer>> buffers;    // by start; a buffer never moves once added
        SourceLoc next = 1;
};

extern SourceManager sourceManager;

#endif
