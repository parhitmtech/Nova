// The step debugger (nova --debug; the protocol is described in debugger.h), and the benchmark's input replay.
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <io.h>      // _read
#else
#include <cerrno>
#include <unistd.h>  // read
#endif

#include "compiler.h"
#include "debugger.h"
#include "runtime.h"
#include "source.h"

using namespace std;

// ── Input replay buffer ───────────────────────────────────────────────────────
// Used only during benchmark mode — captures inputs from first run
// and replays them for subsequent timed runs
vector<int> input_replay_buffer;
int         input_replay_index = -1;  // -1 = live stdin mode

void debug(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

// ── What lowering records ─────────────────────────────────────────────────────
vector<DebugFunction> debug_functions;
map<int, DebugClass> debug_classes;

namespace
{
// ── Where an instruction is ───────────────────────────────────────────────────
// Its file (an id) and line, from its source location: the file and line a person reads, imports included. Worked
// out once per instruction.
struct Place
{
    int file = -1;
    int line = 0;
};
unordered_map<const InstructionNode*, Place> places;
map<string, int> fileIds;     // by the file name as the source manager has it
vector<string> fileKeys;      // by id: the name normalized, to compare with a breakpoint's file

// A file's absolute path, as the system writes it: what a frame tells the front end, which opens the file there
string full_path(const string& path)
{
    error_code ec;
    filesystem::path p = filesystem::weakly_canonical(filesystem::absolute(filesystem::path(path), ec), ec);
    return (ec ? filesystem::path(path) : p).make_preferred().string();
}

// A path as a key, to compare a breakpoint's file with a program's: absolute, with '/', and on Windows in lower case
// (its file names ignore case)
string path_key(const string& path)
{
    string key = filesystem::path(full_path(path)).generic_string();
#ifdef _WIN32
    for (char& c : key) c = (char)tolower((unsigned char)c);
#endif
    return key;
}

Place place_of(const InstructionNode* pc)
{
    auto it = places.find(pc);
    if (it != places.end()) return it->second;
    Place place;
    if (pc->loc)
    {
        PresumedLoc p = sourceManager.presumed(pc->loc);
        auto f = fileIds.find(p.file);
        if (f == fileIds.end())
        {
            f = fileIds.emplace(p.file, (int)fileKeys.size()).first;
            fileKeys.push_back(path_key(p.file));
        }
        place = {f->second, p.line};
    }
    places[pc] = place;
    return place;
}

// ── Breakpoints and stepping ──────────────────────────────────────────────────
map<string, set<int>> breakpoints;  // by file key

enum class Mode { Run, StepIn, Next, StepOut };
Mode mode = Mode::Run;
Place start;             // where the program last stopped, for next
int startDepth = 0;
Place last;              // the statement running, to see when the next one starts
int lastDepth = -1;
bool atEntry = false;    // the first stop of a session started with stepIn

bool breakpoint_at(Place p)
{
    if (breakpoints.empty() || p.file < 0) return false;
    auto it = breakpoints.find(fileKeys[p.file]);
    return it != breakpoints.end() && it->second.count(p.line);
}

// ── JSON ──────────────────────────────────────────────────────────────────────
string quote(const string& s)
{
    string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out + "\"";
}

void send(const string& json)
{
    fflush(stdout);  // what the program printed comes first
    printf("\x1e%s\n", json.c_str());
    fflush(stdout);
}

// ── Values ────────────────────────────────────────────────────────────────────
struct Value
{
    string name, type, value;
    vector<Value> children;
};

const size_t MaxChildren = 100;  // a tensor's rows, a container's elements: the first ones

string number(double v)
{
    char b[40];
    snprintf(b, sizeof b, "%.10g", v);
    return b;
}

string type_text(VarType t)
{
    switch (t)
    {
        case TYPE_INT: return "int";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_TENSOR: return "Tensor";
        case TYPE_VECTOR: return "vector";
        case TYPE_MAP: return "map";
        case TYPE_CLASS: return "object";
        default: return "";
    }
}

string elements(const Tensor& t, size_t from, size_t count)
{
    string s = "[";
    for (size_t i = 0; i < count && i < 20; i++) s += (i ? ", " : "") + number(t.data[from + i]);
    if (count > 20) s += ", ...";
    return s + "]";
}

Value describe(const string& name, VarType t, string typeName, double raw, int depth = 0);

void describe_tensor(Value& v, int h)
{
    if (h < 0 || h >= (int)tensor_heap.size())
    {
        v.value = "none";
        return;
    }
    if (tensor_heap[h].freed)  // the interpreter frees a tensor after its last use; the debugger does not change that
    {
        v.value = "(freed after its last use)";
        return;
    }
    const Tensor& t = tensor_heap[h];
    string shape = "(";
    for (int k = 0; k < t.ndim(); k++) shape += (k ? ", " : "") + to_string(t.shape[k]);
    shape += ")";
    // Its rows, each as its elements; a vector (rank 1), its elements
    size_t cols = (size_t)t.cols, rows = (size_t)t.rows;
    if (t.ndim() <= 1)
    {
        v.value = "Tensor " + shape + " " + elements(t, 0, t.data.size());
        for (size_t i = 0; i < t.data.size() && i < MaxChildren; i++)
            v.children.push_back({"[" + to_string(i) + "]", "double", number(t.data[i]), {}});
        return;
    }
    v.value = "Tensor " + shape + " " + (rows ? elements(t, 0, cols) : "[]") + (rows > 1 ? ", ..." : "");
    for (size_t i = 0; i < rows && i < MaxChildren; i++)
        v.children.push_back({"[" + to_string(i) + "]", "row", elements(t, i * cols, cols), {}});
}

void describe_container(Value& v, int h, int depth)
{
    bool isMap = false;
    VarType elem = TYPE_UNKNOWN;
    vector<pair<string, double>> items;
    if (!containers_describe(h, isMap, elem, items))
    {
        v.value = "none";
        return;
    }
    v.value = v.type + " (" + to_string(items.size()) + (items.size() == 1 ? " element)" : " elements)");
    for (size_t i = 0; i < items.size() && i < MaxChildren; i++)
    {
        string key = isMap ? items[i].first : "[" + items[i].first + "]";
        v.children.push_back(describe(key, elem, type_text(elem), items[i].second, depth + 1));
    }
}

void describe_object(Value& v, int h, int depth)
{
    int cls = 0;
    vector<double> fields;
    if (!objects_describe(h, cls, fields))
    {
        v.value = "none";
        return;
    }
    auto it = debug_classes.find(cls);
    string name = it != debug_classes.end() ? it->second.name : "object";
    v.value = name + " object";
    if (v.type.empty() || v.type == "object") v.type = name;
    if (it == debug_classes.end()) return;
    const DebugClass& c = it->second;
    for (size_t i = 0; i < fields.size() && i < c.fields.size(); i++)
        v.children.push_back(describe(c.fields[i], c.fieldTypes[i], type_text(c.fieldTypes[i]), fields[i], depth + 1));
}

// A value of type t: raw is what its slot (or an element, or a field) holds
Value describe(const string& name, VarType t, string typeName, double raw, int depth)
{
    Value v{name, typeName.empty() ? type_text(t) : typeName, "", {}};
    int h = (int)raw;
    switch (t)
    {
        case TYPE_BOOL: v.value = raw != 0 ? "true" : "false"; break;
        case TYPE_FLOAT:
        case TYPE_DOUBLE: v.value = number(raw); break;
        case TYPE_STRING:
            v.value = h >= 0 && h < (int)strMem.size() ? quote(strMem[h]) : "\"\"";
            break;
        case TYPE_TENSOR: describe_tensor(v, h); break;
        case TYPE_VECTOR:
        case TYPE_MAP:
            if (depth < 4) describe_container(v, h, depth);
            else v.value = v.type;
            break;
        case TYPE_CLASS:
            if (depth < 4) describe_object(v, h, depth);
            else v.value = v.type;
            break;
        default: v.value = to_string(h); break;
    }
    return v;
}

string json(const Value& v)
{
    string s = "{\"name\":" + quote(v.name) + ",\"type\":" + quote(v.type) + ",\"value\":" + quote(v.value);
    if (!v.children.empty())
    {
        s += ",\"children\":[";
        for (size_t i = 0; i < v.children.size(); i++) s += (i ? "," : "") + json(v.children[i]);
        s += "]";
    }
    return s + "}";
}

// ── Frames ────────────────────────────────────────────────────────────────────
// The call stack, outermost first. A function's int slots are saved by each call of it and put back when it returns,
// so a frame's values are in its slots unless a later call of the same function has saved them: then they are in the
// first such call's savedSlots. Float and double slots are not saved by calls; theirs are read as they are.
vector<CallFrame> call_frames()
{
    vector<CallFrame> calls;
    for (stack<CallFrame> s = callStack; !s.empty(); s.pop()) calls.push_back(s.top());
    return vector<CallFrame>(calls.rbegin(), calls.rend());
}

const DebugFunction* function_at(int slotBase)
{
    for (const DebugFunction& f : debug_functions)
        if (f.slotBase == slotBase) return &f;
    return nullptr;
}

double slot_value(const vector<CallFrame>& calls, int depth, const DebugFunction& f, const DebugVariable& var)
{
    if (var.type == TYPE_FLOAT) return var.slot < (int)fmem.size() ? fmem[var.slot] : 0;
    if (var.type == TYPE_DOUBLE) return var.slot < (int)dmem.size() ? dmem[var.slot] : 0;
    if (f.slotBase >= 0 && var.slot >= f.slotBase && var.slot < f.slotBase + f.slotCount)
    {
        for (size_t j = depth + 1; j < calls.size(); j++)
        {
            if (calls[j].slot_base != f.slotBase) continue;
            size_t i = (size_t)(var.slot - f.slotBase);
            return i < calls[j].savedSlots.size() ? calls[j].savedSlots[i] : 0;
        }
    }
    return var.slot < (int)mem.size() ? mem[var.slot] : 0;
}

// A frame: its function's variables in scope at loc (the innermost of a name, where blocks declare it twice)
string frame_json(const vector<CallFrame>& calls, int depth, const DebugFunction* f, SourceLoc loc)
{
    PresumedLoc p = loc ? sourceManager.presumed(loc) : PresumedLoc{};
    string s = "{\"name\":" + quote(f ? f->name : "?") + ",\"file\":" + quote(p.file.empty() ? "" : full_path(p.file)) +
               ",\"line\":" + to_string(p.line) + ",\"column\":" + to_string(p.column) + ",\"variables\":[";
    if (f)
    {
        map<string, const DebugVariable*> visible;
        vector<string> order;
        for (const DebugVariable& v : f->variables)
        {
            if (loc && (loc < v.scopeBegin || loc > v.scopeEnd)) continue;
            if (!visible.count(v.name)) order.push_back(v.name);
            visible[v.name] = &v;  // a later declaration in scope is an inner block's
        }
        bool first = true;
        for (const string& name : order)
        {
            const DebugVariable& v = *visible[name];
            s += (first ? "" : ",") + json(describe(v.name, v.type, v.typeName, slot_value(calls, depth, *f, v)));
            first = false;
        }
    }
    return s + "]}";
}

string frames_json(const InstructionNode* pc)
{
    vector<CallFrame> calls = call_frames();
    string s = "[";
    for (int i = (int)calls.size() - 1; i >= 0; i--)  // innermost first
    {
        const InstructionNode* at = i + 1 < (int)calls.size() ? calls[i + 1].callSite : pc;
        s += frame_json(calls, i, function_at(calls[i].slot_base), at ? at->loc : 0) + ",";
    }
    const InstructionNode* top = calls.empty() ? pc : calls[0].callSite;
    return s + frame_json(calls, -1, debug_functions.empty() ? nullptr : &debug_functions[0], top ? top->loc : 0) +
           "]";
}

// ── Commands ──────────────────────────────────────────────────────────────────
// A thread reads the front end's lines as they come, so the front end is heard while the program runs: "pause" sets a
// flag the program's thread checks before each instruction, and every other line waits in a queue. The program's
// thread takes lines from the queue at a stop, at input(), and while it runs whenever one is waiting (breakpoints
// change at once; stop ends the program). The queue is made once and never freed: at exit, the reader may still be
// waiting for a line.
struct Inbox
{
    mutex lock;
    condition_variable arrived;
    deque<string> lines;
};
Inbox* inbox = nullptr;
atomic<bool> waiting{false};     // a line is in the queue
atomic<bool> pauseAsked{false};  // "pause" came: stop at the next instruction with a place

// The end of input, in the queue: the front end has gone. It ends the program where a command is needed (a stop,
// input()), as "stop" does; a running program runs on to there.
const char* const EndOfInput = "\x04";

// The next line on stdin, without its line break; EndOfInput at the end
// It reads file descriptor 0 itself, never the stdin stream: a thread waiting in fgets holds the stream's lock, and
// exit() (which flushes every stream, taking each one's lock) would wait for it forever.
string read_stdin_line()
{
    static string pending;  // read, not yet returned
    for (;;)
    {
        size_t end = pending.find('\n');
        if (end != string::npos)
        {
            string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        char buf[4096];
#ifdef _WIN32
        int n = _read(0, buf, sizeof buf);
#else
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0)  // the end of input: what is left, then the end
        {
            if (pending.empty()) return EndOfInput;
            string line;
            line.swap(pending);
            return line;
        }
        pending.append(buf, (size_t)n);
    }
}

void read_commands()
{
    for (;;)
    {
        string line = read_stdin_line();
        if (line == "pause")
        {
            pauseAsked.store(true);
            continue;
        }
        {
            lock_guard<mutex> hold(inbox->lock);
            inbox->lines.push_back(line);
            waiting.store(true);
        }
        inbox->arrived.notify_one();
        if (line == EndOfInput) return;
    }
}

// The next line from the front end: waits for one; "stop" when the front end has gone
string read_line()
{
    unique_lock<mutex> hold(inbox->lock);
    inbox->arrived.wait(hold, [] { return !inbox->lines.empty(); });
    string line = inbox->lines.front();
    if (line == EndOfInput) return "stop";  // it stays in the queue: every later read is a stop too
    inbox->lines.pop_front();
    waiting.store(!inbox->lines.empty());
    return line;
}

void set_breakpoints(const string& args);

// While the program runs: the waiting lines that are for a running program, breakpoints and stop. Any other (a step,
// say, sent just before the program stops) stays in the queue for the stop to read.
void take_running_commands()
{
    for (;;)
    {
        string line;
        {
            lock_guard<mutex> hold(inbox->lock);
            if (inbox->lines.empty() ||
                (inbox->lines.front().rfind("breakpoints ", 0) != 0 && inbox->lines.front() != "stop"))
            {
                waiting.store(false);
                return;
            }
            line = inbox->lines.front();
            inbox->lines.pop_front();
        }
        if (line == "stop")
        {
            fflush(stdout);
            exit(0);
        }
        set_breakpoints(line.substr(12));
    }
}

// "breakpoints <file>\t<line>,<line>..."
void set_breakpoints(const string& args)
{
    size_t tab = args.find('\t');
    string file = path_key(args.substr(0, tab));
    set<int>& lines = breakpoints[file];
    lines.clear();
    if (tab == string::npos) return;
    const char* p = args.c_str() + tab + 1;
    while (*p)
    {
        char* end;
        long n = strtol(p, &end, 10);
        if (end == p) break;
        lines.insert((int)n);
        p = *end == ',' ? end + 1 : end;
    }
}

// Commands until one that runs the program: it says how (the mode); stop ends the program here
void wait_for_command(Place at, int depth)
{
    for (;;)
    {
        string cmd = read_line();
        if (cmd.rfind("breakpoints ", 0) == 0) { set_breakpoints(cmd.substr(12)); continue; }
        if (cmd == "stop") { fflush(stdout); exit(0); }
        if (cmd == "continue") mode = Mode::Run;
        else if (cmd == "stepIn") mode = Mode::StepIn;
        else if (cmd == "next") mode = Mode::Next;
        else if (cmd == "stepOut") mode = Mode::StepOut;
        else continue;  // unknown: ignored
        start = at;
        startDepth = depth;
        return;
    }
}

void stop(const char* reason, const string& text, const InstructionNode* pc, Place at, int depth)
{
    pauseAsked.store(false);  // a pause asked for now is answered by this stop
    send("{\"event\":\"stopped\",\"reason\":\"" + string(reason) + "\",\"text\":" + quote(text) +
         ",\"frames\":" + frames_json(pc) + "}");
    wait_for_command(at, depth);
}
}  // namespace

void debugger_start()
{
    inbox = new Inbox();
    thread(read_commands).detach();
    send("{\"event\":\"ready\"}");
    wait_for_command(Place{}, 0);
    atEntry = mode == Mode::StepIn;
}

// A statement starts when the instruction's line differs from the last one's, or a call or a return changed the
// depth. Stepping stops at the next statement (stepIn), at the next in this function or its caller (next), or in
// the caller (stepOut); a breakpoint stops the program when a statement starts on its line. A pause stops it at the
// next instruction with a place, statement or not: a loop on one line never starts another statement.
void debugger_at(InstructionNode* pc)
{
    if (waiting.load(memory_order_relaxed)) take_running_commands();
    Place p = place_of(pc);
    if (p.file < 0) return;  // an instruction lowering gave no place (a jump's label): not a statement
    int depth = (int)callStack.size();
    bool moved = p.file != last.file || p.line != last.line;
    bool statement = moved || depth != lastDepth;
    int returnDepth = lastDepth;  // a return lowers the depth: the caller's statement goes on, it does not start
    last = p;
    lastDepth = depth;
    if (pauseAsked.load(memory_order_relaxed))
    {
        atEntry = false;
        stop("pause", "", pc, p, depth);
        return;
    }
    if (!statement) return;

    const char* reason = nullptr;
    switch (mode)
    {
        case Mode::StepIn: reason = "step"; break;
        case Mode::Next:
            if (depth < startDepth || (depth == startDepth && (p.file != start.file || p.line != start.line)))
                reason = "step";
            break;
        case Mode::StepOut:
            if (depth < startDepth) reason = "step";
            break;
        case Mode::Run: break;
    }
    // A breakpoint stops the program where a statement starts on its line: not where a call returns to the
    // statement that made it
    if (!reason && moved && depth >= returnDepth && breakpoint_at(p)) reason = "breakpoint";
    if (!reason) return;
    if (atEntry) reason = "entry";
    atEntry = false;
    stop(reason, "", pc, p, depth);
}

int debugger_read_input()
{
    send("{\"event\":\"input\"}");
    string line = read_line();
    if (line == "stop")  // the front end has gone
    {
        fflush(stdout);
        exit(0);
    }
    return atoi(line.c_str());
}

void debugger_runtime_error(const string& text)
{
    const InstructionNode* pc = runtime_pc;
    send("{\"event\":\"stopped\",\"reason\":\"exception\",\"text\":" + quote(text) + ",\"frames\":" + frames_json(pc) +
         "}");
    for (;;)  // the program ends after this, whatever comes; breakpoints are for nothing now
    {
        string cmd = read_line();
        if (cmd.rfind("breakpoints ", 0) != 0) return;
    }
}
