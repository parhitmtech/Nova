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
    b->text = std::move(text);
    b->start = next;
    b->lineStarts.push_back(0);
    for (const char* p = b->text.c_str(); (p = strchr(p, '\n')) != nullptr; p++)
    {
        b->lineStarts.push_back((uint32_t)(p - b->text.c_str()) + 1);
    }
    next += (SourceLoc)b->text.size() + 1;  // +1: the '\0' after the text has a location too (end of file)
    buffers.push_back(std::move(b));
    return buffers.back()->start;
}

void SourceManager::clear()
{
    buffers.clear();
    next = 1;
}

const SourceManager::Buffer* SourceManager::buffer_of(SourceLoc loc) const
{
    if (loc == 0) return nullptr;
    // the last buffer that starts at or before loc
    auto it = upper_bound(buffers.begin(), buffers.end(), loc,
                          [](SourceLoc l, const unique_ptr<Buffer>& b) { return l < b->start; });
    if (it == buffers.begin()) return nullptr;
    const Buffer* b = (it - 1)->get();
    return loc - b->start <= b->text.size() ? b : nullptr;
}

const char* SourceManager::text_at(SourceLoc loc) const
{
    const Buffer* b = buffer_of(loc);
    return b ? b->text.c_str() + (loc - b->start) : "";
}

void SourceManager::add_line_directive(SourceLoc loc, int line, const string& file)
{
    Buffer* b = const_cast<Buffer*>(buffer_of(loc));
    if (!b) return;
    uint32_t offset = loc - b->start;
    int physical = (int)(upper_bound(b->lineStarts.begin(), b->lineStarts.end(), offset) - b->lineStarts.begin());
    string name = file.empty() ? presumed(loc).file : file;
    b->directives.push_back({physical + 1, line, name});  // physical is 1-based: the directive's own line
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
        if (d->physical <= physical)
        {
            p.line = d->line + (physical - d->physical);
            p.file = d->file;
            break;
        }
    }
    return p;
}

string SourceManager::buffer_name(SourceLoc loc) const
{
    const Buffer* b = buffer_of(loc);
    return b ? b->name : string();
}

string_view SourceManager::line_text(SourceLoc loc, SourceLoc* lineStart) const
{
    const Buffer* b = buffer_of(loc);
    if (!b) return {};
    uint32_t offset = loc - b->start;
    auto it = upper_bound(b->lineStarts.begin(), b->lineStarts.end(), offset);
    uint32_t first = *(it - 1);
    uint32_t last = first;
    while (last < b->text.size() && b->text[last] != '\n') last++;
    if (last > first && b->text[last - 1] == '\r') last--;
    if (lineStart) *lineStart = b->start + first;
    return string_view(b->text.data() + first, last - first);
}
