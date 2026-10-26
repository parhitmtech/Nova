#ifndef NOVA_SOURCE_H
#define NOVA_SOURCE_H
// Source text and locations. Every text the compiler reads is one buffer in the SourceManager, placed at its own
// range of a single 32-bit offset space, so one SourceLoc (a uint32_t) names any character of any buffer, as
// Clang's SourceLocation does. Lines and columnsare worked out only when a location is printed.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef uint32_t SourceLoc;  // 0: no location

// Where a location is, as a person reads it: the buffer's name, and the line and column. The line follows
// "#@line N" directives (the import preprocessor writes them so that a file's lines keep their own numbers).
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
        // "#@line N" at loc: the line after the one holding loc is line N.
        void add_line_directive(SourceLoc loc, int line);
        PresumedLoc presumed(SourceLoc loc) const;

    private:
        struct Buffer 
        {
            std::string name;
            std::string text;
            SourceLoc start;
            std::vector<uint32_t> lineStarts;                // offset of each line's first character
            std::vector<std::pair<int, int>> directives;     // (physical line the directive applies from, its line)
        };
        const Buffer* buffer_of(SourceLoc loc) const;
        std::vector<std::unique_ptr<Buffer>> buffers;    // by start; a buffer never moves once added
        SourceLoc next = 1;
};

extern SourceManager sourceManager;

#endif