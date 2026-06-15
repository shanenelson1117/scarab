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
 * Description  : Identifies loads whose values feed branch instructions.
 *
 * Algorithm (inline mode):
 *   Called after map_op(), so src_info[] is fully populated.  For each branch,
 *   BFS backward through REG_DATA_DEP edges and mark feeder loads.
 *
 * Replay mode:
 *   Loads a CSV of (proc_id, branch_inst_uid, feeder_inst_uid, depth) tuples.
 *   At load map time, mark feeds_branch if ANY pair for that feeder has
 *   depth <= BRANCH_LOAD_DEP_MAX_DEPTH (0 = unlimited).
 ***************************************************************************************/

#include "branch_load_dep.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <deque>
#include <errno.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
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

struct BldReplayPair {
  uns64 branch_inst_uid;
  uns64 feeder_inst_uid;
  uns   depth;
};

/* CSV trace: one row per branch→feeder-load pair discovered. */
static FILE* g_trace_out = nullptr;
static uns64 g_trace_rows_written = 0;
static char  g_trace_path[MAX_STR_LENGTH + 1];

/* All recorded pairs, per proc (retained for debugging / future branch-aware replay). */
static std::vector<std::vector<BldReplayPair>> g_replay_pairs;

/* feeder_inst_uid → depths from every recorded pair (no min-depth collapse). */
static std::vector<std::unordered_map<uns64, std::vector<uns>>> g_replay_feeder_depths;

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

/* Create parent directories for path (mkdir -p semantics). */
static Flag bld_mkdir_p(const char* path) {
  char buf[MAX_STR_LENGTH + 1];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  for (char* p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
      return FALSE;
    *p = '/';
  }
  if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    return FALSE;
  return TRUE;
}

/* Open the record CSV on first use (after params and CBP_TRACE_R0 are final). */
static Flag bld_trace_open_for_record(void) {
  if (g_trace_out)
    return TRUE;
  if (!BRANCH_LOAD_DEP_TRACE_RECORD)
    return FALSE;

  bld_trace_path(g_trace_path, sizeof(g_trace_path));

  char dir[MAX_STR_LENGTH + 1];
  strncpy(dir, g_trace_path, sizeof(dir) - 1);
  dir[sizeof(dir) - 1] = '\0';
  char* slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    if (dir[0] && !bld_mkdir_p(dir)) {
      fprintf(stderr, "[BLD] Cannot create trace directory %s: %s\n", dir, strerror(errno));
      exit(1);
    }
  }

  g_trace_out = fopen(g_trace_path, "w");
  if (!g_trace_out) {
    fprintf(stderr, "[BLD] Cannot open trace file for writing: %s (%s)\n",
            g_trace_path, strerror(errno));
    exit(1);
  }
  fprintf(g_trace_out, "proc_id,branch_inst_uid,feeder_inst_uid,depth\n");
  fprintf(stderr, "[BLD] Recording branch→feeder trace to %s\n", g_trace_path);
  return TRUE;
}

static void bld_trace_record_pair(uns8 proc_id, uns64 branch_inst_uid, uns64 feeder_inst_uid, uns depth) {
  if (!BRANCH_LOAD_DEP_TRACE_RECORD)
    return;
  if (!bld_trace_open_for_record())
    return;
  fprintf(g_trace_out, "%u,%" PRIu64 ",%" PRIu64 ",%u\n",
          (unsigned)proc_id, (uint64_t)branch_inst_uid,
          (uint64_t)feeder_inst_uid, (unsigned)depth);
  g_trace_rows_written++;
}

static void bld_replay_add_pair(uns8 proc_id, uns64 branch_inst_uid, uns64 feeder_inst_uid, uns depth) {
  g_replay_pairs[proc_id].push_back({branch_inst_uid, feeder_inst_uid, depth});
  g_replay_feeder_depths[proc_id][feeder_inst_uid].push_back(depth);
}

/* Return TRUE if this feeder load should receive replay hit-latency treatment. */
static Flag bld_replay_feeder_included(uns8 proc_id, uns64 feeder_inst_uid, uns max_depth) {
  const auto& depths_by_feeder = g_replay_feeder_depths[proc_id];
  auto it = depths_by_feeder.find(feeder_inst_uid);
  if (it == depths_by_feeder.end())
    return FALSE;

  for (uns depth : it->second) {
    if (max_depth == 0 || depth <= max_depth)
      return TRUE;
  }
  return FALSE;
}

static void bld_load_replay_csv(const char* trace_path, uns8 num_cores) {
  std::ifstream f(trace_path);
  if (!f.is_open()) {
    fprintf(stderr, "[BLD] Cannot open trace file for reading: %s\n", trace_path);
    exit(1);
  }

  std::string header;
  if (!std::getline(f, header)) {
    fprintf(stderr, "[BLD] Empty trace file: %s\n", trace_path);
    exit(1);
  }

  const Flag legacy_format = (header.find("branch_inst_uid") == std::string::npos);

  std::string row;
  while (std::getline(f, row)) {
    if (row.empty())
      continue;

    std::istringstream ss(row);
    std::string tok;
    int pid;
    uns64 branch_inst_uid = 0;
    uns64 feeder_inst_uid;
    uns depth;

    std::getline(ss, tok, ',');
    pid = std::stoi(tok);

    if (legacy_format) {
      /* proc_id, feeder_inst_uid, depth */
      std::getline(ss, tok, ',');
      feeder_inst_uid = (uns64)std::stoull(tok);
      std::getline(ss, tok, ',');
      depth = (uns)std::stoul(tok);
    } else {
      /* proc_id, branch_inst_uid, feeder_inst_uid, depth */
      std::getline(ss, tok, ',');
      branch_inst_uid = (uns64)std::stoull(tok);
      std::getline(ss, tok, ',');
      feeder_inst_uid = (uns64)std::stoull(tok);
      std::getline(ss, tok, ',');
      depth = (uns)std::stoul(tok);
    }

    if (pid >= 0 && (uns8)pid < num_cores)
      bld_replay_add_pair((uns8)pid, branch_inst_uid, feeder_inst_uid, depth);
  }

  if (legacy_format) {
    fprintf(stderr,
            "[BLD] Warning: legacy 3-column trace (no branch_inst_uid) in %s; "
            "re-record for per-branch depth semantics.\n",
            trace_path);
  }
}

void bld_init(uns8 num_cores) {
  g_replay_pairs.resize(num_cores);
  g_replay_feeder_depths.resize(num_cores);
  g_trace_path[0] = '\0';
  g_trace_rows_written = 0;

  if (BRANCH_LOAD_DEP_TRACE_REPLAY) {
    char trace_path[MAX_STR_LENGTH + 1];
    bld_trace_path(trace_path, sizeof(trace_path));
    bld_load_replay_csv(trace_path, num_cores);
  }
}

void bld_process_op(uns8 proc_id, Op* op) {
  /* Replay: mark feeder loads by inst_uid so dcache gives them hit latency.
   * Runs independently of BRANCH_LOAD_DEP.  Off-path ops are skipped. */
  if (BRANCH_LOAD_DEP_TRACE_REPLAY && !op->off_path) {
    uns replay_max_depth = BRANCH_LOAD_DEP_MAX_DEPTH;
    if (bld_replay_feeder_included(proc_id, op->inst_uid, replay_max_depth))
      op->feeds_branch = TRUE;
  }

  if (!BRANCH_LOAD_DEP)
    return;

  if (op->off_path)
    return;

  if (op->inst_info->table_info.cf_type == NOT_CF)
    return;

  /* BFS backward through REG_DATA_DEP src_info edges.  Mark any load found;
   * traverse through ALU ops to find loads beyond them.  Stop once
   * BRANCH_LOAD_DEP_MAX_DEPTH loads have been marked (0 = unlimited).
   * src_info is populated by map_op(), so bld_process_op must be called after map. */
  uns max_loads = BRANCH_LOAD_DEP_MAX_DEPTH;
  uns loads_marked = 0;
  std::unordered_set<Counter> visited;
  std::deque<Op*> worklist;

  for (uns i = 0; i < op->num_srcs; i++) {
    Src_Info* s = &op->src_info[i];
    if (s->type != REG_DATA_DEP) continue;
    Op* src = s->op;
    if (!src || !src->op_pool_valid || src->unique_num != s->unique_num) continue;
    worklist.push_back(src);
  }

  while (!worklist.empty()) {
    if (max_loads > 0 && loads_marked >= max_loads)
      break;

    Op* cur = worklist.front();
    worklist.pop_front();

    if (!visited.insert(cur->unique_num).second)
      continue;

    if (cur->inst_info->table_info.mem_type == MEM_LD) {
      uns depth = loads_marked;
      cur->feeds_branch = TRUE;
      loads_marked++;

      Cache* dcache = get_dcache_for_proc(proc_id);
      if (cur->oracle_info.va != 0 && cache_set_feeds_branch(dcache, proc_id, cur->oracle_info.va))
        STAT_EVENT(proc_id, DCACHE_FEEDER_LINES_MARKED);

      bld_trace_record_pair(proc_id, op->inst_uid, cur->inst_uid, depth);

      if (BRANCH_LOAD_DEP_PREFETCH && cur->oracle_info.va != 0 && model->mem == MODEL_MEM) {
        Addr line_addr;
        if (!cache_access(dcache, cur->oracle_info.va, &line_addr, FALSE))
          new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL,
                      dcache_fill_line, 0, NULL);
      }
    }

    /* Continue BFS through this op's sources regardless of whether it was a load. */
    for (uns i = 0; i < cur->num_srcs; i++) {
      Src_Info* s = &cur->src_info[i];
      if (s->type != REG_DATA_DEP) continue;
      Op* src = s->op;
      if (!src || !src->op_pool_valid || src->unique_num != s->unique_num) continue;
      worklist.push_back(src);
    }
  }
}

void bld_finish(void) {
  if (g_trace_out) {
    fflush(g_trace_out);
    fclose(g_trace_out);
    g_trace_out = nullptr;
    fprintf(stderr, "[BLD] Wrote %" PRIu64 " trace rows to %s\n",
            (uint64_t)g_trace_rows_written, g_trace_path);
  } else if (BRANCH_LOAD_DEP_TRACE_RECORD) {
    fprintf(stderr, "[BLD] Warning: trace recording enabled but no rows were written\n");
  }
}
