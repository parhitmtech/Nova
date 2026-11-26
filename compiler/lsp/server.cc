// nova-lsp: Nova's language server. An editor starts it and talks to it over stdin and stdout in the Language
// Server Protocol (JSON-RPC messages, each after a Content-Length header). The server runs the compiler's own front
// end on each document as it changes (analysis.h), sends the errors and warnings back, and answers hover, go to
// definition, outline, completion and signature help.
//
// Documents are sent whole on every change (text document sync "full"): Nova files are small and the front end
// re-reads one in milliseconds, so the simplest sync is also the most robust. Diagnostics in an imported module are
// published to that module's file.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include "analysis.h"
#include "json.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std;

static const char* const VERSION = "0.1.0";
static const char* const ERRORS_URL = "https://github.com/parhitmtech/Nova/blob/main/docs/ERRORS.md";

// ─────────────────────────────────────────────────────────────────────────────
// File names and URIs
// ─────────────────────────────────────────────────────────────────────────────

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// file:///home/a/b.nova -> /home/a/b.nova; file:///c%3A/a/b.nova -> c:/a/b.nova
static string uri_to_path(const string& uri)
{
    string s = uri.compare(0, 7, "file://") == 0 ? uri.substr(7) : uri;
    string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0)
        {
            out += (char)(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        }
        else out += s[i];
    }
    // a Windows drive: /c:/a -> c:/a
    if (out.size() >= 3 && out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':') out.erase(0, 1);
    return out;
}

static string path_to_uri(const string& path)
{
    string p = path;
    for (char& c : p) if (c == '\\') c = '/';
    string out = "file://";
    if (!p.empty() && p[0] != '/') out += '/';  // c:/a -> file:///c%3A/a
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : p)
    {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// GitHub's anchor for a heading of ERRORS.md: "### E0502: argument of the wrong type" -> e0502-argument-of-the-wrong-type
static string errors_anchor(const DiagInfo& info)
{
    string heading = string(info.code) + ": " + info.title, out;
    for (unsigned char c : heading)
    {
        if (isalnum(c)) out += (char)tolower(c);
        else if (c == ' ' || c == '-') out += '-';
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// The server
// ─────────────────────────────────────────────────────────────────────────────

class Server
{
    public:
        int run();

    private:
        struct Document
        {
            string uri, path, text;
            shared_ptr<Analysis> latest;  // the text as it is now
            shared_ptr<Analysis> good;    // the latest that parsed and was checked, for hover and completion
            set<string> published;        // the URIs it published diagnostics for
        };
        map<string, Document> documents;  // by URI
        bool shutdownRequested = false;

        bool read_message(string& body);
        void send(const Json& message);
        void respond(const Json& id, Json result);
        void respond_error(const Json& id, int code, const string& message);
        void notify(const string& method, Json params);
        void log(const string& text);

        void handle(const Json& message);
        Json initialize(const Json& params);
        void analyze(Document& doc);
        void publish(Document& doc);

        Json position_json(const Analysis& a, const string& file, Position p) const;
        Json range_json(const Analysis& a, const string& file, Range r) const;
        Position position_from(const Document& doc, const Json& p) const;
        const Analysis* semantic(const Document& doc) const;  // the analysis that answers questions about meaning

        Json hover(const Json& params);
        Json definition(const Json& params);
        Json document_symbols(const Json& params);
        Json completion(const Json& params);
        Json signature_help(const Json& params);
};

bool Server::read_message(string& body)
{
    size_t length = 0;
    bool haveLength = false;
    string line;
    while (true)  // the headers, up to an empty line
    {
        line.clear();
        int c;
        while ((c = getchar()) != EOF && c != '\n') line += (char)c;
        if (c == EOF) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.compare(0, 15, "Content-Length:") == 0)
        {
            length = (size_t)strtoull(line.c_str() + 15, nullptr, 10);
            haveLength = true;
        }
    }
    if (!haveLength) return true;  // a message without a length: skip it
    body.resize(length);
    size_t got = fread(&body[0], 1, length, stdin);
    return got == length;
}

void Server::send(const Json& message)
{
    string body = message.dump();
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
    fwrite(body.data(), 1, body.size(), stdout);
    fflush(stdout);
}

void Server::respond(const Json& id, Json result)
{
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
    send(m);
}

void Server::respond_error(const Json& id, int code, const string& message)
{
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("error", Json::object().set("code", code).set("message", message));
    send(m);
}

void Server::notify(const string& method, Json params)
{
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
    send(m);
}

void Server::log(const string& text)
{
    fprintf(stderr, "nova-lsp: %s\n", text.c_str());
    fflush(stderr);
}

int Server::run()
{
    string body;
    while (read_message(body))
    {
        if (body.empty()) continue;
        string error;
        Json message = Json::parse(body, &error);
        if (!message.is_object())
        {
            respond_error(Json(), -32700, "parse error: " + error);
            continue;
        }
        handle(message);
        if (message["method"].as_string() == "exit") return shutdownRequested ? 0 : 1;
    }
    return shutdownRequested ? 0 : 1;  // the editor closed the pipe
}

void Server::handle(const Json& message)
{
    const string& method = message["method"].as_string();
    const Json& id = message["id"];
    const Json& params = message["params"];
    bool request = message.has("id");

    if (method == "initialize") return respond(id, initialize(params));
    if (method == "initialized" || method == "exit") return;
    if (method == "shutdown")
    {
        shutdownRequested = true;
        return respond(id, Json());
    }
    if (method == "textDocument/didOpen")
    {
        const Json& td = params["textDocument"];
        Document& doc = documents[td["uri"].as_string()];
        doc.uri = td["uri"].as_string();
        doc.path = uri_to_path(doc.uri);
        doc.text = td["text"].as_string();
        analyze(doc);
        return;
    }
    if (method == "textDocument/didChange")
    {
        auto it = documents.find(params["textDocument"]["uri"].as_string());
        if (it == documents.end()) return;
        const Json& changes = params["contentChanges"];
        if (!changes.items().empty()) it->second.text = changes.items().back()["text"].as_string();  // full sync
        analyze(it->second);
        return;
    }
    if (method == "textDocument/didClose")
    {
        auto it = documents.find(params["textDocument"]["uri"].as_string());
        if (it == documents.end()) return;
        for (const string& uri : it->second.published)
        {
            notify("textDocument/publishDiagnostics", Json::object().set("uri", uri).set("diagnostics", Json::array()));
        }
        documents.erase(it);
        return;
    }
    if (method == "textDocument/hover") return respond(id, hover(params));
    if (method == "textDocument/definition") return respond(id, definition(params));
    if (method == "textDocument/documentSymbol") return respond(id, document_symbols(params));
    if (method == "textDocument/completion") return respond(id, completion(params));
    if (method == "textDocument/signatureHelp") return respond(id, signature_help(params));
    if (request) respond_error(id, -32601, "method not found: " + method);  // a notification nobody needs is ignored
}

Json Server::initialize(const Json& params)
{
    // NOVA_HOME (where 'orbit' finds the standard library): from the editor, or the environment nova-lsp started in
    const string& home = params["initializationOptions"]["novaHome"].as_string();
    if (!home.empty())
    {
#ifdef _WIN32
        _putenv_s("NOVA_HOME", home.c_str());
#else
        setenv("NOVA_HOME", home.c_str(), 1);
#endif
    }
    Json caps = Json::object();
    caps.set("textDocumentSync", Json::object().set("openClose", true).set("change", 1));
    caps.set("hoverProvider", true);
    caps.set("definitionProvider", true);
    caps.set("documentSymbolProvider", true);
    caps.set("completionProvider", Json::object().set("triggerCharacters", Json::array().push(".")));
    caps.set("signatureHelpProvider",
             Json::object().set("triggerCharacters", Json::array().push("(").push(",")));
    Json result = Json::object();
    result.set("capabilities", caps);
    result.set("serverInfo", Json::object().set("name", "nova-lsp").set("version", VERSION));
    return result;
}

void Server::analyze(Document& doc)
{
    // Locations are 32 bits and every run uses a fresh range: near the end, start over from an empty space
    if (sourceManager.next_location() > 0xC0000000u)
    {
        for (auto& d : documents)
        {
            d.second.latest.reset();
            d.second.good.reset();
        }
        sourceManager.clear();
        for (auto& d : documents) if (&d.second != &doc) analyze(d.second);
    }
    doc.latest = make_shared<Analysis>(doc.path, doc.text);
    if (doc.latest->checked()) doc.good = doc.latest;
    publish(doc);
}

Json Server::position_json(const Analysis& a, const string& file, Position p) const
{
    int character = file == a.path() ? Analysis::byte_to_utf16(a.text(), p.line, p.column) : p.column;
    return Json::object().set("line", max(p.line, 0)).set("character", max(character, 0));
}

Json Server::range_json(const Analysis& a, const string& file, Range r) const
{
    return Json::object().set("start", position_json(a, file, r.start)).set("end", position_json(a, file, r.end));
}

void Server::publish(Document& doc)
{
    const Analysis& a = *doc.latest;
    map<string, Json> byFile;  // file -> its diagnostics
    byFile[doc.path] = Json::array();
    for (const Diagnostic& d : a.diagnostics())
    {
        const DiagInfo& info = diag_info(d.code);
        Location where;
        if (d.loc)
        {
            if (!a.location_of(d.range.begin, d.range.end, where)) continue;
        }
        else
        {
            // a whole line (a module 'orbit' could not find), or no place (put on the first line)
            where.file = d.written.file.empty() ? doc.path : d.written.file;
            int line = max(d.written.line - 1, 0);
            where.range = Range{Position{line, 0}, Position{line, 1 << 20}};
        }
        Json diag = Json::object();
        diag.set("range", range_json(a, where.file, where.range));
        diag.set("severity", d.severity == Severity::Error ? 1 : 2);
        diag.set("code", info.code);
        diag.set("codeDescription", Json::object().set("href", string(ERRORS_URL) + "#" + errors_anchor(info)));
        diag.set("source", "nova");
        diag.set("message", d.message);
        if (d.code == DiagCode::UnusedVariable || d.code == DiagCode::UnreachableCode)
        {
            diag.set("tags", Json::array().push(1));  // "unnecessary": editors fade it
        }
        Json related = Json::array();
        for (const Diagnostic& n : d.notes)
        {
            Location nl;
            if (!n.loc || !a.location_of(n.range.begin, n.range.end, nl)) continue;
            related.push(Json::object()
                             .set("location", Json::object().set("uri", path_to_uri(nl.file))
                                                            .set("range", range_json(a, nl.file, nl.range)))
                             .set("message", n.message));
        }
        if (!related.items().empty()) diag.set("relatedInformation", related);
        Json fixes = Json::array();
        for (const FixIt& f : d.fixes)
        {
            Location fl;
            if (!a.location_of(f.range.begin, f.range.end, fl)) continue;
            fixes.push(Json::object().set("range", range_json(a, fl.file, fl.range)).set("newText", f.replacement));
        }
        if (!fixes.items().empty()) diag.set("data", Json::object().set("fixes", fixes));  // for quick fixes
        byFile[where.file].push(diag);
    }
    set<string> now;
    for (auto& f : byFile)
    {
        string uri = f.first == doc.path ? doc.uri : path_to_uri(f.first);
        now.insert(uri);
        notify("textDocument/publishDiagnostics", Json::object().set("uri", uri).set("diagnostics", f.second));
    }
    for (const string& uri : doc.published)  // files that had diagnostics and have none now
    {
        if (!now.count(uri))
            notify("textDocument/publishDiagnostics", Json::object().set("uri", uri).set("diagnostics", Json::array()));
    }
    doc.published = now;
}

Position Server::position_from(const Document& doc, const Json& p) const
{
    int line = p["line"].as_int();
    return Position{line, Analysis::utf16_to_byte(doc.text, line, p["character"].as_int())};
}

const Analysis* Server::semantic(const Document& doc) const
{
    if (doc.latest && doc.latest->checked()) return doc.latest.get();
    return doc.good ? doc.good.get() : doc.latest.get();
}

Json Server::hover(const Json& params)
{
    auto it = documents.find(params["textDocument"]["uri"].as_string());
    if (it == documents.end()) return Json();
    const Analysis* a = semantic(it->second);
    if (!a) return Json();
    bool found;
    HoverInfo h = a->hover(position_from(it->second, params["position"]), found);
    if (!found || h.code.empty()) return Json();
    string md = "```nova\n" + h.code + "\n```";
    if (!h.detail.empty()) md += "\n\n" + h.detail;
    Json result = Json::object();
    result.set("contents", Json::object().set("kind", "markdown").set("value", md));
    if (h.range.start.line >= 0) result.set("range", range_json(*a, a->path(), h.range));
    return result;
}

Json Server::definition(const Json& params)
{
    auto it = documents.find(params["textDocument"]["uri"].as_string());
    if (it == documents.end()) return Json();
    const Analysis* a = semantic(it->second);
    Location l;
    if (!a || !a->definition(position_from(it->second, params["position"]), l)) return Json();
    string uri = l.file == it->second.path ? it->second.uri : path_to_uri(l.file);
    return Json::object().set("uri", uri).set("range", range_json(*a, l.file, l.range));
}

static int symbol_kind(DocumentSymbol::Kind k)
{
    switch (k)
    {
        case DocumentSymbol::Function: return 12;
        case DocumentSymbol::Method: return 6;
        case DocumentSymbol::Class: return 5;
        case DocumentSymbol::Struct: return 23;
        case DocumentSymbol::Field: return 8;
        case DocumentSymbol::Entry: return 12;
    }
    return 12;
}

Json Server::document_symbols(const Json& params)
{
    auto it = documents.find(params["textDocument"]["uri"].as_string());
    if (it == documents.end() || !it->second.latest) return Json::array();
    const Analysis& a = *it->second.latest;  // the outline follows the text as it is, errors and all
    Json out = Json::array();
    for (const DocumentSymbol& s : a.symbols())
    {
        Json sym = Json::object();
        sym.set("name", s.name).set("detail", s.detail).set("kind", symbol_kind(s.kind));
        sym.set("range", range_json(a, a.path(), s.range)).set("selectionRange", range_json(a, a.path(), s.selection));
        Json children = Json::array();
        for (const DocumentSymbol& c : s.children)
        {
            if (c.range.start.line < 0) continue;
            Json child = Json::object();
            child.set("name", c.name).set("detail", c.detail).set("kind", symbol_kind(c.kind));
            child.set("range", range_json(a, a.path(), c.range)).set("selectionRange", range_json(a, a.path(), c.selection));
            children.push(child);
        }
        sym.set("children", children);
        out.push(sym);
    }
    return out;
}

static int completion_kind(CompletionKind k)
{
    switch (k)
    {
        case CompletionKind::Keyword: return 14;
        case CompletionKind::Variable: return 6;
        case CompletionKind::Function: return 3;
        case CompletionKind::Method: return 2;
        case CompletionKind::Field: return 5;
        case CompletionKind::Class: return 7;
        case CompletionKind::Struct: return 22;
        case CompletionKind::Module: return 9;
        case CompletionKind::TypeParameter: return 25;
        case CompletionKind::Type: return 7;
    }
    return 1;
}

Json Server::completion(const Json& params)
{
    auto it = documents.find(params["textDocument"]["uri"].as_string());
    if (it == documents.end()) return Json::array();
    const Analysis* a = semantic(it->second);
    if (!a) return Json::array();
    Json out = Json::array();
    for (const CompletionItem& c : a->complete(it->second.text, position_from(it->second, params["position"])))
    {
        Json item = Json::object().set("label", c.label).set("kind", completion_kind(c.kind));
        if (!c.detail.empty()) item.set("detail", c.detail);
        out.push(item);
    }
    return out;
}

Json Server::signature_help(const Json& params)
{
    auto it = documents.find(params["textDocument"]["uri"].as_string());
    if (it == documents.end()) return Json();
    const Analysis* a = semantic(it->second);
    SignatureInfo s;
    if (!a || !a->signature(it->second.text, position_from(it->second, params["position"]), s)) return Json();
    Json ps = Json::array();
    for (const string& p : s.params) ps.push(Json::object().set("label", p));
    Json sig = Json::object().set("label", s.label).set("parameters", ps);
    int active = s.params.empty() ? 0 : min(s.active, (int)s.params.size() - 1);
    return Json::object().set("signatures", Json::array().push(sig)).set("activeSignature", 0).set("activeParameter", active);
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--version")
        {
            printf("nova-lsp %s\n", VERSION);
            return 0;
        }
        if (arg == "--stdio") continue;  // the only transport; editors pass it
        fprintf(stderr, "usage: nova-lsp [--stdio] [--version]\n");
        return 1;
    }
#ifdef _WIN32
    // The protocol counts bytes: no "\n" -> "\r\n" translation on the way in or out
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return Server().run();
}
