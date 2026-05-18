// Standalone tool: mark loads that feed branches through ALU chains.
// Compile: g++ -std=c++17 branch_load_dep.cc -o branch_load_dep
// Usage:   ./branch_load_dep input.bz2 output.bz2

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Minimal redefinition of ctype_pin_inst — must stay in sync with
// ctype_pin_inst.h. The static_assert below catches drift.
// ---------------------------------------------------------------------------

#define MAX_SRC_REGS_NUM      8
#define MAX_DST_REGS_NUM      8
#define MAX_MEM_ADDR_REGS_NUM 2
#define MAX_LD_NUM            8
#define MAX_ST_NUM            8

typedef uint8_t compressed_reg_t;

typedef enum {
    WPNM_NOT_IN_WPNM = 0,
    WPNM_REASON_REDIRECT_TO_NOT_INSTRUMENTED,
    WPNM_REASON_RETURN_TO_NOT_INSTRUMENTED,
    WPNM_REASON_NONRET_CF_TO_NOT_INSTRUMENTED,
    WPNM_REASON_NOT_TAKEN_TO_NOT_INSTRUMENTED,
    WPNM_REASON_WRONG_PATH_STORE_TO_NEW_REGION,
    WPNM_NUM_REASONS
} Wrongpath_Nop_Mode_Reason;

typedef struct {
    uint64_t inst_uid;
    uint64_t instruction_addr;
    uint8_t  size;
    uint64_t inst_binary_msb;
    uint64_t inst_binary_lsb;
    uint8_t  op_type;
    uint8_t  cf_type;
    uint8_t  is_fp;
    uint16_t true_op_type;
    uint8_t  num_src_regs;
    uint8_t  num_dst_regs;
    uint8_t  num_ld1_addr_regs;
    uint8_t  num_ld2_addr_regs;
    uint8_t  num_st_addr_regs;
    compressed_reg_t src_regs[MAX_SRC_REGS_NUM];
    compressed_reg_t dst_regs[MAX_DST_REGS_NUM];
    compressed_reg_t ld1_addr_regs[MAX_MEM_ADDR_REGS_NUM];
    compressed_reg_t ld2_addr_regs[MAX_MEM_ADDR_REGS_NUM];
    compressed_reg_t st_addr_regs[MAX_MEM_ADDR_REGS_NUM];
    uint8_t  num_simd_lanes;
    uint8_t  lane_width_bytes;
    uint8_t  num_ld;
    uint8_t  num_st;
    uint8_t  has_immediate;
    uint64_t ld_vaddr[MAX_LD_NUM];
    uint64_t st_vaddr[MAX_ST_NUM];
    uint8_t  ld_size;
    uint8_t  st_size;
    uint64_t branch_target;
    uint8_t  actually_taken    : 1;
    uint8_t  is_string         : 1;
    uint8_t  is_call           : 1;
    uint8_t  is_move           : 1;
    uint8_t  is_prefetch       : 1;
    uint8_t  has_push          : 1;
    uint8_t  has_pop           : 1;
    uint8_t  is_ifetch_barrier : 1;
    uint8_t  is_lock           : 1;
    uint8_t  is_repeat         : 1;
    uint8_t  is_simd           : 1;
    uint8_t  is_gather_scatter : 1;
    uint8_t  is_sentinel       : 1;
    uint8_t  fake_inst         : 1;
    uint8_t  exit              : 1;
    uint8_t  encoding_is_new   : 1;
    Wrongpath_Nop_Mode_Reason fake_inst_reason;
    uint64_t instruction_next_addr;
    char     pin_iclass[16];
    uint8_t  last_inst_from_trace    : 1;
    uint8_t  fetched_instruction     : 1;
    uint8_t  scarab_marker_roi_begin : 1;
    uint8_t  scarab_marker_roi_end   : 1;
    uint8_t  feeds_branch            : 1;
} __attribute__((packed)) ctype_pin_inst;

static_assert(sizeof(ctype_pin_inst) == 239, "ctype_pin_inst layout mismatch — re-check struct definition");

// ---------------------------------------------------------------------------
// Analysis state
// ---------------------------------------------------------------------------

struct InstRecord {
    uint64_t inst_uid;
    bool     is_load;
    uint8_t  num_src_regs;
    uint64_t src_writer_uids[MAX_SRC_REGS_NUM];
};

static std::unordered_map<uint8_t,  uint64_t>   reg_map;
static std::unordered_map<uint64_t, InstRecord> inst_records;
static std::unordered_set<uint64_t>             marked_loads;

static void find_loads(uint64_t uid, std::unordered_set<uint64_t>& visited) {
    if (uid == 0 || visited.count(uid))
        return;
    visited.insert(uid);

    auto it = inst_records.find(uid);
    if (it == inst_records.end())
        return;

    const InstRecord& rec = it->second;
    if (rec.is_load) {
        marked_loads.insert(uid);
        return;
    }

    for (int i = 0; i < rec.num_src_regs; i++)
        find_loads(rec.src_writer_uids[i], visited);
}

// ---------------------------------------------------------------------------
// Trace I/O
// ---------------------------------------------------------------------------

static FILE* open_read(const char* path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "bzip2 -dc %s", path);
    FILE* f = popen(cmd, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    return f;
}

static FILE* open_write(const char* path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "bzip2 > %s", path);
    FILE* f = popen(cmd, "w");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    return f;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.bz2 output.bz2\n", argv[0]);
        return 1;
    }

    // --- pass 1: collect marked loads ---
    FILE* in = open_read(argv[1]);
    ctype_pin_inst pi;
    while (fread(&pi, sizeof(pi), 1, in) == 1) {
        InstRecord rec;
        rec.inst_uid     = pi.inst_uid;
        rec.is_load      = (pi.num_ld > 0);
        rec.num_src_regs = pi.num_src_regs;
        for (int i = 0; i < pi.num_src_regs; i++) {
            auto it = reg_map.find(pi.src_regs[i]);
            rec.src_writer_uids[i] = (it != reg_map.end()) ? it->second : 0;
        }
        inst_records[pi.inst_uid] = rec;

        if (pi.cf_type != 0 /* NOT_CF */) {
            std::unordered_set<uint64_t> visited;
            for (int i = 0; i < rec.num_src_regs; i++)
                find_loads(rec.src_writer_uids[i], visited);
        }

        for (int i = 0; i < pi.num_dst_regs; i++)
            reg_map[pi.dst_regs[i]] = pi.inst_uid;
    }
    pclose(in);

    fprintf(stderr, "marked %zu loads\n", marked_loads.size());

    // --- pass 2: rewrite trace with feeds_branch set ---
    in = open_read(argv[1]);
    FILE* out = open_write(argv[2]);
    while (fread(&pi, sizeof(pi), 1, in) == 1) {
        pi.feeds_branch = marked_loads.count(pi.inst_uid) ? 1 : 0;
        fwrite(&pi, sizeof(pi), 1, out);
    }
    pclose(in);
    pclose(out);

    return 0;
}
