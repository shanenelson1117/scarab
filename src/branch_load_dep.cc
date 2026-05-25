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
 * Description  : Inline def-use analysis identifying loads whose values transitively
 *                feed branch instructions through ALU chains (not load-to-load).
 *
 * Algorithm:
 *   Maintain per-proc:
 *     reg_map   : logical reg id → unique_num of last writer
 *     records   : unique_num → InstRecord (is_load, va, op_ptr, src_writer_uids)
 *
 *   On each decoded op:
 *     1. Build an InstRecord from the op's logical src/dst registers.
 *     2. Update reg_map with this op's dest regs.
 *     3. If the op is a branch, walk backward through records:
 *          - Skip already-visited nodes.
 *          - If a predecessor is a load → mark it (set op->feeds_branch and call
 *            cache_set_feeds_branch for the case where the line is already in cache).
 *          - If a predecessor is an ALU op → continue traversal.
 *          - Loads act as traversal boundaries (no load-to-load chaining).
 *
 *   Record lifetime: a sliding window of BLD_WINDOW_SIZE entries per proc keeps
 *   memory bounded; old records are evicted before they can form stale Op* pointers
 *   because the window comfortably covers any realistic load-to-branch distance.
 ***************************************************************************************/

#include "branch_load_dep.h"

#include <deque>
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
#include "statistics.h"
#include "table_info.h"
}

/* Maximum number of InstRecords retained per core.  A value of 4096 covers
 * any realistic load-to-branch ALU chain while bounding memory use. */
static constexpr unsigned BLD_WINDOW_SIZE = 4096;

struct InstRecord {
  bool is_load;
  Addr va;          /* virtual address (meaningful only when is_load) */
  Op*  op_ptr;      /* raw pointer valid while op is in the ROB */
  std::vector<Counter> src_writer_uids;
  Counter uid;      /* copy for ordered eviction */
};

struct BldState {
  /* reg_id (flattened logical reg) → unique_num of last writer seen */
  std::unordered_map<uns16, Counter> reg_map;

  /* unique_num → record; bounded to BLD_WINDOW_SIZE entries */
  std::unordered_map<Counter, InstRecord> records;

  /* UIDs in insertion order for O(1) eviction of the oldest entry */
  std::deque<Counter> insertion_order;
};

static std::vector<BldState> g_states;

void bld_init(uns8 num_cores) {
  g_states.resize(num_cores);
}

/* Evict the oldest record from records to keep the map within BLD_WINDOW_SIZE.
 * O(1) amortized: insertion_order tracks UIDs in FIFO order. */
static void evict_oldest(BldState& s) {
  if (s.insertion_order.empty())
    return;
  Counter oldest = s.insertion_order.front();
  s.insertion_order.pop_front();
  s.records.erase(oldest);
}

void bld_process_op(uns8 proc_id, Op* op) {
  if (!BRANCH_LOAD_DEP)
    return;

  /* Skip off-path ops; their records could leave stale pointers after recovery. */
  if (op->off_path)
    return;

  BldState& state = g_states[proc_id];

  bool is_load   = (op->inst_info->table_info.mem_type == MEM_LD);
  bool is_branch = (op->inst_info->table_info.cf_type  != NOT_CF);

  /* Build InstRecord for this op. */
  InstRecord rec;
  rec.uid     = op->unique_num;
  rec.is_load = is_load;
  rec.va      = is_load ? op->oracle_info.va : static_cast<Addr>(0);
  rec.op_ptr  = op;

  /* Collect unique_nums of ops that wrote this op's source regs. */
  uns num_srcs = op->inst_info->table_info.num_src_regs;
  for (uns i = 0; i < num_srcs; i++) {
    uns16 reg_id = op->inst_info->srcs[i].id;
    if (reg_id == OP_REG_ID_INVALID)
      continue;
    auto it = state.reg_map.find(reg_id);
    if (it != state.reg_map.end())
      rec.src_writer_uids.push_back(it->second);
  }

  /* Insert into records, evicting the oldest if we've hit the window limit. */
  if (state.records.size() >= BLD_WINDOW_SIZE)
    evict_oldest(state);
  state.records[op->unique_num] = std::move(rec);
  state.insertion_order.push_back(op->unique_num);

  /* Update reg_map: this op becomes the last writer for each of its dest regs. */
  uns num_dests = op->inst_info->table_info.num_dest_regs;
  for (uns i = 0; i < num_dests; i++) {
    uns16 reg_id = op->inst_info->dests[i].id;
    if (reg_id == OP_REG_ID_INVALID)
      continue;
    state.reg_map[reg_id] = op->unique_num;
  }

  if (!is_branch)
    return;

  /* === Branch: traverse backward to find feeding loads ===
   * Each worklist entry carries the uid and how many loads have been crossed
   * to reach it.  When BRANCH_LOAD_DEP_CROSS_LOAD is set, the DFS continues
   * through a load's sources (treating the load as a passthrough) up to one
   * load boundary deep.  Without the flag the original behaviour is preserved:
   * loads are marked but not traversed. */
  std::unordered_set<Counter> visited;
  std::vector<std::pair<Counter, int>> worklist;  /* (uid, loads_crossed) */

  const InstRecord& branch_rec = state.records[op->unique_num];
  for (Counter uid : branch_rec.src_writer_uids)
    worklist.push_back({uid, 0});

  while (!worklist.empty()) {
    auto [uid, loads_crossed] = worklist.back();
    worklist.pop_back();

    if (!visited.insert(uid).second)
      continue;

    auto it = state.records.find(uid);
    if (it == state.records.end())
      continue;

    InstRecord& dep = it->second;

    if (dep.is_load) {
      dep.op_ptr->feeds_branch = TRUE;
      if (dep.va != 0 && cache_set_feeds_branch(&dc->dcache, proc_id, dep.va))
        STAT_EVENT(proc_id, DCACHE_FEEDER_LINES_MARKED);

      if (BRANCH_LOAD_DEP_PREFETCH && dep.va != 0 && model->mem == MODEL_MEM) {
        Addr line_addr;
        if (!cache_access(&dc->dcache, dep.va, &line_addr, FALSE))
          new_mem_req(MRT_DPRF, proc_id, line_addr, DCACHE_LINE_SIZE, 0, NULL,
                      dcache_fill_line, 0, NULL);
      }

      if (BRANCH_LOAD_DEP_CROSS_LOAD && loads_crossed < 1) {
        for (Counter src_uid : dep.src_writer_uids)
          worklist.push_back({src_uid, loads_crossed + 1});
      }
    } else {
      for (Counter src_uid : dep.src_writer_uids)
        worklist.push_back({src_uid, loads_crossed});
    }
  }
}
