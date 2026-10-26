// Interner: one copy of each spelling, named by a Symbol (see interner.h). The string slive in a bump-allocated
// arena of large chunks, and the table is open addressing with linear probing over Symbols.
#include <cstring>
#include "interner.h"

using namespace std;

Interner names;

static const size_t CHUNK = 64 * 1024;

// FNV-1a: short identifiers hash well with it, and it needs no setup
static uint32_t hash_of(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

Interner::Interner()
{
    entries.push_back({"", 0, 0, 0});  // Symbol 0
    slots.assign(1024, 0);
}

const char* Interner::store(const char* s, size_t n)
{
    if (chunkUsed + n > chunkSize)
    {
        chunkSize = n > CHUNK ? n : CHUNK;
        chunks.emplace_back(new char[chunkSize]);
        chunkUsed = 0;
    }
    char* p = chunks.back().get() + chunkUsed;
    memcpy(p, s, n);
    chunkUsed += n;
    return p;
}

void Interner::grow()
{
    vector<Symbol> bigger(slots.size() * 2, 0);
    size_t mask = bigger.size() - 1;
    for (Symbol sym = 1; sym < (Symbol)entries.size(); sym++)
    {
        size_t i = entries[sym].hash & mask;
        while (bigger[i]) i = (i + 1) & mask;
        bigger[i] = sym;
    }
    slots.swap(bigger);
}

Symbol Interner::intern(const char* s, size_t n)
{
    uint32_t h = hash_of(s, n);
    size_t mask = slots.size() - 1;
    for (size_t i = h & mask; ; i = (i + 1) & mask)
    {
        Symbol sym = slots[i];
        if (sym == 0)
        {
            sym = (Symbol)entries.size();
            entries.push_back({store(s, n), (uint32_t)n, h, 0});
            slots[i] = sym;
            if (entries.size() * 10 > slots.size() * 7) grow();  // keep the table at most 70% full
            return sym;
        }
        const Entry& e = entries[sym];
        if (e.hash == h && e.length == n && memcmp(e.text, s, n) == 0) return sym;
    }
}