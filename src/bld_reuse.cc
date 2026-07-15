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
 * File         : bld_reuse.cc
 * Description  : Reuse-distance study for branch-delaying loads. See bld_reuse.h.
 ***************************************************************************************/

#include "bld_reuse.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>

extern "C" {
#include "globals/global_defs.h"
#include "globals/utils.h"

#include "core.param.h"
#include "memory/memory.param.h"
}

/* ------------------------------------------------------------------------- */
/* Exact stack (reuse) distance over a reference stream, via an order-statistics
 * tree of live per-line timestamps (Olken's algorithm). access(line) returns the
 * number of DISTINCT lines touched since `line` was last seen, or NOT_TRACKED if
 * the line is not currently tracked (first touch, or aged past the cap). At most
 * `cap` distinct lines are tracked; the least-recently-used is dropped on
 * overflow (cap == 0 means unbounded).                                        */
/* ------------------------------------------------------------------------- */

typedef __gnu_pbds::tree<uint64_t, __gnu_pbds::null_type, std::less<uint64_t>,
                         __gnu_pbds::rb_tree_tag,
                         __gnu_pbds::tree_order_statistics_node_update>
    OrderStatTree;

static constexpr uint64_t SD_NOT_TRACKED = UINT64_MAX;

struct StackDist {
  OrderStatTree                        live;         // set of live timestamps
  std::unordered_map<Addr, uint64_t>   ts;           // line -> its live timestamp
  std::unordered_map<uint64_t, Addr>   line_at_ts;   // timestamp -> line (for evict)
  uint64_t                             clock = 1;
  uint64_t                             cap   = 0;

  uint64_t access(Addr line) {
    uint64_t dist = SD_NOT_TRACKED;
    auto     it   = ts.find(line);
    if(it != ts.end()) {
      uint64_t prev = it->second;
      /* #live timestamps strictly greater than prev == #distinct more-recent lines */
      dist = live.size() - live.order_of_key(prev) - 1;
      live.erase(prev);
      line_at_ts.erase(prev);
    }

    uint64_t now = clock++;
    live.insert(now);
    ts[line]         = now;
    line_at_ts[now]  = line;

    if(cap && (uint64_t)ts.size() > cap) {
      uint64_t oldest = *live.begin();
      live.erase(live.begin());
      auto lit = line_at_ts.find(oldest);
      if(lit != line_at_ts.end()) {
        ts.erase(lit->second);
        line_at_ts.erase(lit);
      }
    }
    return dist;
  }
};

/* ------------------------------------------------------------------------- */
/* Per-core study state.                                                       */
/* ------------------------------------------------------------------------- */

static constexpr int BLD_REUSE_NB = 28;  // log2 buckets: bucket b ~ cache size 2^b lines

static inline int bld_reuse_bucket(uint64_t dist) {
  uint64_t size = dist + 1;  // fully-assoc cache size (lines) that would capture the reuse
  int      b    = 63 - __builtin_clzll(size);
  if(b < 0)
    b = 0;
  if(b > BLD_REUSE_NB - 1)
    b = BLD_REUSE_NB - 1;
  return b;
}

struct ReuseState {
  StackDist full;    // advanced on every on-path demand access
  StackDist feeder;  // advanced only on delaying-load accesses

  std::unordered_map<Addr, uint32_t> delaying_pcs;    // delaying load PCs (value unused)
  std::unordered_map<Addr, uint32_t> feeder_count;    // delaying line -> #delaying-load accesses

  // feeder-only stream (per delaying-load access)
  Counter feeder_accesses    = 0;
  Counter feeder_reuse       = 0;  // was tracked -> a real reuse distance recorded
  Counter feeder_cold        = 0;  // first ever touch by a delaying load
  Counter feeder_overflow    = 0;  // seen before but aged past cap
  Counter feeder_dist_sum    = 0;
  Counter feeder_hist[BLD_REUSE_NB]        = {0};
  Counter feeder_hist_hit[BLD_REUSE_NB]    = {0};
  Counter feeder_hist_miss[BLD_REUSE_NB]   = {0};

  // full-stream depth, evaluated at each delaying-load access
  Counter full_reuse         = 0;
  Counter full_notracked     = 0;  // cold or aged past cap in the full stream
  Counter full_dist_sum      = 0;
  Counter full_hist[BLD_REUSE_NB]      = {0};
  Counter full_hist_hit[BLD_REUSE_NB]  = {0};
  Counter full_hist_miss[BLD_REUSE_NB] = {0};

  // real-L1 outcome at delaying-load accesses
  Counter real_hit  = 0;
  Counter real_miss = 0;
};

static std::vector<ReuseState> g_reuse;
static bool                    g_reuse_active = false;

/* ------------------------------------------------------------------------- */

void bld_reuse_init(uns8 num_cores) {
  if(!BRANCH_LOAD_DEP_REUSE_STUDY)
    return;
  g_reuse_active = true;
  g_reuse.clear();
  g_reuse.resize(num_cores);
  uint64_t cap = (uint64_t)BRANCH_LOAD_DEP_REUSE_CAP;
  for(auto& s : g_reuse) {
    s.full.cap   = cap;
    s.feeder.cap = cap;
  }
}

void bld_reuse_mark_delaying_pc(uns8 proc_id, Addr pc) {
  if(!g_reuse_active || proc_id >= g_reuse.size())
    return;
  g_reuse[proc_id].delaying_pcs[pc] = 1;
}

void bld_reuse_note_access(uns8 proc_id, Addr pc, Addr line_addr, Flag is_load, Flag real_hit) {
  if(!g_reuse_active || proc_id >= g_reuse.size())
    return;
  ReuseState& s = g_reuse[proc_id];

  /* Advance the full demand stream for every access; capture this line's depth. */
  uint64_t full_dist = s.full.access(line_addr);

  if(!is_load || s.delaying_pcs.find(pc) == s.delaying_pcs.end())
    return;  // only delaying loads contribute to the feeder measurement

  s.feeder_accesses++;
  if(real_hit)
    s.real_hit++;
  else
    s.real_miss++;

  /* ---- feeder-only stream ---- */
  uint32_t& cnt = s.feeder_count[line_addr];
  cnt++;
  uint64_t feeder_dist = s.feeder.access(line_addr);
  if(feeder_dist != SD_NOT_TRACKED) {
    int b = bld_reuse_bucket(feeder_dist);
    s.feeder_reuse++;
    s.feeder_dist_sum += feeder_dist;
    s.feeder_hist[b]++;
    if(real_hit)
      s.feeder_hist_hit[b]++;
    else
      s.feeder_hist_miss[b]++;
  } else if(cnt <= 1) {
    s.feeder_cold++;  // first ever delaying-load touch of this line
  } else {
    s.feeder_overflow++;  // seen before but aged past the cap
  }

  /* ---- full-stream depth at this delaying-load access ---- */
  if(full_dist != SD_NOT_TRACKED) {
    int b = bld_reuse_bucket(full_dist);
    s.full_reuse++;
    s.full_dist_sum += full_dist;
    s.full_hist[b]++;
    if(real_hit)
      s.full_hist_hit[b]++;
    else
      s.full_hist_miss[b]++;
  } else {
    s.full_notracked++;
  }
}

/* ------------------------------------------------------------------------- */
/* Output path: <trace_dir>/<bench>_<simpt>_reuse.csv, mirroring bld_trace_path. */
/* ------------------------------------------------------------------------- */

static void bld_reuse_path(char* out, size_t sz) {
  const char* dir   = BRANCH_LOAD_DEP_TRACE_DIR;
  const char* trace = CBP_TRACE_R0;

  if(!trace || !*trace) {
    snprintf(out, sz, "%s/bld_reuse.csv", dir);
    return;
  }

  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, trace, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  char*       slash    = strrchr(tmp, '/');
  const char* filename = slash ? slash + 1 : tmp;
  char        simpt[64];
  const char* dot = strchr(filename, '.');
  if(dot) {
    size_t n = (size_t)(dot - filename) < sizeof(simpt) - 1 ? (size_t)(dot - filename) : sizeof(simpt) - 1;
    strncpy(simpt, filename, n);
    simpt[n] = '\0';
  } else {
    strncpy(simpt, filename, sizeof(simpt) - 1);
    simpt[sizeof(simpt) - 1] = '\0';
  }

  if(slash)
    *slash = '\0';
  for(int lvl = 0; lvl < 2; lvl++) {
    char* p = strrchr(tmp, '/');
    if(p)
      *p = '\0';
  }
  const char* bench_slash = strrchr(tmp, '/');
  const char* bench       = bench_slash ? bench_slash + 1 : tmp;

  snprintf(out, sz, "%s/%s_%s_reuse.csv", dir, bench, simpt);
}

static void bld_reuse_emit_hist(FILE* f, uns8 proc, const char* stream, const char* outcome,
                                const Counter* hist) {
  for(int b = 0; b < BLD_REUSE_NB; b++) {
    if(hist[b] == 0)
      continue;
    fprintf(f, "%u,hist,%s,%s,%d,%llu,%llu\n", (unsigned)proc, stream, outcome, b,
            (unsigned long long)((uint64_t)1 << b), (unsigned long long)hist[b]);
  }
}

void bld_reuse_finish(void) {
  if(!g_reuse_active)
    return;

  char path[MAX_STR_LENGTH + 1];
  bld_reuse_path(path, sizeof(path));
  FILE* f = fopen(path, "w");
  if(!f) {
    fprintf(stderr, "[BLD_REUSE] cannot open %s for writing\n", path);
    g_reuse.clear();
    g_reuse_active = false;
    return;
  }

  fprintf(f, "# reuse-distance study for branch-delaying loads\n");
  fprintf(f, "# cap=%u line_bytes=%u\n", (unsigned)BRANCH_LOAD_DEP_REUSE_CAP, (unsigned)DCACHE_LINE_SIZE);
  fprintf(f, "# feeder stream = delaying-load accesses only; full stream = all on-path demand accesses\n");
  fprintf(f, "# stack distance = distinct lines since last access; bucket b -> fully-assoc cache size ~2^b lines\n");
  fprintf(f, "# rows: <proc>,summary,<key>,<value>  and  <proc>,hist,<stream>,<outcome>,<bucket>,<cache_size_lines>,<count>\n");

  for(uns8 proc = 0; proc < g_reuse.size(); proc++) {
    ReuseState& s = g_reuse[proc];
    if(s.feeder_accesses == 0 && s.delaying_pcs.empty())
      continue;

    uint64_t distinct = s.feeder_count.size();
    uint64_t reused   = 0;
    for(const auto& kv : s.feeder_count)
      if(kv.second >= 2)
        reused++;

    double pct_reused    = distinct ? 100.0 * (double)reused / (double)distinct : 0.0;
    double feeder_mean   = s.feeder_reuse ? (double)s.feeder_dist_sum / (double)s.feeder_reuse : 0.0;
    double full_mean     = s.full_reuse ? (double)s.full_dist_sum / (double)s.full_reuse : 0.0;

    fprintf(f, "%u,summary,delaying_pcs,%llu\n", proc, (unsigned long long)s.delaying_pcs.size());
    fprintf(f, "%u,summary,delaying_load_accesses,%llu\n", proc, (unsigned long long)s.feeder_accesses);
    fprintf(f, "%u,summary,distinct_delaying_lines,%llu\n", proc, (unsigned long long)distinct);
    fprintf(f, "%u,summary,reused_delaying_lines,%llu\n", proc, (unsigned long long)reused);
    fprintf(f, "%u,summary,pct_reused,%.4f\n", proc, pct_reused);
    fprintf(f, "%u,summary,real_hit,%llu\n", proc, (unsigned long long)s.real_hit);
    fprintf(f, "%u,summary,real_miss,%llu\n", proc, (unsigned long long)s.real_miss);
    fprintf(f, "%u,summary,feeder_reuse_recorded,%llu\n", proc, (unsigned long long)s.feeder_reuse);
    fprintf(f, "%u,summary,feeder_cold,%llu\n", proc, (unsigned long long)s.feeder_cold);
    fprintf(f, "%u,summary,feeder_overflow,%llu\n", proc, (unsigned long long)s.feeder_overflow);
    fprintf(f, "%u,summary,feeder_mean_stack_dist,%.4f\n", proc, feeder_mean);
    fprintf(f, "%u,summary,full_reuse_recorded,%llu\n", proc, (unsigned long long)s.full_reuse);
    fprintf(f, "%u,summary,full_notracked,%llu\n", proc, (unsigned long long)s.full_notracked);
    fprintf(f, "%u,summary,full_mean_stack_dist,%.4f\n", proc, full_mean);

    bld_reuse_emit_hist(f, proc, "feeder", "all", s.feeder_hist);
    bld_reuse_emit_hist(f, proc, "feeder", "realhit", s.feeder_hist_hit);
    bld_reuse_emit_hist(f, proc, "feeder", "realmiss", s.feeder_hist_miss);
    bld_reuse_emit_hist(f, proc, "full", "all", s.full_hist);
    bld_reuse_emit_hist(f, proc, "full", "realhit", s.full_hist_hit);
    bld_reuse_emit_hist(f, proc, "full", "realmiss", s.full_hist_miss);
  }

  fclose(f);
  fprintf(stderr, "[BLD_REUSE] wrote %s\n", path);
  g_reuse.clear();
  g_reuse_active = false;
}
