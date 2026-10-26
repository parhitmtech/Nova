// SourceManager: every source buffer at its own range of one 32-bit location space (see source.h).
#include <algorithm>
#include <cstring>
#include "source.h"

using namespace std;

SourceManager sourceManager;

SourceLoc SourceManager::add_buffer(const string& name, string text)
{
    unique_ptr<Buffer> b(new Buffer());
    b->name = name;
    b->text = move(text);
    b->start = next;
    b->lineStarts.push_back(0);
    for (const char* p = b->text.c_str(); (p = strchr(p, '\n')) != nullptr; p++)
    {
        b->lineStarts.push_back((uint32_t)(p - b->text.c_str()) + 1);
    }
    next += (SourceLoc)b->text.size() + 1;  // +1: the '\0' after the text has a location too (end of file)
    buffers.push_back(move(b));
    return buffers.back()->start;
}

const SourceManager::Buffer* SourceManager:: buffer_of(SourceLoc loc) const
{
    // the last buffer that starts at or before loc
    auto it = upper_bound(buffers.begin(), buffers.end(), loc,
                          [](SourceLoc l, const unique_ptr<Buffer>& b) { return l < b->start; });
    return (it == buffers.begin()) ? nullptr : (it - 1)->get();
}

const char* SourceManager::text_at(SourceLoc loc) const
{
    const Buffer* b = buffer_of(loc);
    return b ? b->text.c_str() + (loc - b->start) : "";
}

void SourceManager::add_line_directive(SourceLoc loc, int line)
{
    Buffer* b = const_cast<Buffer*>(buffer_of(loc));
    if (!b) return;
    uint32_t offset = loc - b->start;
    int physical = (int)(upper_bound(b->lineStarts.begin(), b->lineStarts.end(), offset) - b->lineStarts.begin());
    b->directives.push_back({physical + 1, line});  // physical is 1-based: the directive's own line
}

PresumedLoc SourceManager::presumed(SourceLoc loc) const 
{
    PresumedLoc p;
    const Buffer* b = buffer_of(loc);
    if (!b) return p;
    uint32_t offset = loc - b->start;
    auto it = upper_bound(b->lineStarts.begin(), b->lineStarts.end(), offset);
    int physical = (int)(it - b->lineStarts.begin());  // 1-based
    p.file = b->name;
    p.column = (int)(offset - *(it - 1)) + 1;
    p.line = physical;
    for (auto d = b->directives.rbegin(); d != b->directives.rend(); ++d)
    {
        if (d->first <= physical)
        {
            p.line = d->second + (physical - d->first);
            break;
        }
    }
    return p;
}