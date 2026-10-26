#ifndef NOVA_INTERNER_H
#define NOVA_INTERNER_H
// Interned names. Every distinct spelling (an identifier, a keyword, a string literal's text) is stored once and
// named by a Symbol, a small integer: comparing two names is comparing two integers, and a table keyed by name can 
// be an array indexed by Symbol. Each symbol also carries a tag; the lexer uses it to mark keywords, so recognising
// a keyword costs the same single hash lookup that interning the identifier already does (as in Clang's
// IdentifierTable).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef uint32_t Symbol;  // 0: no symbol

class Interner
{
    public:
        Interner();
        Symbol intern(const char* s, size_t n);
        Symbol intern(std::string_view s) { return intern(s.data(), s.size()); }
        std::string_view text(Symbol s) const { return {entries[s].text, entries[s].length}; }
        int tag(Symbol s) const { return entries[s].tag; }
        void set_tag(Symbol s, int tag) { entries[s].tag = tag; }
        size_t size() const { return entries.size() - 1; }
    
    private:
        struct Entry
        {
            const char* text;
            uint32_t length;
            uint32_t hash;
            int tag;
        };
        const char* store(const char* s, size_t n);  // a copy in the arena (it never moves)
        void grow();
        std::vector<Entry> entries;    // by Symbol; [0] is "no symbol"
        std::vector<Symbol> slots;     // open addressing, a power of two in size; 0 is empty
        std::vector<std::unique_ptr<char[]>> chunks;    // the arena
        size_t chunkUsed = 0, chunkSize = 0;
};

extern Interner names;

#endif