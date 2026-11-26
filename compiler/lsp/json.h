#ifndef NOVA_LSP_JSON_H
#define NOVA_LSP_JSON_H
// A small JSON value, reader and writer for the language server's messages: enough for the Language Server
// Protocol, with no library to install. Objects keep their keys in order; numbers are doubles (LSP's integers fit).

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Json
{
    public:
        enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

        Json() = default;
        Json(std::nullptr_t) {}
        Json(bool b) : kind(Kind::Bool), boolean(b) {}
        Json(int n) : kind(Kind::Number), number(n) {}
        Json(int64_t n) : kind(Kind::Number), number((double)n) {}
        Json(uint32_t n) : kind(Kind::Number), number(n) {}
        Json(size_t n) : kind(Kind::Number), number((double)n) {}
        Json(double n) : kind(Kind::Number), number(n) {}
        Json(const char* s) : kind(Kind::String), text(s) {}
        Json(std::string s) : kind(Kind::String), text(std::move(s)) {}

        static Json array() { Json j; j.kind = Kind::Array; return j; }
        static Json object() { Json j; j.kind = Kind::Object; return j; }

        Kind type() const { return kind; }
        bool is_null() const { return kind == Kind::Null; }
        bool is_object() const { return kind == Kind::Object; }
        bool is_array() const { return kind == Kind::Array; }
        bool is_string() const { return kind == Kind::String; }
        bool is_number() const { return kind == Kind::Number; }

        // Reading, with a fallback when the value is missing or of another kind
        bool as_bool(bool fallback = false) const { return kind == Kind::Bool ? boolean : fallback; }
        double as_number(double fallback = 0) const { return kind == Kind::Number ? number : fallback; }
        int as_int(int fallback = 0) const { return kind == Kind::Number ? (int)number : fallback; }
        const std::string& as_string() const;  // "" when not a string
        const std::vector<Json>& items() const { return elements; }
        const std::vector<std::pair<std::string, Json>>& fields() const { return members; }

        // obj["key"]: the member, or a null value when it is missing (reading never adds)
        const Json& operator[](const std::string& key) const;
        bool has(const std::string& key) const;

        // Building
        Json& set(const std::string& key, Json value);  // adds or replaces; returns *this to chain
        Json& push(Json value);

        std::string dump() const;
        // The value in text, or a null value with error set when it is not valid JSON
        static Json parse(const std::string& text, std::string* error = nullptr);

    private:
        Kind kind = Kind::Null;
        bool boolean = false;
        double number = 0;
        std::string text;
        std::vector<Json> elements;
        std::vector<std::pair<std::string, Json>> members;
        void dump_to(std::string& out) const;
        friend class JsonReader;
};

#endif
