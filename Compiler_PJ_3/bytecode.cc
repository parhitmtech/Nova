#include "bytecode.h"
#include "compiler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>

using namespace std;

// ── CRC32 (IEEE 802.3 polynomial) ────────────────────────────────────────────
static uint32_t s_crc_table[256];
static bool     s_crc_ready = false;

static void crc32_init() {
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

static uint32_t crc32_buf(const void* data, size_t len) {
    crc32_init();
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    while (len--) crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ── Write buffer ──────────────────────────────────────────────────────────────
struct BufWriter {
    vector<uint8_t> buf;

    void raw(const void* d, size_t n) {
        const uint8_t* p = (const uint8_t*)d;
        buf.insert(buf.end(), p, p + n);
    }
    void u8(uint8_t  v) { raw(&v, 1); }
    void u16(uint16_t v){ raw(&v, 2); }
    void u32(uint32_t v){ raw(&v, 4); }
    void i32(int32_t  v){ raw(&v, 4); }
    void f32(float    v){ raw(&v, 4); }
    void f64(double   v){ raw(&v, 8); }
    void str(const string& s) {
        uint32_t n = (uint32_t)s.size();
        u32(n); raw(s.data(), n);
    }
};

// ── Read buffer ───────────────────────────────────────────────────────────────
struct BufReader {
    const uint8_t* data;
    size_t pos, total;
    bool ok;

    BufReader(const uint8_t* d, size_t n) : data(d), pos(0), total(n), ok(true) {}

    void raw(void* out, size_t n) {
        if (pos + n > total) { ok = false; return; }
        memcpy(out, data + pos, n);
        pos += n;
    }
    uint8_t  u8()  { uint8_t  v=0; raw(&v,1); return v; }
    uint16_t u16() { uint16_t v=0; raw(&v,2); return v; }
    uint32_t u32() { uint32_t v=0; raw(&v,4); return v; }
    int32_t  i32() { int32_t  v=0; raw(&v,4); return v; }
    float    f32() { float    v=0; raw(&v,4); return v; }
    double   f64() { double   v=0; raw(&v,8); return v; }
    string   str() {
        uint32_t n = u32();
        if (!ok || pos + n > total) { ok = false; return ""; }
        string s((const char*)(data + pos), n);
        pos += n;
        return s;
    }
};

// ── Global index maps (emit side) ─────────────────────────────────────────────
static unordered_map<const InstructionNode*, uint32_t> g_to_idx;
static vector<const InstructionNode*>                  g_to_node;

static void index_block(const InstructionNode* head) {
    for (const InstructionNode* p = head; p; p = p->next) {
        if (!g_to_idx.count(p)) {
            g_to_idx[p] = (uint32_t)g_to_node.size();
            g_to_node.push_back(p);
        }
    }
}

static uint32_t idx(const InstructionNode* p) { return g_to_idx.at(p); }

// ── Instruction serializer ────────────────────────────────────────────────────
static void emit_instr(BufWriter& w, const InstructionNode* n) {
    w.u16((uint16_t)n->type);
    w.u32((uint32_t)n->line_no);

    switch (n->type) {
    case NOOP: break;

    case IN:
        w.i32(n->input_inst.var_index);
        break;

    case OUT:
        w.i32(n->output_inst.var_index);
        w.u8(n->output_inst.is_string    ? 1 : 0);
        w.u8(n->output_inst.is_string_var ? 1 : 0);
        w.u8(n->output_inst.newline      ? 1 : 0);
        w.u32((uint32_t)n->output_inst.value_type);
        break;

    case ASSIGN:
        w.i32(n->assign_inst.left_hand_side_index);
        w.i32(n->assign_inst.operand1_index);
        w.i32(n->assign_inst.operand2_index);
        w.u32((uint32_t)n->assign_inst.op);
        break;

    case CJMP:
        w.u32((uint32_t)n->cjmp_inst.condition_op);
        w.i32(n->cjmp_inst.operand1_index);
        w.i32(n->cjmp_inst.operand2_index);
        w.u32(idx(n->cjmp_inst.target));
        break;

    case JMP:
        w.u32(idx(n->jmp_inst.target));
        break;

    case CALL: {
        w.u32(idx(n->call_inst.function_head));
        w.i32(n->call_inst.ret_val_index);
        w.i32(n->call_inst.func_slot_base);
        w.i32(n->call_inst.func_slot_count);
        w.i32(n->call_inst.num_params);
        w.i32(n->call_inst.num_ret_slots);
        for (int i = 0; i < n->call_inst.num_params; i++) {
            w.u32((uint32_t)n->call_inst.param_types[i]);
            w.u32((uint32_t)n->call_inst.arg_types[i]);
            w.i32(n->call_inst.param_slots[i]);
            w.i32(n->call_inst.arg_val_slots[i]);
        }
        for (int i = 0; i < n->call_inst.num_ret_slots; i++)
            w.i32(n->call_inst.all_ret_slots[i]);
        break;
    }

    case RET:
        w.i32(n->ret_inst.ret_val_index);
        break;

    case ARRAY_READ:
    case ARRAY_WRITE:
        w.i32(n->array_inst.base_index);
        w.u8(n->array_inst.dynamic_base ? 1 : 0);
        w.i32(n->array_inst.index_slot);
        w.i32(n->array_inst.target_index);
        w.i32(n->array_inst.array_size);
        w.i32(n->array_inst.size_slot);
        w.i32(n->array_inst.line_no);
        break;

    case ALLOC:
        w.i32(n->alloc_inst.base_slot);
        w.i32(n->alloc_inst.size_slot);
        break;

    case STRCAT:
        w.i32(n->strcat_inst.dest_slot);
        w.i32(n->strcat_inst.left_slot);
        w.i32(n->strcat_inst.right_slot);
        break;

    case SCMP:
        w.u32((uint32_t)n->scmp_inst.condition_op);
        w.i32(n->scmp_inst.operand1_index);
        w.i32(n->scmp_inst.operand2_index);
        w.u32(idx(n->scmp_inst.target));
        break;

    case ASSIGN_F:
        w.i32(n->assign_f_inst.left_hand_side_index);
        w.i32(n->assign_f_inst.operand1_index);
        w.i32(n->assign_f_inst.operand2_index);
        w.u32((uint32_t)n->assign_f_inst.op);
        break;

    case ASSIGN_D:
        w.i32(n->assign_d_inst.left_hand_side_index);
        w.i32(n->assign_d_inst.operand1_index);
        w.i32(n->assign_d_inst.operand2_index);
        w.u32((uint32_t)n->assign_d_inst.op);
        break;

    case CAST:
        w.i32(n->cast_inst.src_index);
        w.i32(n->cast_inst.dst_index);
        w.u32((uint32_t)n->cast_inst.src_type);
        w.u32((uint32_t)n->cast_inst.dst_type);
        break;

    case TENSOR_CALL:
        w.u32((uint32_t)n->tensor_call_inst.op);
        w.i32(n->tensor_call_inst.result_slot);
        w.i32(n->tensor_call_inst.num_args);
        for (int i = 0; i < 8; i++) w.i32(n->tensor_call_inst.arg_slots[i]);
        for (int i = 0; i < 8; i++) w.u32((uint32_t)n->tensor_call_inst.arg_types[i]);
        break;

    case SPILL_LOAD:
    case SPILL_STORE:
        w.i32(n->spill_inst.spill_slot);
        w.i32(n->spill_inst.temp_slot);
        break;

    case HF_INFER:
        w.u32((uint32_t)n->hf_inst.op);
        w.i32(n->hf_inst.model_slot);
        w.i32(n->hf_inst.input_slot);
        w.i32(n->hf_inst.result_slot);
        w.i32(n->hf_inst.max_tokens);
        break;

    case SOL_CALL:
        w.u32((uint32_t)n->sol_inst.op);
        w.i32(n->sol_inst.job_slot);
        w.i32(n->sol_inst.model_slot);
        w.i32(n->sol_inst.task_slot);
        w.i32(n->sol_inst.dataset_slot);
        w.i32(n->sol_inst.epochs);
        w.i32(n->sol_inst.batch_size);
        w.f32(n->sol_inst.lr);
        w.i32(n->sol_inst.input_slot);
        w.i32(n->sol_inst.result_slot);
        w.i32(n->sol_inst.path_slot);
        break;

    default:
        fprintf(stderr, "NBC emit: unknown instruction type %d\n", (int)n->type);
        break;
    }
}

// ── emit_bytecode ─────────────────────────────────────────────────────────────
void emit_bytecode(struct InstructionNode* program, const std::string& outPath)
{
    g_to_idx.clear();
    g_to_node.clear();

    // Index all blocks: main first, then each function (map iteration is sorted)
    index_block(program);
    for (auto& kv : functionTable)
        index_block(kv.second);

    BufWriter w;

    // ── Header ────────────────────────────────────────────────────────────────
    w.raw(NBC_MAGIC, 4);
    w.u16(NBC_VERSION);
    w.u32(0);  // flags (reserved)

    // ── strMem snapshot ───────────────────────────────────────────────────────
    w.u32((uint32_t)strMem.size());
    for (auto& s : strMem) w.str(s);

    // ── int mem snapshot ──────────────────────────────────────────────────────
    w.u32((uint32_t)next_available);
    for (int i = 0; i < next_available; i++)
        w.i32((i < (int)mem.size()) ? mem[i] : 0);

    // ── float mem snapshot ────────────────────────────────────────────────────
    w.u32((uint32_t)next_float_available);
    for (int i = 0; i < next_float_available; i++)
        w.f32((i < (int)fmem.size()) ? fmem[i] : 0.0f);

    // ── double mem snapshot ───────────────────────────────────────────────────
    w.u32((uint32_t)next_double_available);
    for (int i = 0; i < next_double_available; i++)
        w.f64((i < (int)dmem.size()) ? dmem[i] : 0.0);

    // ── Block headers: num_blocks + [name + instr_count] per block ────────────
    // Block 0 = main (empty name), blocks 1..N = functions (sorted by name)
    uint32_t num_blocks = 1 + (uint32_t)functionTable.size();
    w.u32(num_blocks);

    // main block
    uint32_t main_count = 0;
    for (const InstructionNode* p = program; p; p = p->next) main_count++;
    w.str("");            // empty name = main
    w.u32(main_count);

    // function blocks
    for (auto& kv : functionTable) {
        uint32_t cnt = 0;
        for (const InstructionNode* p = kv.second; p; p = p->next) cnt++;
        w.str(kv.first);
        w.u32(cnt);
    }

    // ── Flat instruction array (global order) ─────────────────────────────────
    for (auto* n : g_to_node)
        emit_instr(w, n);

    // ── CRC32 ─────────────────────────────────────────────────────────────────
    uint32_t crc = crc32_buf(w.buf.data(), w.buf.size());
    w.u32(crc);

    // ── Write file ────────────────────────────────────────────────────────────
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "NBC: cannot write %s\n", outPath.c_str());
        return;
    }
    fwrite(w.buf.data(), 1, w.buf.size(), f);
    fclose(f);

    fprintf(stderr, "NBC: wrote %s (%zu bytes, %u instrs, %u blocks)\n",
            outPath.c_str(), w.buf.size(),
            (uint32_t)g_to_node.size(), num_blocks);
}

// ── Instruction deserializer ──────────────────────────────────────────────────
static void load_instr(BufReader& r, InstructionNode* n,
                       const vector<InstructionNode*>& nodes)
{
    n->type   = (InstructionType)r.u16();
    n->line_no = (int)r.u32();

    switch (n->type) {
    case NOOP: break;

    case IN:
        n->input_inst.var_index = r.i32();
        break;

    case OUT:
        n->output_inst.var_index     = r.i32();
        n->output_inst.is_string     = r.u8() != 0;
        n->output_inst.is_string_var = r.u8() != 0;
        n->output_inst.newline       = r.u8() != 0;
        n->output_inst.value_type    = (VarType)r.u32();
        break;

    case ASSIGN:
        n->assign_inst.left_hand_side_index = r.i32();
        n->assign_inst.operand1_index       = r.i32();
        n->assign_inst.operand2_index       = r.i32();
        n->assign_inst.op = (ArithmeticOperatorType)r.u32();
        break;

    case CJMP:
        n->cjmp_inst.condition_op   = (ConditionalOperatorType)r.u32();
        n->cjmp_inst.operand1_index = r.i32();
        n->cjmp_inst.operand2_index = r.i32();
        n->cjmp_inst.target         = nodes[r.u32()];
        break;

    case JMP:
        n->jmp_inst.target = nodes[r.u32()];
        break;

    case CALL: {
        n->call_inst.function_head  = nodes[r.u32()];
        n->call_inst.ret_val_index  = r.i32();
        n->call_inst.func_slot_base = r.i32();
        n->call_inst.func_slot_count= r.i32();
        int np = n->call_inst.num_params    = r.i32();
        int nr = n->call_inst.num_ret_slots = r.i32();
        n->call_inst.param_types  = np > 0 ? new VarType[np] : nullptr;
        n->call_inst.arg_types    = np > 0 ? new VarType[np] : nullptr;
        n->call_inst.param_slots  = np > 0 ? new int[np]     : nullptr;
        n->call_inst.arg_val_slots= np > 0 ? new int[np]     : nullptr;
        for (int i = 0; i < np; i++) {
            n->call_inst.param_types[i]   = (VarType)r.u32();
            n->call_inst.arg_types[i]     = (VarType)r.u32();
            n->call_inst.param_slots[i]   = r.i32();
            n->call_inst.arg_val_slots[i] = r.i32();
        }
        n->call_inst.all_ret_slots = nr > 0 ? new int[nr] : nullptr;
        for (int i = 0; i < nr; i++)
            n->call_inst.all_ret_slots[i] = r.i32();
        break;
    }

    case RET:
        n->ret_inst.ret_val_index = r.i32();
        break;

    case ARRAY_READ:
    case ARRAY_WRITE:
        n->array_inst.base_index   = r.i32();
        n->array_inst.dynamic_base = r.u8() != 0;
        n->array_inst.index_slot   = r.i32();
        n->array_inst.target_index = r.i32();
        n->array_inst.array_size   = r.i32();
        n->array_inst.size_slot    = r.i32();
        n->array_inst.line_no      = r.i32();
        break;

    case ALLOC:
        n->alloc_inst.base_slot = r.i32();
        n->alloc_inst.size_slot = r.i32();
        break;

    case STRCAT:
        n->strcat_inst.dest_slot  = r.i32();
        n->strcat_inst.left_slot  = r.i32();
        n->strcat_inst.right_slot = r.i32();
        break;

    case SCMP:
        n->scmp_inst.condition_op   = (ConditionalOperatorType)r.u32();
        n->scmp_inst.operand1_index = r.i32();
        n->scmp_inst.operand2_index = r.i32();
        n->scmp_inst.target         = nodes[r.u32()];
        break;

    case ASSIGN_F:
        n->assign_f_inst.left_hand_side_index = r.i32();
        n->assign_f_inst.operand1_index       = r.i32();
        n->assign_f_inst.operand2_index       = r.i32();
        n->assign_f_inst.op = (ArithmeticOperatorType)r.u32();
        break;

    case ASSIGN_D:
        n->assign_d_inst.left_hand_side_index = r.i32();
        n->assign_d_inst.operand1_index       = r.i32();
        n->assign_d_inst.operand2_index       = r.i32();
        n->assign_d_inst.op = (ArithmeticOperatorType)r.u32();
        break;

    case CAST:
        n->cast_inst.src_index = r.i32();
        n->cast_inst.dst_index = r.i32();
        n->cast_inst.src_type  = (VarType)r.u32();
        n->cast_inst.dst_type  = (VarType)r.u32();
        break;

    case TENSOR_CALL:
        n->tensor_call_inst.op          = (TensorOp)r.u32();
        n->tensor_call_inst.result_slot = r.i32();
        n->tensor_call_inst.num_args    = r.i32();
        for (int i = 0; i < 8; i++) n->tensor_call_inst.arg_slots[i] = r.i32();
        for (int i = 0; i < 8; i++) n->tensor_call_inst.arg_types[i] = (VarType)r.u32();
        break;

    case SPILL_LOAD:
    case SPILL_STORE:
        n->spill_inst.spill_slot = r.i32();
        n->spill_inst.temp_slot  = r.i32();
        break;

    case HF_INFER:
        n->hf_inst.op          = (HFOp)r.u32();
        n->hf_inst.model_slot  = r.i32();
        n->hf_inst.input_slot  = r.i32();
        n->hf_inst.result_slot = r.i32();
        n->hf_inst.max_tokens  = r.i32();
        break;

    case SOL_CALL:
        n->sol_inst.op           = (SolOp)r.u32();
        n->sol_inst.job_slot     = r.i32();
        n->sol_inst.model_slot   = r.i32();
        n->sol_inst.task_slot    = r.i32();
        n->sol_inst.dataset_slot = r.i32();
        n->sol_inst.epochs       = r.i32();
        n->sol_inst.batch_size   = r.i32();
        n->sol_inst.lr           = r.f32();
        n->sol_inst.input_slot   = r.i32();
        n->sol_inst.result_slot  = r.i32();
        n->sol_inst.path_slot    = r.i32();
        break;

    default:
        fprintf(stderr, "NBC load: unknown instruction type %d\n", (int)n->type);
        break;
    }
}

// ── load_bytecode ─────────────────────────────────────────────────────────────
struct InstructionNode* load_bytecode(const std::string& path)
{
    // Read entire file
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "NBC: cannot open %s\n", path.c_str()); return nullptr; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 10) { fclose(f); fprintf(stderr, "NBC: file too small\n"); return nullptr; }
    vector<uint8_t> raw((size_t)fsize);
    fread(raw.data(), 1, (size_t)fsize, f);
    fclose(f);

    // Verify CRC (last 4 bytes cover everything before them)
    uint32_t stored_crc;
    memcpy(&stored_crc, raw.data() + fsize - 4, 4);
    uint32_t computed_crc = crc32_buf(raw.data(), (size_t)fsize - 4);
    if (stored_crc != computed_crc) {
        fprintf(stderr, "NBC: CRC mismatch (stored=%08X computed=%08X)\n",
                stored_crc, computed_crc);
        return nullptr;
    }

    // Parse (exclude CRC bytes from reader)
    BufReader r(raw.data(), (size_t)fsize - 4);

    // ── Header ────────────────────────────────────────────────────────────────
    char magic[4]; r.raw(magic, 4);
    if (memcmp(magic, NBC_MAGIC, 4) != 0) {
        fprintf(stderr, "NBC: invalid magic\n"); return nullptr;
    }
    uint16_t ver = r.u16();
    if (ver != NBC_VERSION) {
        fprintf(stderr, "NBC: version %u (expected %u)\n", ver, NBC_VERSION);
        return nullptr;
    }
    r.u32();  // flags (reserved)

    // ── Restore strMem ────────────────────────────────────────────────────────
    strMem.clear();
    uint32_t sc = r.u32();
    strMem.reserve(sc);
    for (uint32_t i = 0; i < sc; i++) strMem.push_back(r.str());
    next_str_available = (int)sc;

    // ── Restore int mem ───────────────────────────────────────────────────────
    uint32_t mc = r.u32();
    mem.assign(mc, 0);
    for (uint32_t i = 0; i < mc; i++) mem[i] = r.i32();
    next_available = (int)mc;

    // ── Restore float mem ─────────────────────────────────────────────────────
    uint32_t fc = r.u32();
    fmem.assign(fc, 0.0f);
    for (uint32_t i = 0; i < fc; i++) fmem[i] = r.f32();
    next_float_available = (int)fc;

    // ── Restore double mem ────────────────────────────────────────────────────
    uint32_t dc = r.u32();
    dmem.assign(dc, 0.0);
    for (uint32_t i = 0; i < dc; i++) dmem[i] = r.f64();
    next_double_available = (int)dc;

    // ── Block headers ─────────────────────────────────────────────────────────
    uint32_t num_blocks = r.u32();
    vector<string>   block_names(num_blocks);
    vector<uint32_t> block_counts(num_blocks);
    uint32_t total_instrs = 0;
    for (uint32_t b = 0; b < num_blocks; b++) {
        block_names[b]  = r.str();
        block_counts[b] = r.u32();
        total_instrs   += block_counts[b];
    }

    if (total_instrs == 0) {
        fprintf(stderr, "NBC: no instructions\n");
        return nullptr;
    }

    // ── Allocate all nodes; link within blocks; null-terminate each block ─────
    vector<InstructionNode*> nodes(total_instrs);
    for (uint32_t i = 0; i < total_instrs; i++) {
        nodes[i] = new InstructionNode();
        memset(nodes[i], 0, sizeof(InstructionNode));
    }

    {
        uint32_t g = 0;
        for (uint32_t b = 0; b < num_blocks; b++) {
            uint32_t cnt = block_counts[b];
            for (uint32_t i = 0; i < cnt; i++) {
                nodes[g + i]->next = (i + 1 < cnt) ? nodes[g + i + 1] : nullptr;
            }
            g += cnt;
        }
    }

    // ── Deserialize instructions (jump targets resolved via nodes[]) ───────────
    for (uint32_t i = 0; i < total_instrs; i++) {
        if (!r.ok) break;
        // Preserve next pointer (set above) across load_instr
        InstructionNode* nxt = nodes[i]->next;
        load_instr(r, nodes[i], nodes);
        nodes[i]->next = nxt;
    }

    if (!r.ok) {
        fprintf(stderr, "NBC: truncated or malformed file\n");
        return nullptr;
    }

    // ── Restore functionTable ─────────────────────────────────────────────────
    functionTable.clear();
    {
        uint32_t g = 0;
        for (uint32_t b = 0; b < num_blocks; b++) {
            if (!block_names[b].empty())
                functionTable[block_names[b]] = nodes[g];
            g += block_counts[b];
        }
    }

    fprintf(stderr, "NBC: loaded %s (%u instrs, %u blocks)\n",
            path.c_str(), total_instrs, num_blocks);

    return nodes[0];  // main block head always at global index 0
}
