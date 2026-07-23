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
 * File         : br_exec_wait.cc
 * Description  : Per-cycle branch operand-delay stats (see header).
 ***************************************************************************************/

#include "br_exec_wait.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bld_reuse.h"
#include "branch_load_dep.h"

extern "C" {
#include "core.param.h"
#include "globals/global_defs.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "inst_info.h"
#include "libs/cache_lib.h"
#include "memory/mem_req.h"
#include "memory/memory.h"
#include "memory/memory.param.h"
#include "model.h"
#include "dcache_stage.h"
#include "op.h"
#include "op_info.h"
#include "op_pool.h"
#include "statistics.h"
#include "table_info.h"
}

#include "cmp_model.h"

/* Cycle when this branch would complete exec (and retire for CF) if it had no
 * operand dependencies: one cycle after ROB issue to reach the RS, then branch
 * execution latency. */
static Counter br_hypothetical_resolve_cycle(Op* op) {
  return op->issue_cycle + 1 + (Counter)abs(op->inst_info->latency);
}

static Flag br_op_in_operand_delay_window(Op* op, Counter now) {
  if (op->off_path || op->inst_info->table_info.cf_type == NOT_CF)
    return FALSE;
  if (!op->bp_pred_info->recover_at_exec)
    return FALSE;
  if (!op->bp_pred_info->mispred && !op->bp_pred_info->misfetch)
    return FALSE;
  /* Past hypothetical resolve but still waiting on at least one operand. */
  if (op_sources_not_rdy_is_clear(op))
    return FALSE;

  Counter hypo = br_hypothetical_resolve_cycle(op);
  if (now < hypo)
    return FALSE;
  if (op->exec_cycle <= now)
    return FALSE;
  if (op->exec_cycle != MAX_CTR && hypo >= op->exec_cycle)
    return FALSE;
  return TRUE;
}

/* Per-cycle bottleneck attribution for a stalled mispredicted branch.
 *
 * We walk the branch's not-ready dependency cone (following only not-ready
 * source edges through live ops) and reduce it to the single most-severe
 * bottleneck currently present, classified by the blocking op's pipeline
 * state.  Interior ops that are themselves just waiting on sources are
 * transparent (we descend through them); the "leaf" ops doing actual work
 * are what we charge.  Memory stalls dominate the severity order because a
 * D$ miss / MSHR wait is the long pole whenever one is present in the cone. */

enum BrStallBucket {
  BR_STALL_DCACHE_MISS = 0,  // OS_MISS        - load missed in D$ (memory latency)
  BR_STALL_MEM_BUFFER,       // OS_WAIT_MEM    - waiting for a miss-buffer/MSHR entry
  BR_STALL_DCACHE_PORT,      // OS_WAIT_DCACHE - waiting for a D$ port
  BR_STALL_LOAD_LATENCY,     // OS_SCHEDULED load - load executing (hit latency)
  BR_STALL_EXEC_PORT,        // OS_READY/TENTATIVE - data-ready, awaiting a FU/scheduler
  BR_STALL_ALU_LATENCY,      // OS_SCHEDULED non-load - ALU executing (compute latency)
  BR_STALL_NOT_ISSUED,       // OS_FETCHED     - producer not yet dispatched into the window
  BR_STALL_UNRESOLVED,       // no live leaf found (producers retired / interior-only)
};

static Op* br_live_src_op(const Src_Info* s) {
  if (s->op && s->op != &invalid_op && s->op->op_pool_valid &&
      s->op->unique_num == s->unique_num)
    return s->op;
  return nullptr;
}

static Flag br_op_done(Op* op, Counter now) {
  return op->state == OS_DONE || op->done_cycle <= now;
}

/* Push op's still-in-flight (not-done) live producers onto the worklist.
 * We follow src_info[].op directly rather than the not-ready bit, because a
 * producer that has been speculatively woken (OS_WAIT_FWD) clears the bit while
 * its value is still forwarding -- following bits would lose the chain there.
 * Returns the number of not-done producers found. */
static uns br_push_inflight_producers(Op* op, Counter now, std::deque<Op*>& worklist,
                                      std::unordered_set<Op*>& visited) {
  uns added = 0;
  for (uns i = 0; i < op->num_srcs; i++) {
    Op* p = br_live_src_op(&op->src_info[i]);
    if (!p || br_op_done(p, now))
      continue;
    added++;
    /* Key on the Op pointer, not unique_num: a cracked x86 instruction expands
     * into several micro-ops that all share one unique_num, so keying on
     * unique_num would collapse the intra-instruction uop chain and lose the
     * real producer (e.g. a missing load) sitting a couple of uops deeper. */
    if (visited.insert(p).second)
      worklist.push_back(p);
  }
  return added;
}

/* Returns (severity, bucket) for a leaf op's state; severity < 0 means the op is
 * an interior node we should descend through rather than charge. */
static int br_leaf_severity(Op* op, int* bucket_out) {
  Flag is_load = (op->inst_info->table_info.mem_type == MEM_LD);
  switch (op->state) {
    case OS_MISS:        *bucket_out = BR_STALL_DCACHE_MISS;  return 80;
    case OS_WAIT_MEM:    *bucket_out = BR_STALL_MEM_BUFFER;   return 75;
    case OS_WAIT_DCACHE: *bucket_out = BR_STALL_DCACHE_PORT;  return 70;
    case OS_SCHEDULED:  // executing: address-gen/data for a load, compute for an ALU
      *bucket_out = is_load ? BR_STALL_LOAD_LATENCY : BR_STALL_ALU_LATENCY;
      return is_load ? 60 : 30;
    case OS_READY:
    case OS_TENTATIVE:   *bucket_out = BR_STALL_EXEC_PORT;    return 50;
    case OS_FETCHED:     *bucket_out = BR_STALL_NOT_ISSUED;   return 20;
    default:             return -1;  // IN_ROB/IN_RS/SLEEP/WAIT_FWD/LOW_PRIORITY: interior
  }
}

static int br_classify_operand_stall(Op* branch_op, Counter now) {
  std::deque<Op*> worklist;
  std::unordered_set<Op*> visited;

  br_push_inflight_producers(branch_op, now, worklist, visited);

  int best_sev = -1;
  int best_bucket = BR_STALL_UNRESOLVED;
  uns guard = 0;

  while (!worklist.empty() && guard++ < 4096) {
    Op* op = worklist.front();
    worklist.pop_front();

    int bucket = BR_STALL_UNRESOLVED;
    int sev = br_leaf_severity(op, &bucket);

    if (sev < 0) {
      /* Interior op (waiting on sources or forwarding): descend through its
       * in-flight producers. If it has none, it is itself ready-but-unscheduled
       * (inputs all done, awaiting a FU/forward slot) -> charge EXEC_PORT. */
      if (br_push_inflight_producers(op, now, worklist, visited) == 0) {
        sev = 50;
        bucket = BR_STALL_EXEC_PORT;
      }
    }

    if (sev > best_sev) {
      best_sev = sev;
      best_bucket = bucket;
    }
  }

  return (best_sev < 0) ? BR_STALL_UNRESOLVED : best_bucket;
}

static void br_charge_operand_stall(uns8 proc_id, Op* branch_op, Counter now) {
  switch (br_classify_operand_stall(branch_op, now)) {
    case BR_STALL_DCACHE_MISS:  INC_STAT_EVENT(proc_id, BR_WAIT_DCACHE_MISS, 1);  break;
    case BR_STALL_MEM_BUFFER:   INC_STAT_EVENT(proc_id, BR_WAIT_MEM_BUFFER, 1);   break;
    case BR_STALL_DCACHE_PORT:  INC_STAT_EVENT(proc_id, BR_WAIT_DCACHE_PORT, 1);  break;
    case BR_STALL_LOAD_LATENCY: INC_STAT_EVENT(proc_id, BR_WAIT_LOAD_LATENCY, 1); break;
    case BR_STALL_EXEC_PORT:    INC_STAT_EVENT(proc_id, BR_WAIT_EXEC_PORT, 1);    break;
    case BR_STALL_ALU_LATENCY:  INC_STAT_EVENT(proc_id, BR_WAIT_ALU_LATENCY, 1);  break;
    case BR_STALL_NOT_ISSUED:   INC_STAT_EVENT(proc_id, BR_WAIT_NOT_ISSUED, 1);   break;
    default:                    INC_STAT_EVENT(proc_id, BR_WAIT_UNRESOLVED, 1);   break;
  }
}

/* ===================================================================== *
 * Branch-delay load attribution (--branch_load_dep_attribute_record) and
 * address-based replay (--branch_load_dep_addr_replay).
 *
 * For every cycle a mispredicted branch is stalled past its zero-operand
 * resolve point, charge that cycle to ONE prefetchable load, split into:
 *   A (ANCESTOR):    the critical outstanding D$ miss inside the branch's own
 *                    dataflow cone -- prefetching it directly speeds the branch.
 *   B (WINDOW_CLOG): when the cone has no outstanding miss, the oldest in-flight
 *                    load miss at the ROB head -- it clogs the window and holds
 *                    the branch (and everything else) back; prefetching it
 *                    unblocks retirement.
 *   (neither):       no outstanding miss anywhere -- operand-latency/exec-port
 *                    bound, not prefetchable; counted in g_none_total.
 * We record both the load PC and its DATA cacheline (va & ~(line-1)), and flag
 * whether that line's miss is compulsory (first global miss = cold data) vs a
 * repeat miss (warm/evicted).  Output: branch_delay_loads.csv (per simpoint dir)
 * plus a flat line,cycles file in the trace dir that addr-replay reads back.
 * ===================================================================== */

static Addr br_line_of(Addr va) {
  return va & ~(Addr)(DCACHE_LINE_SIZE - 1);
}

static Flag br_op_is_load_miss(Op* op) {
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return FALSE;
  return op->state == OS_MISS || op->state == OS_WAIT_MEM;
}

/* Walk the branch's not-ready cone; return the deepest outstanding load miss
 * (longest serial tail = the load most responsible for the delay), or NULL. */
static Op* br_find_cone_critical_miss(Op* branch_op, Counter now) {
  std::deque<std::pair<Op*, int>> worklist;
  std::unordered_set<Op*> visited;
  for (uns i = 0; i < branch_op->num_srcs; i++) {
    Op* p = br_live_src_op(&branch_op->src_info[i]);
    if (!p || br_op_done(p, now))
      continue;
    if (visited.insert(p).second)
      worklist.push_back({p, 1});
  }

  Op* best = nullptr;
  int best_depth = -1;
  uns guard = 0;
  while (!worklist.empty() && guard++ < 4096) {
    Op* op = worklist.front().first;
    int depth = worklist.front().second;
    worklist.pop_front();

    if (br_op_is_load_miss(op) && depth > best_depth) {
      best = op;
      best_depth = depth;
    }
    /* Descend through producers (pointer-keyed visited: do not collapse the
     * micro-op chain of one cracked x86 instruction). */
    for (uns i = 0; i < op->num_srcs; i++) {
      Op* p = br_live_src_op(&op->src_info[i]);
      if (!p || br_op_done(p, now))
        continue;
      if (visited.insert(p).second)
        worklist.push_back({p, depth + 1});
    }
  }
  return best;
}

/* Oldest (closest to the ROB head) in-flight load miss in the machine -- the
 * window-clog culprit. node_head is the oldest op in program order. */
static Op* br_find_window_clog_miss(uns8 proc_id) {
  for (Op* op = cmp_model.node_stage[proc_id].node_head; op; op = op->next_node) {
    if (!op->op_pool_valid)
      continue;
    if (br_op_is_load_miss(op))
      return op;
  }
  return nullptr;
}

struct BrLineRec {
  Counter cycles = 0;  // stall cycles charged to this (pc,line)
  Counter cold = 0;    // of those, cycles where the line's miss was compulsory
};
/* category -> pc -> line -> record */
static std::unordered_map<Addr, std::unordered_map<Addr, BrLineRec>> g_anc;
static std::unordered_map<Addr, std::unordered_map<Addr, BrLineRec>> g_clog;
static Counter g_anc_total = 0;
static Counter g_clog_total = 0;
static Counter g_none_total = 0;
static Counter g_cold_total = 0;
/* Global per-line load-miss count, populated from the dcache on every on-path
 * load miss; lets us tell a compulsory (cold) miss from a repeat (warm) one. */
static std::unordered_map<Addr, Counter> g_line_miss_count;

/* Per-dynamic-instance prefetch triggers for the seqnum-driven prefetcher: one
 * (seqnum, line) per delaying-load instance. seqnum is the load's on-path
 * retire-order op_num; an off-path window-clog load has no stable op_num, so it
 * anchors to the on-path branch's op_num. Deduped on unique_num (globally unique
 * across recovery, unlike op_num) so a load charged over many stall cycles is
 * written once. Flushed sorted by seqnum to <bench>_<simpt>_pf.csv. */
static std::vector<std::pair<Counter, Addr>> g_pf_rows;
static std::unordered_set<Counter> g_pf_seen;

void br_note_load_miss(Addr line_addr) {
  g_line_miss_count[br_line_of(line_addr)]++;
}

static void br_charge_line(std::unordered_map<Addr, std::unordered_map<Addr, BrLineRec>>& cat,
                           Op* miss_op, Counter* cat_total) {
  Addr pc = miss_op->inst_info->addr;
  Addr line = br_line_of(miss_op->oracle_info.va);
  if (BRANCH_LOAD_DEP_REUSE_STUDY)
    bld_reuse_mark_delaying_pc(miss_op->proc_id, pc);
  BrLineRec& r = cat[pc][line];
  r.cycles++;
  (*cat_total)++;
  /* Compulsory if this is the only time the line has ever missed. */
  if (g_line_miss_count[line] <= 1) {
    r.cold++;
    g_cold_total++;
  }
}

/* Emit one prefetch trigger per dynamic delaying-load instance. Deduped on
 * unique_num so a load charged over many stall cycles is written once. On-path
 * loads record their own op_num; an off-path window-clog load has no stable
 * op_num, so it anchors to the on-path branch (the consumer we are helping). */
static void br_pf_emit(Op* m, Op* branch_op) {
  if (!g_pf_seen.insert(m->unique_num).second)
    return;
  Counter seqnum = m->off_path ? branch_op->op_num : m->op_num;
  g_pf_rows.push_back({seqnum, br_line_of(m->oracle_info.va)});
}

static void br_record_delay_load(uns8 proc_id, Op* branch_op, Counter now) {
  Op* m = br_find_cone_critical_miss(branch_op, now);
  if (m) {
    br_charge_line(g_anc, m, &g_anc_total);
    br_pf_emit(m, branch_op);
  } else if ((m = br_find_window_clog_miss(proc_id)) != nullptr) {
    br_charge_line(g_clog, m, &g_clog_total);
    br_pf_emit(m, branch_op);
  } else {
    g_none_total++;
  }
}

/* trace-dir path for the flat replay file, named <bench>_<simpt>_braddr.csv to
 * mirror the bld trace naming so a replay run finds the matching simpoint. */
static void br_braddr_path(char* out, size_t sz) {
  const char* dir = BRANCH_LOAD_DEP_TRACE_DIR;
  const char* trace = CBP_TRACE_R0;
  if (!trace || !*trace) {
    snprintf(out, sz, "%s/branch_delay_braddr.csv", dir);
    return;
  }
  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, trace, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  char* slash = strrchr(tmp, '/');
  const char* filename = slash ? slash + 1 : tmp;
  char simpt[64];
  const char* dot = strchr(filename, '.');
  size_t n = dot ? (size_t)(dot - filename) : strlen(filename);
  if (n > sizeof(simpt) - 1)
    n = sizeof(simpt) - 1;
  strncpy(simpt, filename, n);
  simpt[n] = '\0';
  if (slash)
    *slash = '\0';
  for (int lvl = 0; lvl < 2; lvl++) {
    char* p = strrchr(tmp, '/');
    if (p)
      *p = '\0';
  }
  const char* bench_slash = strrchr(tmp, '/');
  const char* bench = bench_slash ? bench_slash + 1 : tmp;
  snprintf(out, sz, "%s/%s_%s_braddr.csv", dir, bench, simpt);
}

/* Same <bench>_<simpt> derivation as br_braddr_path, for the non-feeder
 * displacement pool file. */
static void br_nfpool_path(char* out, size_t sz) {
  const char* dir = BRANCH_LOAD_DEP_TRACE_DIR;
  const char* trace = CBP_TRACE_R0;
  if (!trace || !*trace) {
    snprintf(out, sz, "%s/branch_delay_nfpool.csv", dir);
    return;
  }
  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, trace, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  char* slash = strrchr(tmp, '/');
  const char* filename = slash ? slash + 1 : tmp;
  char simpt[64];
  const char* dot = strchr(filename, '.');
  size_t n = dot ? (size_t)(dot - filename) : strlen(filename);
  if (n > sizeof(simpt) - 1)
    n = sizeof(simpt) - 1;
  strncpy(simpt, filename, n);
  simpt[n] = '\0';
  if (slash)
    *slash = '\0';
  for (int lvl = 0; lvl < 2; lvl++) {
    char* p = strrchr(tmp, '/');
    if (p)
      *p = '\0';
  }
  const char* bench_slash = strrchr(tmp, '/');
  const char* bench = bench_slash ? bench_slash + 1 : tmp;
  snprintf(out, sz, "%s/%s_%s_nfpool.csv", dir, bench, simpt);
}

/* Generic <dir>/<bench>_<simpt>_<suffix>.csv derivation (same scheme as
 * br_braddr_path / br_nfpool_path) for additional per-simpoint files. The
 * directory is a parameter so record (trace dir) and replay (pf/coverage dirs)
 * can point at different locations. */
static void br_trace_suffix_path(char* out, size_t sz, const char* dir, const char* suffix) {
  const char* trace = CBP_TRACE_R0;
  if (!trace || !*trace) {
    snprintf(out, sz, "%s/branch_delay_%s.csv", dir, suffix);
    return;
  }
  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, trace, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  char* slash = strrchr(tmp, '/');
  const char* filename = slash ? slash + 1 : tmp;
  char simpt[64];
  const char* dot = strchr(filename, '.');
  size_t n = dot ? (size_t)(dot - filename) : strlen(filename);
  if (n > sizeof(simpt) - 1)
    n = sizeof(simpt) - 1;
  strncpy(simpt, filename, n);
  simpt[n] = '\0';
  if (slash)
    *slash = '\0';
  for (int lvl = 0; lvl < 2; lvl++) {
    char* p = strrchr(tmp, '/');
    if (p)
      *p = '\0';
  }
  const char* bench_slash = strrchr(tmp, '/');
  const char* bench = bench_slash ? bench_slash + 1 : tmp;
  snprintf(out, sz, "%s/%s_%s_%s.csv", dir, bench, simpt, suffix);
}

void br_exec_wait_finish(void) {
  if (!BRANCH_LOAD_DEP_ATTRIBUTE_RECORD)
    return;

  /* Merge A+B per-line cycle totals for the flat replay file. */
  std::unordered_map<Addr, Counter> line_cycles;
  auto accumulate = [&](std::unordered_map<Addr, std::unordered_map<Addr, BrLineRec>>& cat) {
    for (auto& pc_kv : cat)
      for (auto& ln_kv : pc_kv.second)
        line_cycles[ln_kv.first] += ln_kv.second.cycles;
  };
  accumulate(g_anc);
  accumulate(g_clog);

  Counter total = g_anc_total + g_clog_total + g_none_total;
  Counter pf = g_anc_total + g_clog_total;

  /* Rich per-(category,pc,line) CSV for analysis (simpoint cwd). */
  FILE* f = fopen("branch_delay_loads.csv", "w");
  if (f) {
    fprintf(f, "# branch-mispredict operand-wait cycles attributed to prefetchable loads\n");
    fprintf(f,
            "# total_wait_cycles=%llu ancestor=%llu window_clog=%llu none=%llu "
            "cold_cycles=%llu distinct_lines=%zu\n",
            (unsigned long long)total, (unsigned long long)g_anc_total,
            (unsigned long long)g_clog_total, (unsigned long long)g_none_total,
            (unsigned long long)g_cold_total, line_cycles.size());
    fprintf(f, "category,pc,line,cycles,cold_cycles\n");
    auto dump = [&](const char* name,
                    std::unordered_map<Addr, std::unordered_map<Addr, BrLineRec>>& cat) {
      typedef std::pair<std::pair<Addr, Addr>, BrLineRec> Entry;
      std::vector<Entry> v;
      for (auto& pc_kv : cat)
        for (auto& ln_kv : pc_kv.second)
          v.push_back({{pc_kv.first, ln_kv.first}, ln_kv.second});
      std::sort(v.begin(), v.end(),
                [](const Entry& a, const Entry& b) { return a.second.cycles > b.second.cycles; });
      for (auto& e : v)
        fprintf(f, "%s,0x%llx,0x%llx,%llu,%llu\n", name,
                (unsigned long long)e.first.first, (unsigned long long)e.first.second,
                (unsigned long long)e.second.cycles, (unsigned long long)e.second.cold);
    };
    dump("ANCESTOR", g_anc);
    dump("WINDOW_CLOG", g_clog);
    fclose(f);
  }

  /* Flat line,cycles file (sorted desc) for addr-replay, into the trace dir. */
  char path[MAX_STR_LENGTH + 1];
  br_braddr_path(path, sizeof(path));
  FILE* g = fopen(path, "w");
  if (g) {
    std::vector<std::pair<Addr, Counter>> v(line_cycles.begin(), line_cycles.end());
    std::sort(v.begin(), v.end(),
              [](const std::pair<Addr, Counter>& a, const std::pair<Addr, Counter>& b) {
                return a.second > b.second;
              });
    fprintf(g, "# total_prefetchable_cycles=%llu distinct_lines=%zu\n",
            (unsigned long long)pf, v.size());
    for (auto& kv : v)
      fprintf(g, "0x%llx,%llu\n", (unsigned long long)kv.first,
              (unsigned long long)kv.second);
    fclose(g);
  }

  /* Non-feeder displacement pool: every load line that missed L1 >=2x
   * (reused-and-evicted = the true retention candidates), sorted desc by miss
   * count and capped to pool_cap. The count-matched non-feeder oracle draws from
   * this at replay; feeder lines are filtered out there. */
  char npath[MAX_STR_LENGTH + 1];
  br_nfpool_path(npath, sizeof(npath));
  FILE* nf = fopen(npath, "w");
  if (nf) {
    std::vector<std::pair<Addr, Counter>> pool;
    for (auto& kv : g_line_miss_count)
      if (kv.second >= 2)
        pool.push_back(kv);
    std::sort(pool.begin(), pool.end(),
              [](const std::pair<Addr, Counter>& a, const std::pair<Addr, Counter>& b) {
                return a.second > b.second;
              });
    size_t cap = (size_t)BRANCH_LOAD_DEP_ADDR_REPLAY_POOL_CAP;
    size_t emit = (cap && pool.size() > cap) ? cap : pool.size();
    fprintf(nf, "# reused-missed non-feeder pool: load lines with L1 miss_count>=2\n");
    fprintf(nf, "# reused_missed_lines=%zu emitted=%zu cap=%zu\n", pool.size(), emit, cap);
    for (size_t i = 0; i < emit; i++)
      fprintf(nf, "0x%llx,%llu\n", (unsigned long long)pool[i].first,
              (unsigned long long)pool[i].second);
    fclose(nf);
  }

  /* Per-instance prefetch trigger stream for the seqnum-driven prefetcher:
   * (seqnum, line) sorted ascending by seqnum so a replay run advances a single
   * cursor. seqnum = on-path retire-order op_num; line = block-aligned address. */
  char pfpath[MAX_STR_LENGTH + 1];
  br_trace_suffix_path(pfpath, sizeof(pfpath), BRANCH_LOAD_DEP_TRACE_DIR, "pf");
  FILE* pfp = fopen(pfpath, "w");
  if (pfp) {
    std::sort(g_pf_rows.begin(), g_pf_rows.end(),
              [](const std::pair<Counter, Addr>& a, const std::pair<Counter, Addr>& b) {
                return a.first < b.first;
              });
    fprintf(pfp, "# per-instance branch-delay prefetch triggers, sorted by seqnum\n");
    fprintf(pfp, "# rows=%zu\n", g_pf_rows.size());
    fprintf(pfp, "seqnum,line\n");
    for (auto& r : g_pf_rows)
      fprintf(pfp, "%llu,0x%llx\n", (unsigned long long)r.first,
              (unsigned long long)r.second);
    fclose(pfp);
  }
}

/* ----------------------- address-based replay ------------------------ */

static Flag g_addr_replay_loaded = FALSE;
static std::unordered_set<Addr> g_addr_replay_set;

/* Non-feeder displacement oracle: count-matched random set drawn from the
 * reused-missed pool, disjoint from the feeder set. */
static Flag g_nf_replay_loaded = FALSE;
static std::unordered_set<Addr> g_nf_replay_set;

static uint64_t br_splitmix64(uint64_t* s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* Record the (target, selected) line counts to the simpoint cwd so the analysis
 * can confirm the feeder/non-feeder oracles retained an equal number of lines. */
static void br_replay_marker(const char* mode, uns target, uns selected) {
  FILE* m = fopen("replay_selection.csv", "a");
  if (m) {
    fprintf(m, "%s,%u,%u\n", mode, target, selected);
    fclose(m);
  }
}

static void br_addr_replay_load(void) {
  g_addr_replay_loaded = TRUE;
  char path[MAX_STR_LENGTH + 1];
  br_braddr_path(path, sizeof(path));
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "br_addr_replay: cannot open %s\n", path);
    return;
  }
  Counter total = 0;
  std::vector<std::pair<Addr, Counter>> lines;
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) {
    if (buf[0] == '#' || buf[0] == '\n')
      continue;
    unsigned long long line = 0, cyc = 0;
    if (sscanf(buf, "0x%llx,%llu", &line, &cyc) == 2) {
      lines.push_back({(Addr)line, (Counter)cyc});
      total += cyc;
    }
  }
  fclose(f);
  /* Take the smallest prefix (already sorted desc) covering the requested
   * fraction of recorded stall cycles. */
  double cov = (double)BRANCH_LOAD_DEP_ADDR_REPLAY_COVERAGE;
  Counter target = (Counter)(cov * (double)total);
  Counter acc = 0;
  uns picked = 0;
  for (auto& kv : lines) {
    g_addr_replay_set.insert(kv.first);
    acc += kv.second;
    picked++;
    if (acc >= target)
      break;
  }
  fprintf(stderr, "br_addr_replay: %u/%zu lines cover %.1f%% of %llu cycles (target %.0f%%)\n",
          picked, lines.size(), total ? 100.0 * (double)acc / (double)total : 0.0,
          (unsigned long long)total, 100.0 * cov);
  if (!BRANCH_LOAD_DEP_ADDR_REPLAY_NONFEEDER)
    br_replay_marker("feeder", picked, picked);
}

/* Build the count-matched non-feeder set: N = size of the feeder coverage set;
 * draw N random reused-missed lines (from nfpool.csv) that are not feeders. */
static void br_nf_replay_load(void) {
  g_nf_replay_loaded = TRUE;
  if (!g_addr_replay_loaded)
    br_addr_replay_load();  // feeder set + coverage prefix -> N
  uns N = (uns)g_addr_replay_set.size();

  char path[MAX_STR_LENGTH + 1];
  br_nfpool_path(path, sizeof(path));
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "br_nf_replay: cannot open %s\n", path);
    br_replay_marker("nonfeeder", N, 0);
    return;
  }
  std::vector<Addr> cand;  // distinct reused-missed non-feeder lines
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) {
    if (buf[0] == '#' || buf[0] == '\n')
      continue;
    unsigned long long line = 0, cnt = 0;
    if (sscanf(buf, "0x%llx,%llu", &line, &cnt) >= 1) {
      Addr L = br_line_of((Addr)line);
      if (!g_addr_replay_set.count(L))  // exclude feeders
        cand.push_back(L);
    }
  }
  fclose(f);

  /* Deterministic Fisher-Yates shuffle (seeded), then take the first N. */
  uint64_t state = (uint64_t)BRANCH_LOAD_DEP_ADDR_REPLAY_SEED;
  for (size_t i = cand.size(); i > 1;) {
    --i;
    size_t j = (size_t)(br_splitmix64(&state) % (uint64_t)(i + 1));
    std::swap(cand[i], cand[j]);
  }
  uns sel = 0;
  for (size_t i = 0; i < cand.size() && sel < N; i++)
    if (g_nf_replay_set.insert(cand[i]).second)
      sel++;

  fprintf(stderr,
          "br_nf_replay: selected %u/%u non-feeder lines (pool %zu, seed %u)\n",
          sel, N, cand.size(), (unsigned)BRANCH_LOAD_DEP_ADDR_REPLAY_SEED);
  br_replay_marker("nonfeeder", N, sel);
}

Flag br_addr_replay_hit(Addr line_addr) {
  if (BRANCH_LOAD_DEP_ADDR_REPLAY_NONFEEDER) {
    if (!g_nf_replay_loaded)
      br_nf_replay_load();
    return g_nf_replay_set.count(br_line_of(line_addr)) ? TRUE : FALSE;
  }
  if (!g_addr_replay_loaded)
    br_addr_replay_load();
  return g_addr_replay_set.count(br_line_of(line_addr)) ? TRUE : FALSE;
}

/* Per-core set of feeder lines observed so far, used to separate the first
   (cold/compulsory) access to a feeder line from later reuse. */
static std::unordered_map<uns8, std::unordered_set<Addr>> g_feeder_seen;

void feeder_access_profile_note(uns8 proc_id, Addr line_addr, Flag hit) {
  if (!BRANCH_LOAD_DEP_FEEDER_ACCESS_PROFILE)
    return;
  /* Only profile lines that belong to the attribution feeder set (honors the
     same trace_dir/coverage knobs as br_addr_replay_hit). */
  if (!br_addr_replay_hit(line_addr))
    return;
  Addr line = br_line_of(line_addr);
  STAT_EVENT(proc_id, DCACHE_FEEDER_ACCESS_TOTAL);
  if (hit) {
    /* A resident feeder line is by definition already seen. */
    g_feeder_seen[proc_id].insert(line);
    STAT_EVENT(proc_id, DCACHE_FEEDER_ACCESS_REUSE_HIT);
  } else {
    /* insert().second is TRUE when the line was not previously seen -> cold. */
    if (g_feeder_seen[proc_id].insert(line).second)
      STAT_EVENT(proc_id, DCACHE_FEEDER_ACCESS_COLD);
    else
      STAT_EVENT(proc_id, DCACHE_FEEDER_ACCESS_REUSE_MISS);
  }
}

/* ================= seqnum-driven replay prefetcher ==================== *
 * Reads <bench>_<simpt>_pf.csv (seqnum,line) and, on each retired op, issues
 * MRT_DPRF prefetches for recorded lines whose seqnum is within
 * BRANCH_LOAD_DEP_PF_LOOKAHEAD of the current retire pointer. Coverage gating
 * (optional) comes from a separately-located braddr.csv line-set.
 * ===================================================================== */

static Flag g_pf_replay_loaded = FALSE;
static std::vector<std::pair<Counter, Addr>> g_pf_replay_rows;  // (seqnum, line), sorted asc
static size_t g_pf_replay_cursor = 0;
static std::unordered_set<Addr> g_pf_cov_set;  // coverage line-set
static Flag g_pf_cov_gated = FALSE;            // gate prefetches by g_pf_cov_set

static void br_pf_replay_load(void) {
  g_pf_replay_loaded = TRUE;
  char path[MAX_STR_LENGTH + 1];
  br_trace_suffix_path(path, sizeof(path), BRANCH_LOAD_DEP_PF_DIR, "pf");
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "br_pf_replay: cannot open %s\n", path);
    return;
  }
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) {
    if (buf[0] == '#' || buf[0] == '\n')
      continue;
    unsigned long long seq = 0, line = 0;
    if (sscanf(buf, "%llu,0x%llx", &seq, &line) == 2)  // skips the seqnum,line header
      g_pf_replay_rows.push_back({(Counter)seq, (Addr)line});
  }
  fclose(f);
  std::sort(g_pf_replay_rows.begin(), g_pf_replay_rows.end(),
            [](const std::pair<Counter, Addr>& a, const std::pair<Counter, Addr>& b) {
              return a.first < b.first;
            });
  fprintf(stderr, "br_pf_replay: loaded %zu prefetch triggers from %s\n", g_pf_replay_rows.size(), path);
}

/* Coverage line-set: smallest prefix of braddr.csv (sorted desc by cycles)
 * covering BRANCH_LOAD_DEP_PF_COVERAGE of recorded stall cycles. 1.0 = no gate. */
static void br_pf_cov_load(void) {
  double cov = (double)BRANCH_LOAD_DEP_PF_COVERAGE;
  if (cov >= 1.0)
    return;  // keep all lines -> g_pf_cov_gated stays FALSE
  char path[MAX_STR_LENGTH + 1];
  br_trace_suffix_path(path, sizeof(path), BRANCH_LOAD_DEP_PF_COVERAGE_DIR, "braddr");
  FILE* f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "br_pf_cov: cannot open %s (coverage gating disabled)\n", path);
    return;
  }
  Counter total = 0;
  std::vector<std::pair<Addr, Counter>> lines;
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) {
    if (buf[0] == '#' || buf[0] == '\n')
      continue;
    unsigned long long line = 0, cyc = 0;
    if (sscanf(buf, "0x%llx,%llu", &line, &cyc) == 2) {
      lines.push_back({(Addr)line, (Counter)cyc});
      total += cyc;
    }
  }
  fclose(f);
  g_pf_cov_gated = TRUE;
  Counter target = (Counter)(cov * (double)total);
  Counter acc = 0;
  for (auto& kv : lines) {  // braddr.csv is written sorted desc by cycles
    g_pf_cov_set.insert(kv.first);
    acc += kv.second;
    if (acc >= target)
      break;
  }
  fprintf(stderr, "br_pf_cov: %zu lines cover %.0f%% of %llu cycles\n", g_pf_cov_set.size(), 100.0 * cov,
          (unsigned long long)total);
}

static void br_pf_issue(uns8 proc_id, Addr line_addr) {
  if (model->mem != MODEL_MEM)
    return;
  if (BRANCH_LOAD_DEP_PF_LEVEL == 0) {
    /* L1D: skip if already resident. */
    Cache* dcache = &cmp_model.dcache_stage[proc_id].dcache;
    Addr la;
    if (cache_access(dcache, line_addr, &la, FALSE)) {
      STAT_EVENT(proc_id, BLD_PF_ALREADY_PRESENT);
      return;
    }
    if (new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL, dcache_fill_line, 0, NULL))
      STAT_EVENT(proc_id, BLD_PF_ISSUED);
    else
      STAT_EVENT(proc_id, BLD_PF_DROPPED_BUFFER);
    return;
  }
  Pref_Req_Info info;
  memset(&info, 0, sizeof(info));
  uns size;
  if (BRANCH_LOAD_DEP_PF_LEVEL == 1) {
    info.dest = DEST_MLC;
    size = MLC_LINE_SIZE;
  } else {
    info.dest = DEST_L1;
    size = L1_LINE_SIZE;
  }
  if (new_mem_req(MRT_DPRF, proc_id, line_addr, size, 0, NULL, NULL, 0, &info))
    STAT_EVENT(proc_id, BLD_PF_ISSUED);
  else
    STAT_EVENT(proc_id, BLD_PF_DROPPED_BUFFER);
}

/* Retire hook: R = op_num of the op just retired (the monotonic retire pointer).
 * Fire every not-yet-issued trigger whose seqnum <= R + W. */
void br_pf_replay_on_retire(uns8 proc_id, Counter op_num) {
  if (!BRANCH_LOAD_DEP_PF_REPLAY)
    return;
  if (!g_pf_replay_loaded) {
    br_pf_replay_load();
    br_pf_cov_load();
  }
  Counter horizon = op_num + (Counter)BRANCH_LOAD_DEP_PF_LOOKAHEAD;
  while (g_pf_replay_cursor < g_pf_replay_rows.size() && g_pf_replay_rows[g_pf_replay_cursor].first <= horizon) {
    Addr line = g_pf_replay_rows[g_pf_replay_cursor].second;
    if (!g_pf_cov_gated || g_pf_cov_set.count(line))
      br_pf_issue(proc_id, line);
    else
      STAT_EVENT(proc_id, BLD_PF_SKIPPED_COVERAGE);
    g_pf_replay_cursor++;
  }
}

void br_exec_wait_cycle(uns8 proc_id, Counter cycle_count) {
  Op* tracked = NULL;
  for (Op* op = cmp_model.node_stage[proc_id].node_head; op; op = op->next_node) {
    if (!op->op_pool_valid)
      continue;
    if (br_op_in_operand_delay_window(op, cycle_count)) {
      tracked = op;
      break;
    }
  }
  if (!tracked)
    return;

  ASSERT(proc_id, !tracked->off_path);

  Counter hypo = br_hypothetical_resolve_cycle(tracked);
  if (cycle_count == hypo && tracked->bp_pred_info->mispred)
    bld_mispred_chain_classify(proc_id, tracked);

  INC_STAT_EVENT(proc_id, BR_EXEC_RESOLVE_TOTAL, 1);
  INC_STAT_EVENT(proc_id, BR_EXEC_OPERAND_WAIT, 1);

  if (tracked->bp_pred_info->mispred) {
    INC_STAT_EVENT(proc_id, BR_EXEC_RESOLVE_TOTAL_MISPRED, 1);
    INC_STAT_EVENT(proc_id, BR_EXEC_OPERAND_WAIT_MISPRED, 1);
    /* Attribute this stall cycle to the dominant bottleneck in the branch's
       not-ready dependency cone. Sums to BR_EXEC_OPERAND_WAIT_MISPRED. */
    br_charge_operand_stall(proc_id, tracked, cycle_count);
    if (BRANCH_LOAD_DEP_ATTRIBUTE_RECORD)
      br_record_delay_load(proc_id, tracked, cycle_count);
  } else {
    INC_STAT_EVENT(proc_id, BR_EXEC_RESOLVE_TOTAL_MISFETCH, 1);
    INC_STAT_EVENT(proc_id, BR_EXEC_OPERAND_WAIT_MISFETCH, 1);
  }

  if (tracked->bp_pred_info->mispred && tracked->bp_pred_info->misfetch) {
    INC_STAT_EVENT(proc_id, BR_EXEC_RESOLVE_TOTAL_BOTH, 1);
    INC_STAT_EVENT(proc_id, BR_EXEC_OPERAND_WAIT_BOTH, 1);
  }
}
