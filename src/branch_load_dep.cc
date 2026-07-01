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
 * Map batch protocol:
 *   1. bld_process_op() on each op: insert InstRecord (src_info at map time).
 *   2. bld_map_batch_branches() after the batch: walk branches so same-cycle
 *      producers are already in the record table.
 *
 * Backward walk uses live Op* edges when available and falls back to InstRecord
 * edges after producer ops retire from the op pool.  Register and memory
 * producer edges (REG_DATA_DEP, MEM_ADDR_DEP, MEM_DATA_DEP) are followed.
 *
 * Profile mode (--branch_load_dep_profile 1): unbounded InstRecords, walks
 * REG_DATA_DEP+MEM_DATA_DEP value slice from CF_CBR branches, emits BLD_PROFILE_*
 * stats for window sizing (see memory.stat.def).
 ***************************************************************************************/

#include "branch_load_dep.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "core.param.h"
#include "dcache_stage.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
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

static constexpr unsigned BLD_WINDOW_SIZE = 8192;
static constexpr unsigned BLD_PROFILE_SPAN_BINS = 21; /* log2 buckets: 1, 2-3, ..., 2^20+ */
static constexpr unsigned BLD_PROFILE_HOP_BINS  = 16; /* hops 1 .. 16+ */

struct BldReplayPair {
  uns64 branch_inst_uid;
  uns64 feeder_inst_uid;
  uns   depth;
};

struct InstRecord {
  bool is_load;
  Addr va;
  uns64 inst_uid;
  Op* op_ptr;
  Counter op_num;
  std::vector<Counter> src_writer_uids;
  std::vector<Counter> src_value_writer_uids;
  Counter uid;
};

struct BldState {
  std::unordered_map<Counter, InstRecord> records;
  std::deque<Counter> insertion_order;
};

static FILE* g_trace_out = nullptr;
static FILE* g_bld_debug_out = nullptr;
static uns g_bld_debug_remaining = 0;
static std::vector<BldState> g_bld_states;
static std::vector<std::vector<BldReplayPair>> g_replay_pairs;
static std::vector<std::unordered_map<uns64, std::vector<uns>>> g_replay_feeder_depths;

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

static void bld_evict_oldest(BldState& state) {
  if (state.insertion_order.empty())
    return;
  Counter oldest = state.insertion_order.front();
  state.insertion_order.pop_front();
  state.records.erase(oldest);
}

static void bld_insert_record(BldState& state, InstRecord rec) {
  if (!BRANCH_LOAD_DEP_PROFILE && state.records.size() >= BLD_WINDOW_SIZE)
    bld_evict_oldest(state);
  Counter uid = rec.uid;
  state.records[uid] = std::move(rec);
  state.insertion_order.push_back(uid);
}

static Flag bld_is_producer_dep(Src_Info* s) {
  return s->type == REG_DATA_DEP || s->type == MEM_ADDR_DEP || s->type == MEM_DATA_DEP;
}

static Flag bld_is_value_dep(Src_Info* s) {
  return s->type == REG_DATA_DEP || s->type == MEM_DATA_DEP;
}

static uns bld_span_log2_bucket(Counter span) {
  if (span <= 1)
    return 0;
  if (span > ((Counter)1 << 20))
    return BLD_PROFILE_SPAN_BINS - 1;
  return LOG2((uns)span);
}

static uns bld_hop_bucket(uns hop) {
  if (hop == 0)
    hop = 1;
  if (hop >= BLD_PROFILE_HOP_BINS)
    return BLD_PROFILE_HOP_BINS - 1;
  return hop - 1;
}

static void bld_stat_span_hist(uns8 proc_id, Stat_Enum base, Counter span) {
  uns bucket = bld_span_log2_bucket(span);
  STAT_EVENT(proc_id, base + bucket);
}

static void bld_stat_hop_hist(uns8 proc_id, Counter hop) {
  uns bucket = bld_hop_bucket((uns)hop);
  STAT_EVENT(proc_id, BLD_PROFILE_MAX_LOAD_HOP_BIN_1 + bucket);
}

static InstRecord bld_build_record(Op* op) {
  InstRecord rec;
  rec.uid      = op->unique_num;
  rec.op_num   = op->op_num;
  rec.is_load  = (op->inst_info->table_info.mem_type == MEM_LD);
  rec.va       = rec.is_load ? op->oracle_info.va : static_cast<Addr>(0);
  rec.inst_uid = op->inst_uid;
  rec.op_ptr   = op;

  for (uns i = 0; i < op->num_srcs; i++) {
    Src_Info* s = &op->src_info[i];
    if (bld_is_producer_dep(s))
      rec.src_writer_uids.push_back(s->unique_num);
    if (bld_is_value_dep(s))
      rec.src_value_writer_uids.push_back(s->unique_num);
  }
  return rec;
}

static void bld_replay_add_pair(uns8 proc_id, uns64 branch_inst_uid, uns64 feeder_inst_uid, uns depth) {
  g_replay_pairs[proc_id].push_back({branch_inst_uid, feeder_inst_uid, depth});
  g_replay_feeder_depths[proc_id][feeder_inst_uid].push_back(depth);
}

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
      std::getline(ss, tok, ',');
      feeder_inst_uid = (uns64)std::stoull(tok);
      std::getline(ss, tok, ',');
      depth = (uns)std::stoul(tok);
    } else {
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

  fprintf(stderr, "[BLD] Loaded %zu replay pairs from %s (%zu unique feeders on proc 0)\n",
          g_replay_pairs[0].size(), trace_path, g_replay_feeder_depths[0].size());

  if (legacy_format) {
    fprintf(stderr,
            "[BLD] Warning: legacy 3-column trace (no branch_inst_uid) in %s; "
            "re-record for per-branch depth semantics.\n",
            trace_path);
  }
}

static Op* bld_live_op(const InstRecord* rec) {
  if (!rec || !rec->op_ptr || !rec->op_ptr->op_pool_valid)
    return nullptr;
  if (rec->op_ptr->unique_num != rec->uid)
    return nullptr;
  return rec->op_ptr;
}

static void bld_mark_feeder_load(uns8 proc_id, uns64 branch_inst_uid, Op* feeder_op,
                                 const InstRecord* feeder_rec, uns depth) {
  Op* live_op = nullptr;
  if (feeder_op && feeder_op->op_pool_valid)
    live_op = feeder_op;
  else if (feeder_rec)
    live_op = bld_live_op(feeder_rec);

  Flag is_load = live_op ? (live_op->inst_info->table_info.mem_type == MEM_LD)
                         : (feeder_rec && feeder_rec->is_load);
  if (!is_load)
    return;

  uns64 feeder_inst_uid = live_op ? live_op->inst_uid : feeder_rec->inst_uid;
  Addr va = live_op ? live_op->oracle_info.va : feeder_rec->va;

  if (live_op)
    live_op->feeds_branch = TRUE;

  Cache* dcache = get_dcache_for_proc(proc_id);
  if (va != 0 && cache_set_feeds_branch(dcache, proc_id, va))
    STAT_EVENT(proc_id, DCACHE_FEEDER_LINES_MARKED);

  if (BRANCH_LOAD_DEP_TRACE_RECORD && g_trace_out)
    fprintf(g_trace_out, "%u,%" PRIu64 ",%" PRIu64 ",%u\n",
            (unsigned)proc_id, (uint64_t)branch_inst_uid,
            (uint64_t)feeder_inst_uid, (unsigned)depth);

  if (BRANCH_LOAD_DEP_PREFETCH && va != 0 && model->mem == MODEL_MEM) {
    Addr line_addr;
    if (!cache_access(dcache, va, &line_addr, FALSE))
      new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL,
                  dcache_fill_line, 0, NULL);
  }
}

using BldChainVisitor = void (*)(uns8 proc_id, Op* live_op, const InstRecord* rec,
                                 uns bfs_depth, void* ctx);

static Op* bld_src_producer_op(Src_Info* s) {
  if (s->op && s->op->op_pool_valid && s->op->unique_num == s->unique_num)
    return s->op;
  return nullptr;
}

/* BFS backward walk using live src_info->op. Seeds from not-ready operand edges when
 * seed_not_ready is set (operand-wait window at first stall cycle). */
static void bld_walk_operand_chain_live(Op* branch_op, BldChainVisitor visitor, void* ctx,
                                        Flag seed_not_ready) {
  std::unordered_set<Counter> visited;
  std::deque<std::pair<Op*, uns>> worklist;

  for (uns i = 0; i < branch_op->num_srcs; i++) {
    if (seed_not_ready && !op_sources_test_not_rdy(branch_op, i))
      continue;
    Src_Info* s = &branch_op->src_info[i];
    if (!bld_is_producer_dep(s))
      continue;
    Op* prod = bld_src_producer_op(s);
    if (!prod || visited.count(prod->unique_num))
      continue;
    worklist.push_back({prod, 1});
  }

  while (!worklist.empty()) {
    Op* cur_op = worklist.front().first;
    uns depth    = worklist.front().second;
    worklist.pop_front();

    if (!visited.insert(cur_op->unique_num).second)
      continue;

    visitor(branch_op->proc_id, cur_op, nullptr, depth, ctx);

    for (uns i = 0; i < cur_op->num_srcs; i++) {
      Src_Info* s = &cur_op->src_info[i];
      if (!bld_is_producer_dep(s))
        continue;
      Op* prod = bld_src_producer_op(s);
      if (!prod || visited.count(prod->unique_num))
        continue;
      worklist.push_back({prod, depth + 1});
    }
  }
}

/* BFS backward walk from branch operand producers (same edges as feeder recording). */
static void bld_walk_operand_chain(uns8 proc_id, Op* branch_op, BldChainVisitor visitor,
                                   void* ctx, Flag* stop_walk) {
  std::unordered_set<Counter> visited;
  std::deque<std::pair<Counter, uns>> worklist;

  BldState& state = g_bld_states[proc_id];

  for (uns i = 0; i < branch_op->num_srcs; i++) {
    Src_Info* s = &branch_op->src_info[i];
    if (!bld_is_producer_dep(s))
      continue;
    if (visited.count(s->unique_num))
      continue;
    worklist.push_back({s->unique_num, 1});
  }

  while (!worklist.empty()) {
    Counter cur_uid = worklist.front().first;
    uns depth       = worklist.front().second;
    worklist.pop_front();

    if (!visited.insert(cur_uid).second)
      continue;

    auto it = state.records.find(cur_uid);
    if (it == state.records.end())
      continue;

    const InstRecord& cur_rec = it->second;
    Op* live_op               = bld_live_op(&cur_rec);

    visitor(proc_id, live_op, &cur_rec, depth, ctx);
    if (stop_walk && *stop_walk)
      return;

    if (live_op && live_op->op_pool_valid) {
      for (uns i = 0; i < live_op->num_srcs; i++) {
        Src_Info* s = &live_op->src_info[i];
        if (!bld_is_producer_dep(s))
          continue;
        if (visited.count(s->unique_num))
          continue;
        worklist.push_back({s->unique_num, depth + 1});
      }
    } else {
      for (Counter src_uid : cur_rec.src_writer_uids) {
        if (visited.count(src_uid))
          continue;
        worklist.push_back({src_uid, depth + 1});
      }
    }
  }
}

struct BldValueChainWalkCtx {
  Op* branch_op;
  Counter max_node_span;
  Counter max_load_span;
  uns max_load_hop;
  uns load_count;
  uns node_count;
  Flag record_miss;
};

static Counter bld_op_span_from_branch(Op* branch_op, Counter producer_op_num) {
  if (producer_op_num >= branch_op->op_num)
    return 0;
  return branch_op->op_num - producer_op_num;
}

static void bld_value_chain_visit(uns8 proc_id, Op* live_op, const InstRecord* rec, uns hop,
                                  void* ctx_void) {
  (void)proc_id;
  auto* ctx = static_cast<BldValueChainWalkCtx*>(ctx_void);
  Counter producer_op_num = live_op ? live_op->op_num : (rec ? rec->op_num : 0);
  Counter span              = bld_op_span_from_branch(ctx->branch_op, producer_op_num);

  ctx->node_count++;
  if (span > ctx->max_node_span)
    ctx->max_node_span = span;

  Flag is_load = live_op ? (live_op->inst_info->table_info.mem_type == MEM_LD)
                         : (rec && rec->is_load);
  if (is_load) {
    ctx->load_count++;
    if (span > ctx->max_load_span)
      ctx->max_load_span = span;
    if (hop > ctx->max_load_hop)
      ctx->max_load_hop = hop;
  }
}

/* Backward walk on REG_DATA_DEP + MEM_DATA_DEP using InstRecords (unbounded in profile mode). */
static void bld_walk_value_chain(uns8 proc_id, Op* branch_op, BldChainVisitor visitor, void* ctx,
                                 Flag value_deps_only, Counter window_limit) {
  std::unordered_set<Counter> visited;
  std::deque<std::pair<Counter, uns>> worklist;
  BldState& state = g_bld_states[proc_id];

  for (uns i = 0; i < branch_op->num_srcs; i++) {
    Src_Info* s = &branch_op->src_info[i];
    if (value_deps_only ? !bld_is_value_dep(s) : !bld_is_producer_dep(s))
      continue;
    if (visited.count(s->unique_num))
      continue;
    worklist.push_back({s->unique_num, 1});
  }

  while (!worklist.empty()) {
    Counter cur_uid = worklist.front().first;
    uns depth       = worklist.front().second;
    worklist.pop_front();

    if (!visited.insert(cur_uid).second)
      continue;

    auto it = state.records.find(cur_uid);
    if (it == state.records.end()) {
      if (auto* walk_ctx = static_cast<BldValueChainWalkCtx*>(ctx))
        walk_ctx->record_miss = TRUE;
      continue;
    }

    const InstRecord& cur_rec = it->second;
    Counter span              = bld_op_span_from_branch(branch_op, cur_rec.op_num);
    if (window_limit != 0 && span > window_limit)
      continue;

    Op* live_op = bld_live_op(&cur_rec);
    visitor(proc_id, live_op, &cur_rec, depth, ctx);

    if (live_op && live_op->op_pool_valid) {
      for (uns i = 0; i < live_op->num_srcs; i++) {
        Src_Info* s = &live_op->src_info[i];
        if (value_deps_only ? !bld_is_value_dep(s) : !bld_is_producer_dep(s))
          continue;
        if (visited.count(s->unique_num))
          continue;
        worklist.push_back({s->unique_num, depth + 1});
      }
    } else {
      const std::vector<Counter>& parents =
          value_deps_only ? cur_rec.src_value_writer_uids : cur_rec.src_writer_uids;
      for (Counter src_uid : parents) {
        if (visited.count(src_uid))
          continue;
        worklist.push_back({src_uid, depth + 1});
      }
    }
  }
}

static void bld_profile_direct_reg_span(uns8 proc_id, Op* branch_op) {
  Counter max_span = 0;
  for (uns i = 0; i < branch_op->num_srcs; i++) {
    Src_Info* s = &branch_op->src_info[i];
    if (s->type != REG_DATA_DEP)
      continue;
    Counter span = bld_op_span_from_branch(branch_op, s->op_num);
    if (span > max_span)
      max_span = span;
  }
  if (max_span > 0)
    bld_stat_span_hist(proc_id, BLD_PROFILE_DIRECT_REG_SPAN_BIN_0, max_span);
}

static void bld_profile_cbranch(uns8 proc_id, Op* branch_op) {
  STAT_EVENT(proc_id, BLD_PROFILE_CBRANCHES);
  bld_profile_direct_reg_span(proc_id, branch_op);

  BldValueChainWalkCtx full_ctx{};
  full_ctx.branch_op = branch_op;
  bld_walk_value_chain(proc_id, branch_op, bld_value_chain_visit, &full_ctx, TRUE, 0);

  INC_STAT_EVENT(proc_id, BLD_PROFILE_VALUE_CHAIN_NODES, full_ctx.node_count);
  INC_STAT_EVENT(proc_id, BLD_PROFILE_VALUE_CHAIN_LOADS, full_ctx.load_count);

  if (full_ctx.record_miss)
    STAT_EVENT(proc_id, BLD_PROFILE_CBR_RECORD_MISS);

  if (full_ctx.load_count > 0) {
    STAT_EVENT(proc_id, BLD_PROFILE_CBR_HAS_VALUE_LOAD);
    bld_stat_span_hist(proc_id, BLD_PROFILE_MAX_LOAD_SPAN_BIN_0, full_ctx.max_load_span);
    if (full_ctx.max_load_hop > 0)
      bld_stat_hop_hist(proc_id, full_ctx.max_load_hop);
  }

  if (full_ctx.max_load_span > BLD_WINDOW_SIZE)
    STAT_EVENT(proc_id, BLD_PROFILE_CBR_LOAD_SPAN_GT_8192);
  if (full_ctx.max_node_span > BLD_WINDOW_SIZE)
    STAT_EVENT(proc_id, BLD_PROFILE_CBR_NODE_SPAN_GT_8192);

  BldValueChainWalkCtx win_ctx{};
  win_ctx.branch_op = branch_op;
  bld_walk_value_chain(proc_id, branch_op, bld_value_chain_visit, &win_ctx, TRUE, BLD_WINDOW_SIZE);

  if (win_ctx.load_count < full_ctx.load_count)
    STAT_EVENT(proc_id, BLD_PROFILE_CBR_WINDOW_LOST_LOAD);

  if (BRANCH_LOAD_DEP) {
    BldValueChainWalkCtx prod_ctx{};
    prod_ctx.branch_op = branch_op;
    bld_walk_value_chain(proc_id, branch_op, bld_value_chain_visit, &prod_ctx, FALSE,
                         BLD_WINDOW_SIZE);
    if (prod_ctx.load_count < full_ctx.load_count)
      STAT_EVENT(proc_id, BLD_PROFILE_CBR_PROD_WALK_FEWER_LOADS);
  }
}

struct BldFeederWalkCtx {
  uns8 proc_id;
  uns64 branch_inst_uid;
  uns max_loads;
  uns loads_marked;
  Flag stop;
};

static void bld_feeder_walk_visitor(uns8 proc_id, Op* live_op, const InstRecord* rec,
                                    uns depth, void* ctx_void) {
  auto* ctx = static_cast<BldFeederWalkCtx*>(ctx_void);
  (void)depth;

  if (ctx->stop || (ctx->max_loads != 0 && ctx->loads_marked >= ctx->max_loads)) {
    ctx->stop = TRUE;
    return;
  }

  Flag is_load = live_op ? (live_op->inst_info->table_info.mem_type == MEM_LD)
                         : (rec && rec->is_load);
  if (!is_load)
    return;

  bld_mark_feeder_load(ctx->proc_id, ctx->branch_inst_uid, live_op, rec, ctx->loads_marked);
  ctx->loads_marked++;
  if (ctx->max_loads != 0 && ctx->loads_marked >= ctx->max_loads)
    ctx->stop = TRUE;
}

static void bld_walk_branch_feeders(uns8 proc_id, Op* branch_op) {
  BldFeederWalkCtx ctx{proc_id, branch_op->inst_uid, BRANCH_LOAD_DEP_MAX_DEPTH, 0, FALSE};
  bld_walk_operand_chain(proc_id, branch_op, bld_feeder_walk_visitor, &ctx, &ctx.stop);
}

struct BldChainNode {
  Op* live_op;
  const InstRecord* rec;
  uns bfs_depth;
};

struct BldMispredWalkCtx {
  std::vector<BldChainNode> nodes;
};

static void bld_mispred_collect_visitor(uns8 proc_id, Op* live_op, const InstRecord* rec,
                                        uns bfs_depth, void* ctx_void) {
  (void)proc_id;
  auto* ctx = static_cast<BldMispredWalkCtx*>(ctx_void);
  ctx->nodes.push_back({live_op, rec, bfs_depth});
}

static Flag bld_chain_node_is_load(const BldChainNode& n) {
  if (n.live_op)
    return n.live_op->inst_info->table_info.mem_type == MEM_LD;
  return n.rec && n.rec->is_load;
}

static Flag bld_chain_node_dcmiss(const BldChainNode& n) {
  return n.live_op &&
         (n.live_op->oracle_info.dcmiss || n.live_op->engine_info.dcmiss);
}

static Flag bld_chain_node_still_inflight(const BldChainNode& n) {
  return n.live_op && n.live_op->done_cycle > cycle_count;
}

static Counter bld_op_exec_latency(Op* op) {
  if (!op || op->done_cycle <= op->issue_cycle)
    return 0;
  return op->done_cycle - op->issue_cycle;
}

/* Critical load: among not-ready (still in-flight) loads on the chain, pick max
 * (done_cycle - issue_cycle); BFS-depth tie-break (deeper wins). */
static const BldChainNode* bld_pick_critical_node(const std::vector<BldChainNode>& nodes,
                                                    Flag loads_only, Flag inflight_only) {
  const BldChainNode* best = nullptr;
  Counter best_lat         = 0;
  uns best_depth           = 0;

  for (const BldChainNode& n : nodes) {
    if (loads_only && !bld_chain_node_is_load(n))
      continue;
    if (!n.live_op)
      continue;
    if (inflight_only && !bld_chain_node_still_inflight(n))
      continue;

    Counter lat = bld_op_exec_latency(n.live_op);
    if (!best || lat > best_lat || (lat == best_lat && n.bfs_depth > best_depth)) {
      best       = &n;
      best_lat   = lat;
      best_depth = n.bfs_depth;
    }
  }
  return best;
}

static const char* bld_mispred_bucket_name(int bucket) {
  switch (bucket) {
    case 0:
      return "CHAIN_NO_LOAD_MISS";
    case 1:
      return "CRITICAL_NON_LOAD";
    case 2:
      return "CRITICAL_LOAD_NOT_IN_CSV";
    case 3:
      return "CRITICAL_FEEDER_HIT";
    case 4:
      return "CRITICAL_FEEDER_MISS";
    case 5:
      return "CRITICAL_NONFEEDER_MISS";
    default:
      return "UNKNOWN";
  }
}

static void bld_log_mispred_exemplar(uns8 proc_id, Op* branch_op,
                                     const std::vector<BldChainNode>& nodes, int bucket) {
  if (!g_bld_debug_out || g_bld_debug_remaining == 0)
    return;

  fprintf(g_bld_debug_out, "mispred,proc=%u,cycle=%" PRIu64 ",branch_uid=%" PRIu64 ",bucket=%s\n",
          (unsigned)proc_id, (uint64_t)cycle_count, (uint64_t)branch_op->inst_uid,
          bld_mispred_bucket_name(bucket));
  fprintf(g_bld_debug_out, "inst_uid,feeds_branch,dcmiss,dep_on_l1_miss,latency,bfs_depth\n");

  for (const BldChainNode& n : nodes) {
    if (!bld_chain_node_is_load(n) || !n.live_op)
      continue;
    Op* op = n.live_op;
    fprintf(g_bld_debug_out, "%" PRIu64 ",%d,%d,%d,%" PRIu64 ",%u\n", (uint64_t)op->inst_uid,
            (int)op->feeds_branch, (int)op->oracle_info.dcmiss,
            (int)op->engine_info.dep_on_l1_miss, (uint64_t)bld_op_exec_latency(op),
            (unsigned)n.bfs_depth);
  }
  fprintf(g_bld_debug_out, "\n");
  fflush(g_bld_debug_out);
  g_bld_debug_remaining--;
}

void bld_mispred_chain_classify(uns8 proc_id, Op* branch_op) {
  BldMispredWalkCtx walk_ctx;
  bld_walk_operand_chain_live(branch_op, bld_mispred_collect_visitor, &walk_ctx, TRUE);

  const std::vector<BldChainNode>& nodes = walk_ctx.nodes;

  Flag any_load_dcmiss = FALSE;
  for (const BldChainNode& n : nodes) {
    if (bld_chain_node_is_load(n) && bld_chain_node_dcmiss(n)) {
      any_load_dcmiss = TRUE;
      break;
    }
  }

  int bucket;
  if (!any_load_dcmiss) {
    bucket = 0;
    STAT_EVENT(proc_id, BLD_MISPRED_CHAIN_NO_LOAD_MISS);
  } else {
    const BldChainNode* critical_any =
        bld_pick_critical_node(nodes, FALSE, TRUE);
    if (!critical_any) {
      critical_any = bld_pick_critical_node(nodes, FALSE, FALSE);
    }
    if (!critical_any || !bld_chain_node_is_load(*critical_any)) {
      bucket = 1;
      STAT_EVENT(proc_id, BLD_MISPRED_CRITICAL_NON_LOAD);
    } else {
      const BldChainNode* critical_load = bld_pick_critical_node(nodes, TRUE, TRUE);
      if (!critical_load)
        critical_load = bld_pick_critical_node(nodes, TRUE, FALSE);
      Op* load_op = critical_load->live_op;
      Flag feeder = load_op->feeds_branch;
      Flag dcmiss = bld_chain_node_dcmiss(*critical_load);

      if (feeder && !dcmiss) {
        bucket = 3;
        STAT_EVENT(proc_id, BLD_MISPRED_CRITICAL_FEEDER_HIT);
      } else if (feeder && dcmiss) {
        bucket = 4;
        STAT_EVENT(proc_id, BLD_MISPRED_CRITICAL_FEEDER_MISS);
      } else if (!feeder && dcmiss) {
        bucket = 5;
        STAT_EVENT(proc_id, BLD_MISPRED_CRITICAL_NONFEEDER_MISS);
      } else {
        bucket = 2;
        STAT_EVENT(proc_id, BLD_MISPRED_CRITICAL_LOAD_NOT_IN_CSV);
      }
    }
  }

  bld_log_mispred_exemplar(proc_id, branch_op, nodes, bucket);
}

void bld_init(uns8 num_cores) {
  g_bld_states.resize(num_cores);
  g_replay_pairs.resize(num_cores);
  g_replay_feeder_depths.resize(num_cores);

  char trace_path[MAX_STR_LENGTH + 1];
  bld_trace_path(trace_path, sizeof(trace_path));

  if (BRANCH_LOAD_DEP_TRACE_RECORD) {
    g_trace_out = fopen(trace_path, "w");
    if (!g_trace_out) {
      fprintf(stderr, "[BLD] Cannot open trace file for writing: %s\n",
              trace_path);
      exit(1);
    }
    fprintf(g_trace_out, "proc_id,branch_inst_uid,feeder_inst_uid,depth\n");
    fprintf(stderr, "[BLD] Recording branch-feeder trace to %s\n", trace_path);
  }

  if (BRANCH_LOAD_DEP_TRACE_REPLAY)
    bld_load_replay_csv(trace_path, num_cores);

  if (BRANCH_LOAD_DEP_PROFILE) {
    fprintf(stderr,
            "[BLD] Profiling conditional-branch value-slice spans "
            "(unbounded InstRecords, REG+MEM_DATA deps)\n");
    fprintf(stderr,
            "[BLD]   grep BLD_PROFILE_ memory.stat.*.out after sim; "
            "span bin N covers op_num deltas [2^N, 2^(N+1)-1] (bin 0 = 1)\n");
  }

  g_bld_debug_remaining = BLD_DEBUG_MISPRED;
  if (BLD_DEBUG_MISPRED > 0) {
    g_bld_debug_out = fopen("bld_debug.csv", "w");
    if (!g_bld_debug_out) {
      fprintf(stderr, "[BLD] Cannot open bld_debug.csv for writing in cwd\n");
      exit(1);
    }
    fprintf(stderr, "[BLD] Logging up to %u mispredict exemplars to bld_debug.csv\n",
            (unsigned)BLD_DEBUG_MISPRED);
  }
}

void bld_process_op(uns8 proc_id, Op* op) {
  if (BRANCH_LOAD_DEP_TRACE_REPLAY && !op->off_path) {
    uns replay_max_depth = BRANCH_LOAD_DEP_MAX_DEPTH;
    if (bld_replay_feeder_included(proc_id, op->inst_uid, replay_max_depth)) {
      op->feeds_branch = TRUE;
      STAT_EVENT(proc_id, BLD_REPLAY_MARK_OPS);
      if (op->inst_info->table_info.mem_type == MEM_LD)
        STAT_EVENT(proc_id, BLD_REPLAY_MARK_LOADS);
    }
  }

  if (!BRANCH_LOAD_DEP && !BRANCH_LOAD_DEP_PROFILE)
    return;

  BldState& state = g_bld_states[proc_id];
  InstRecord rec = bld_build_record(op);
  bld_insert_record(state, std::move(rec));
}

void bld_map_batch_branches(uns8 proc_id, Op** ops, uns op_count) {
  if (!BRANCH_LOAD_DEP && !BRANCH_LOAD_DEP_PROFILE)
    return;

  for (uns i = 0; i < op_count; i++) {
    Op* op = ops[i];
    if (!op || op->off_path)
      continue;
    if (op->inst_info->table_info.cf_type == NOT_CF)
      continue;
    if (BRANCH_LOAD_DEP_PROFILE && op->inst_info->table_info.cf_type == CF_CBR)
      bld_profile_cbranch(proc_id, op);
    if (BRANCH_LOAD_DEP)
      bld_walk_branch_feeders(proc_id, op);
  }
}

void bld_finish(void) {
  if (g_trace_out) {
    fflush(g_trace_out);
    fclose(g_trace_out);
    g_trace_out = nullptr;
  }
  if (g_bld_debug_out) {
    fflush(g_bld_debug_out);
    fclose(g_bld_debug_out);
    g_bld_debug_out = nullptr;
  }
}
