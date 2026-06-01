/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : branch_load_dep.cc
 * Description  : Identifies loads whose values directly feed branch instructions.
 *
 * Algorithm (inline mode):
 *   Called after map_op(), so src_info[] is fully populated.  For each branch,
 *   iterate its REG_DATA_DEP sources; any source that is a load is marked
 *   feeds_branch.
 *
 * Replay mode:
 *   Loads a pre-recorded CSV of (proc_id, feeder_inst_uid, depth) pairs at
 *   init time and marks ops by inst_uid lookup in bld_process_op().
 ***************************************************************************************/

#include "branch_load_dep.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "core.param.h"
#include "dcache_stage.h"
#include "memory/memory.param.h"
#include "inst_info.h"
#include "libs/cache_lib.h"
#include "memory/memory.h"
#include "model.h"
#include "op.h"
#include "op_info.h"
#include "prefetcher/pref_bld.h"
#include "statistics.h"
#include "table_info.h"
}

/* CSV trace: one row per branch→feeder-load pair discovered. */
static FILE* g_trace_out = nullptr;

/* Build the per-benchmark trace path from CBP_TRACE_R0. */
static void bld_trace_path(char* out, size_t sz) {
  const char* dir   = BRANCH_LOAD_DEP_TRACE_DIR;
  const char* trace = CBP_TRACE_R0;

  if (!trace || !*trace) {
    snprintf(out, sz, "%s/bld_trace.csv", dir);
    return;
  }

  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, trace, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  char* slash = strrchr(tmp, '/');
  const char* filename = slash ? slash + 1 : tmp;
  char simpt[64];
  const char* dot = strchr(filename, '.');
  if (dot) {
    size_t n = (size_t)(dot - filename) < sizeof(simpt) - 1
                   ? (size_t)(dot - filename)
                   : sizeof(simpt) - 1;
    strncpy(simpt, filename, n);
    simpt[n] = '\0';
  } else {
    strncpy(simpt, filename, sizeof(simpt) - 1);
    simpt[sizeof(simpt) - 1] = '\0';
  }

  if (slash)
    *slash = '\0';

  for (int lvl = 0; lvl < 2; lvl++) {
    char* p = strrchr(tmp, '/');
    if (p)
      *p = '\0';
  }

  const char* bench_slash = strrchr(tmp, '/');
  const char* bench       = bench_slash ? bench_slash + 1 : tmp;

  snprintf(out, sz, "%s/%s_%s.csv", dir, bench, simpt);
}

/* feeder inst_uid → minimum recorded depth, per proc. */
static std::vector<std::unordered_map<uns64, uns>> g_replay_maps;

void bld_init(uns8 num_cores) {
  g_replay_maps.resize(num_cores);

  char trace_path[MAX_STR_LENGTH + 1];
  bld_trace_path(trace_path, sizeof(trace_path));

  if (BRANCH_LOAD_DEP_TRACE_RECORD) {
    g_trace_out = fopen(trace_path, "w");
    if (!g_trace_out) {
      fprintf(stderr, "[BLD] Cannot open trace file for writing: %s\n",
              trace_path);
      exit(1);
    }
    fprintf(g_trace_out, "proc_id,feeder_inst_uid,depth\n");
  }

  if (BRANCH_LOAD_DEP_TRACE_REPLAY) {
    std::ifstream f(trace_path);
    if (!f.is_open()) {
      fprintf(stderr, "[BLD] Cannot open trace file for reading: %s\n",
              trace_path);
      exit(1);
    }
    std::string row;
    std::getline(f, row); /* skip header */
    while (std::getline(f, row)) {
      std::istringstream ss(row);
      std::string tok;
      int pid;
      std::getline(ss, tok, ','); pid = std::stoi(tok);
      std::getline(ss, tok, ','); uns64 iuid = (uns64)std::stoull(tok);
      std::getline(ss, tok, ','); uns   d    = (uns)std::stoul(tok);
      if (pid >= 0 && (uns8)pid < num_cores) {
        auto& m = g_replay_maps[(uns8)pid];
        auto it = m.find(iuid);
        if (it == m.end() || d < it->second)
          m[iuid] = d;
      }
    }
  }
}

void bld_process_op(uns8 proc_id, Op* op) {
  /* Replay: mark feeder loads by inst_uid so dcache gives them hit latency.
   * Runs independently of BRANCH_LOAD_DEP.  Off-path ops are skipped. */
  if (BRANCH_LOAD_DEP_TRACE_REPLAY && !op->off_path) {
    const auto& m = g_replay_maps[proc_id];
    auto it = m.find(op->inst_uid);
    if (it != m.end()) {
      uns replay_max_depth = BRANCH_LOAD_DEP_MAX_DEPTH;
      if (replay_max_depth == 0 || it->second <= replay_max_depth)
        op->feeds_branch = TRUE;
    }
  }

  if (!BRANCH_LOAD_DEP)
    return;

  if (op->off_path)
    return;

  if (op->inst_info->table_info.cf_type == NOT_CF)
    return;

  /* For each direct REG_DATA_DEP source of this branch, mark it if it is a load.
   * src_info is populated by map_op(), so bld_process_op must be called after map. */
  for (uns i = 0; i < op->num_srcs; i++) {
    Src_Info* s = &op->src_info[i];
    if (s->type != REG_DATA_DEP)
      continue;
    Op* src = s->op;
    if (!src || !src->op_pool_valid || src->unique_num != s->unique_num)
      continue;
    if (src->inst_info->table_info.mem_type != MEM_LD)
      continue;

    src->feeds_branch = TRUE;
    Cache* dcache = get_dcache_for_proc(proc_id);
    if (src->oracle_info.va != 0 && cache_set_feeds_branch(dcache, proc_id, src->oracle_info.va))
      STAT_EVENT(proc_id, DCACHE_FEEDER_LINES_MARKED);

    if (BRANCH_LOAD_DEP_TRACE_RECORD && g_trace_out)
      fprintf(g_trace_out, "%u,%" PRIu64 ",%u\n",
              (unsigned)proc_id, (uint64_t)src->inst_uid, 0u);

    if (BRANCH_LOAD_DEP_PREFETCH && src->oracle_info.va != 0 && model->mem == MODEL_MEM) {
      Addr line_addr;
      if (!cache_access(dcache, src->oracle_info.va, &line_addr, FALSE))
        new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL,
                    dcache_fill_line, 0, NULL);
    }
  }
}

void bld_finish(void) {
  if (g_trace_out) {
    fflush(g_trace_out);
    fclose(g_trace_out);
    g_trace_out = nullptr;
  }
}
