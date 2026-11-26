// The language server's JSON (json.h): a recursive-descent reader and a writer.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "json.h"

using namespace std;

static const string emptyString;
static const Json nullJson;

const string& Json::as_string() const { return kind == Kind::String ? text : emptyString; }

const Json& Json::operator[](const string& key) const
{
    if (kind != Kind::Object) return nullJson;
    for (const auto& m : members) if (m.first == key) return m.second;
    return nullJson;
}

bool Json::has(const string& key) const
{
    if (kind != Kind::Object) return false;
    for (const auto& m : members) if (m.first == key) return true;
    return false;
}

Json& Json::set(const string& key, Json value)
{
    if (kind != Kind::Object)
    {
        *this = object();
    }
    for (auto& m : members)
    {
        if (m.first == key)
        {
            m.second = std::move(value);
            return *this;
        }
    }
    members.emplace_back(key, std::move(value));
    return *this;
}

Json& Json::push(Json value)
{
    if (kind != Kind::Array) *this = array();
    elements.push_back(std::move(value));
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
// Writing
// ─────────────────────────────────────────────────────────────────────────────

static void write_string(string& out, const string& s)
{
    out += '"';
    for (unsigned char c : s)
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
                else out += (char)c;  // UTF-8 passes through; the server only sends text it was sent or wrote
        }
    }
    out += '"';
}

void Json::dump_to(string& out) const
{
    switch (kind)
    {
        case Kind::Null: out += "null"; return;
        case Kind::Bool: out += boolean ? "true" : "false"; return;
        case Kind::Number:
        {
            char buf[32];
            if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 1e15)
                snprintf(buf, sizeof buf, "%lld", (long long)number);
            else if (std::isfinite(number))
                snprintf(buf, sizeof buf, "%.17g", number);
            else
                snprintf(buf, sizeof buf, "null");
            out += buf;
            return;
        }
        case Kind::String: write_string(out, text); return;
        case Kind::Array:
        {
            out += '[';
            for (size_t i = 0; i < elements.size(); i++)
            {
                if (i) out += ',';
                elements[i].dump_to(out);
            }
            out += ']';
            return;
        }
        case Kind::Object:
        {
            out += '{';
            for (size_t i = 0; i < members.size(); i++)
            {
                if (i) out += ',';
                write_string(out, members[i].first);
                out += ':';
                members[i].second.dump_to(out);
            }
            out += '}';
            return;
        }
    }
}

string Json::dump() const
{
    string out;
    dump_to(out);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reading
// ─────────────────────────────────────────────────────────────────────────────

class JsonReader
{
    public:
        explicit JsonReader(const string& s) : s(s) {}

        bool read(Json& out, string& error)
        {
            skip();
            if (!value(out, 0)) { error = message.empty() ? "invalid JSON" : message; return false; }
            skip();
            if (i != s.size()) { error = "text after the JSON value"; return false; }
            return true;
        }

    private:
        const string& s;
        size_t i = 0;
        string message;

        void skip() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
        bool fail(const char* m) { message = m; return false; }

        bool literal(const char* word)
        {
            size_t n = 0;
            while (word[n]) n++;
            if (s.compare(i, n, word) != 0) return false;
            i += n;
            return true;
        }

        bool value(Json& out, int depth)
        {
            if (depth > 200) return fail("JSON nested too deeply");
            if (i >= s.size()) return fail("unexpected end of JSON");
            char c = s[i];
            if (c == '{') return object(out, depth);
            if (c == '[') return array(out, depth);
            if (c == '"')
            {
                out = Json(string());
                return string_value(out.text);
            }
            if (c == 't' && literal("true")) { out = Json(true); return true; }
            if (c == 'f' && literal("false")) { out = Json(false); return true; }
            if (c == 'n' && literal("null")) { out = Json(); return true; }
            if (c == '-' || (c >= '0' && c <= '9')) return number(out);
            return fail("unexpected character in JSON");
        }

        bool number(Json& out)
        {
            size_t start = i;
            if (s[i] == '-') i++;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    s[i] == '+' || s[i] == '-')) i++;
            string t = s.substr(start, i - start);
            char* end = nullptr;
            double d = strtod(t.c_str(), &end);
            if (!end || *end != '\0' || t.empty()) return fail("invalid number in JSON");
            out = Json(d);
            return true;
        }

        static void append_utf8(string& out, uint32_t cp)
        {
            if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000)
            {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            else
            {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }

        bool hex4(uint32_t& v)
        {
            if (i + 4 > s.size()) return false;
            v = 0;
            for (int k = 0; k < 4; k++)
            {
                char c = s[i++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= c - '0';
                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                else return false;
            }
            return true;
        }

        bool string_value(string& out)
        {
            i++;  // the opening quote
            while (i < s.size())
            {
                char c = s[i++];
                if (c == '"') return true;
                if (c != '\\')
                {
                    out += c;
                    continue;
                }
                if (i >= s.size()) break;
                char e = s[i++];
                switch (e)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t cp;
                        if (!hex4(cp)) return fail("invalid \\u escape in JSON");
                        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                        {
                            i += 2;
                            uint32_t lo;
                            if (!hex4(lo)) return fail("invalid \\u escape in JSON");
                            if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: return fail("invalid escape in a JSON string");
                }
            }
            return fail("unterminated JSON string");
        }

        bool array(Json& out, int depth)
        {
            out = Json::array();
            i++;
            skip();
            if (i < s.size() && s[i] == ']') { i++; return true; }
            while (true)
            {
                Json v;
                skip();
                if (!value(v, depth + 1)) return false;
                out.elements.push_back(std::move(v));
                skip();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; return true; }
                return fail("expected ',' or ']' in a JSON array");
            }
        }

        bool object(Json& out, int depth)
        {
            out = Json::object();
            i++;
            skip();
            if (i < s.size() && s[i] == '}') { i++; return true; }
            while (true)
            {
                skip();
                if (i >= s.size() || s[i] != '"') return fail("expected a key in a JSON object");
                string key;
                if (!string_value(key)) return false;
                skip();
                if (i >= s.size() || s[i] != ':') return fail("expected ':' in a JSON object");
                i++;
                skip();
                Json v;
                if (!value(v, depth + 1)) return false;
                out.members.emplace_back(std::move(key), std::move(v));
                skip();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; return true; }
                return fail("expected ',' or '}' in a JSON object");
            }
        }
};

Json Json::parse(const string& text, string* error)
{
    Json out;
    string e;
    if (!JsonReader(text).read(out, e))
    {
        if (error) *error = e;
        return Json();
    }
    if (error) error->clear();
    return out;
}
