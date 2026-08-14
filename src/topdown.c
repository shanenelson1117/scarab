/*
 * Copyright (c) 2025 University of California, Santa Cruz
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
 * File         : topdown.c
 * Author       : Yinyuan Zhao, Litz Lab
 * Date         : 05/2025
 *    Implements the Top-Down performance analysis methodology based on:
 *      Yasin, A. "A Top-Down Method for Performance Analysis and Counters Architecture,"
 *      2014 IEEE International Symposium on Performance Analysis of Systems and Software.
 ***************************************************************************************/

#include "topdown.h"

#include <stdio.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "dcache_stage.h"
#include "icache_stage.h"
#include "idq_stage.h"
#include "lsq.h"
#include "map_stage.h"
#include "node_stage.h"
#include "op.h"
#include "td_load_replay.h"

const static uns64 TOPDOWN_SCALE_FACTOR = 10000;
const static int TOPDOWN_RECOVERY_DEPTH = 2;

/* per-load memory-boundness CSV outputs (opened lazily on the first retired load) */
static FILE* td_load_pc_fp = NULL;
static FILE* td_load_addr_fp = NULL;

/* Dynamic combined-depth: per-proc front-end-bound vs mem-bound accounting over a window. */
static Counter g_dyn_fe_cur[MAX_NUM_PROCS];    // fe-bound cycles this window
static Counter g_dyn_mem_cur[MAX_NUM_PROCS];   // mem-bound cycles this window
static double  g_dyn_fe_frac[MAX_NUM_PROCS];   // last completed window's fe/(fe+mem)
static Flag    g_dyn_valid[MAX_NUM_PROCS];     // has a window completed yet?
static Counter g_dyn_win_start[MAX_NUM_PROCS]; // cycle the current window opened

/* Front-end-bound fraction of "bound" cycles (fe/(fe+mem)) used to bucket the dynamic
 * combined-L2 depths. Neutral (0.5) until the first window completes. */
double topdown_fe_bound_fraction(uns8 proc_id) {
  if (TD_COMBINED_DYN_WINDOW == 0) {  // cumulative: compute live from the running counts
    Counter tot = g_dyn_fe_cur[proc_id] + g_dyn_mem_cur[proc_id];
    return tot ? (double)g_dyn_fe_cur[proc_id] / (double)tot : 0.5;
  }
  return g_dyn_valid[proc_id] ? g_dyn_fe_frac[proc_id] : 0.5;
}

/**************************************************************************************/
/* Events Update */

/* ==================================================================================
 * Event Definitions
 * ----------------------------------------------------------------------------------
 * TotalSlots*            = Total number of issue-pipeline slots.
 * SlotsIssued*           = Utilized issue-pipeline slots to issue operations.
 * SlotsRetired*          = Utilized issue-pipeline slots to retire (complete) operations.
 * FetchBubbles           = Unutilized issue-pipeline slots while there is no backend-stall.
 * RecoveryBubbles        = Unutilized issue-pipeline slots due to recovery from earlier miss-speculation.
 * BrMispredRetired*      = Retired miss-predicted branch instructions.
 * MachineClears*         = Machine clear events (pipeline is flushed).
 * MsSlotsRetired*        = Retired pipeline slots supplied by the micro-sequencer fetch-unit.
 * OpsExecuted*           = Number of operations executed in a cycle.
 * MemStalls.AnyLoad      = Cycles with no uops executed and at least one in-flight load not completed.
 * MemStalls.L1miss       = Cycles with no uops executed and at least one in-flight load that missed the L1-cache.
 * MemStalls.L2miss       = Cycles with no uops executed and at least one in-flight load that missed the L2-cache.
 * MemStalls.L3miss       = Cycles with no uops executed and at least one in-flight load that missed the L3-cache.
 * MemStalls.Stores       = Cycles with few uops executed and no more stores can be issued.
 * ExtMemOutstanding      = Number of outstanding requests to the memory controller every cycle.
 *
 * -----------------------------------------------------------------------------------
 * [Defined in https://github.com/andikleen/pmu-tools/blob/master/skl_client_ratios.py]
 *
 * BackendStalls = CoreStalls + (∑OpsExecuted[= FEW]) + StoreStalls
 * =================================================================================== */

void topdown_bp_recovery(uns proc_id, Op* op) {
  ASSERT(op->proc_id, op->inst_info->table_info.cf_type);

  STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (op->bp_pred_info->recover_at_exec) {
    ASSERT(op->proc_id, !op->off_path);
    STAT_EVENT(proc_id, TOPDOWN_BR_MISPRED_RETIRED_CYCLES);
  }

  idq_stage_set_recovery_cycle(TOPDOWN_RECOVERY_DEPTH);
}

void topdown_idq_update(uns proc_id, int count_available, int count_issued, int count_issued_on_path) {
  // per-load memory-boundness tracking: classify this cycle and tag every in-flight load's window
  // (needed by the record pass, the in-sim gap-tracking mode, and the on-demand marked-RRIP
  // policy which reads td_mem_cycles/td_window_cycles at fill time -- no record CSV)
  if (TD_LOAD_TRACK_ENABLE || TD_LOAD_EVICT_TRACK || TD_LOAD_RRIP_MARK || TD_COMBINED_ON_MLC) {
    Flag backend_stall = (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0);
    Flag mem_bound_cycle = backend_stall && (lsq_get_in_flight_load_num() > 0);
    lsq_tag_inflight_loads(mem_bound_cycle);
  }

  // td_fe_rrip_*: credit the demand icache miss the front end is blocked on. A front-end-
  // bound cycle = the machine took no ops and it is NOT a backend stall (miss on crit path).
  // Needed for the L1I (td_fe_rrip_mark), the L2 (td_fe_rrip_on_mlc), or the combined L2 policy.
  if (TD_FE_RRIP_MARK || TD_FE_RRIP_ON_MLC || TD_COMBINED_ON_MLC) {
    Flag backend_stall = (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0);
    Flag fe_bound_cycle = !backend_stall && (count_available == 0);
    icache_tag_inflight_miss(proc_id, fe_bound_cycle);
  }

  // dynamic combined-L2 (depth or threshold): accumulate the fe-vs-mem balance, snapshot/window
  if (TD_COMBINED_ON_MLC && (TD_COMBINED_DYNAMIC_DEPTH || TD_COMBINED_DYNAMIC_THRESH)) {
    Flag backend_stall = (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0);
    if (!backend_stall && count_available == 0)
      g_dyn_fe_cur[proc_id]++;  // front-end-bound cycle
    else if (backend_stall && lsq_get_in_flight_load_num() > 0)
      g_dyn_mem_cur[proc_id]++;  // memory-bound cycle
    uns win = TD_COMBINED_DYN_WINDOW;
    if (win > 0 && (cycle_count - g_dyn_win_start[proc_id]) >= (Counter)win) {
      Counter tot = g_dyn_fe_cur[proc_id] + g_dyn_mem_cur[proc_id];
      g_dyn_fe_frac[proc_id] = tot ? (double)g_dyn_fe_cur[proc_id] / (double)tot : 0.5;
      g_dyn_valid[proc_id] = TRUE;
      g_dyn_fe_cur[proc_id] = 0;
      g_dyn_mem_cur[proc_id] = 0;
      g_dyn_win_start[proc_id] = cycle_count;
    }
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS, ISSUE_WIDTH);
  INC_STAT_EVENT(proc_id, TOPDOWN_ISSUED_SLOTS, count_issued);
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS, count_issued_on_path);

  int recovery_cycle = idq_stage_get_recovery_cycle();
  if (recovery_cycle != 0) {
    ASSERT(proc_id, recovery_cycle > 0);
    idq_stage_set_recovery_cycle(recovery_cycle - 1);
    INC_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES_SLOTS, ISSUE_WIDTH - count_available);
    return;
  }

  // only increment frontend-stall when there is no backend-stall
  if (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0) {
    STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
    if (lsq_get_in_flight_load_num() > 0) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES);
    } else if (!lsq_available(MEM_ST)) {
      STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES);
    }
    return;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS, ISSUE_WIDTH - count_available);
  if (count_available == 0)
    STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES);
}

void topdown_exec_update(uns proc_id, uns8 fus_busy) {
  if (fus_busy <= TOPDOWN_FU_EXEC_FEW && node->node_count != 0) {
    STAT_EVENT(proc_id, TOPDOWN_EXEC_STALLS_CYCLES);
  }
}

/*
 * Called at retirement of a load. Emits one row per retired (on-path) load to two
 * sibling CSVs -- keyed by instruction PC and by data access address -- recording the
 * fraction of the load's dispatch->done window that was classified memory-bound.
 * Every load is logged unconditionally; threshold filtering happens downstream at replay.
 */
void topdown_load_retire(uns proc_id, Op* op) {
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  if (op->td_window_cycles == 0)
    return;

  // in-sim eviction tracking: distinct fills into set(l) between exceeding reuses of line l
  if (TD_LOAD_EVICT_TRACK)
    td_load_evict_note_access(op);

  if (!TD_LOAD_TRACK_ENABLE)
    return;

  double frac = (double)op->td_mem_cycles / (double)op->td_window_cycles;

  char path[MAX_STR_LENGTH + 1];

  if (!td_load_pc_fp) {
    td_load_mkdir_p(TD_LOAD_DIR);
    td_load_derive_csv_path(TD_LOAD_DIR, "mem_bound_loads_pc", path, sizeof(path));
    td_load_pc_fp = fopen(path, "w");
    ASSERT(proc_id, td_load_pc_fp);
    fprintf(td_load_pc_fp, "pc,mem_bound_fraction\n");
  }
  fprintf(td_load_pc_fp, "0x%llx,%f\n", (unsigned long long)op->inst_info->addr, frac);

  if (!td_load_addr_fp) {
    td_load_derive_csv_path(TD_LOAD_DIR, "mem_bound_loads_addr", path, sizeof(path));
    td_load_addr_fp = fopen(path, "w");
    ASSERT(proc_id, td_load_addr_fp);
    fprintf(td_load_addr_fp, "addr,mem_bound_fraction\n");
  }
  fprintf(td_load_addr_fp, "0x%llx,%f\n", (unsigned long long)op->oracle_info.va, frac);
}

/**************************************************************************************/
/*
 * Metrics Update
 *  At the end of simulation, the events can be directly used to calculate the
 *  metrics using the following formulas.
 */

/*
 * Top-Down Metrics Formulas
 *
 * Frontend Bound       = FetchBubbles / TotalSlots
 * Bad Speculation      = (SlotsIssued - SlotsRetired + RecoveryBubbles) / TotalSlots
 * Retiring             = SlotsRetired / TotalSlots
 * Backend Bound        = 1 - (Frontend Bound + Bad Speculation + Retiring)
 *
 * Fetch Latency Bound  = FetchBubbles[≥ #MIW] / Clocks
 * Fetch Bandwidth Bound = Frontend Bound - Fetch Latency Bound
 *
 * #BrMispredFraction   = BrMispredRetired / (BrMispredRetired + MachineClears)
 * Branch Mispredicts   = #BrMispredFraction * Bad Speculation
 * Machine Clears       = Bad Speculation - Branch Mispredicts
 *
 * MicroSequencer       = MsSlotsRetired / TotalSlots
 * BASE                 = Retiring - MicroSequencer
 *
 * #ExecutionStalls     = (∑OpsExecuted[= FEW]) / Clocks
 *
 * Memory Bound         = (MemStalls.AnyLoad + MemStalls.Stores) / Clocks
 * Core Bound           = #ExecutionStalls - Memory Bound
 *
 * L1 Bound             = (MemStalls.AnyLoad - MemStalls.L1miss) / Clocks
 * L2 Bound             = (MemStalls.L1miss - MemStalls.L2miss) / Clocks
 * L3 Bound             = (MemStalls.L2miss - MemStalls.L3miss) / Clocks
 * Ext. Memory Bound    = MemStalls.L3miss / Clocks
 *
 * MEM Bandwidth        = ExtMemOutstanding[≥ THRESHOLD] / ExtMemOutstanding[≥ 1]
 * MEM Latency          = (ExtMemOutstanding[≥ 1] / Clocks) - MEM Bandwidth
 */

void topdown_done(uns proc_id) {
  /* Top-Level Breakdown */
  uns64 frontend_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_FRONTEND_BOUND, frontend_bound);

  uns64 bad_spec_slots = GET_STAT_EVENT(proc_id, TOPDOWN_ISSUED_SLOTS) -
                         GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) +
                         GET_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES_SLOTS);
  uns64 bad_spec_bound = bad_spec_slots * TOPDOWN_SCALE_FACTOR / GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_BAD_SPEC_BOUND, bad_spec_bound);

  uns64 retiring_bound = GET_STAT_EVENT(proc_id, TOPDOWN_RETIRED_SLOTS) * TOPDOWN_SCALE_FACTOR /
                         GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
  INC_STAT_EVENT(proc_id, TOPDOWN_RETIRING_BOUND, retiring_bound);

  uns64 backend_bound = TOPDOWN_SCALE_FACTOR - frontend_bound - bad_spec_bound - retiring_bound;
  INC_STAT_EVENT(proc_id, TOPDOWN_BACKEND_BOUND, backend_bound);

  /* Backend Breakdown */
  // prevent division by zero
  uns64 backend_stalls_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
  if (backend_stalls_cycles == 0) {
    backend_stalls_cycles++;
  }

  uns64 mem_stalls_cycles = (GET_STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES) +
                             GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES));
  uns64 mem_bound = backend_bound * mem_stalls_cycles / backend_stalls_cycles;
  INC_STAT_EVENT(proc_id, TOPDOWN_MEM_BOUND, mem_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_CORE_BOUND, backend_bound - mem_bound);

  /* Retiring Breakdown */
  // TODO: need more metadata from the simulation frontend to determine if an operand requires MicroSequencer

  /* Front-End Breakdown */
  ASSERT(proc_id, GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS) != 0);
  uns64 latency_bound = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_GT_MIW_CYCLES) * TOPDOWN_SCALE_FACTOR /
                        GET_STAT_EVENT(proc_id, NODE_CYCLE);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_LATENCY_BOUND, latency_bound);
  INC_STAT_EVENT(proc_id, TOPDOWN_FETCH_BANDWIDTH_BOUND, frontend_bound - latency_bound);

  /* Bad Spec Breakdown */
  // prevent division by zero
  uns64 bad_spec_cycles = GET_STAT_EVENT(proc_id, TOPDOWN_BR_MISPRED_RETIRED_CYCLES) +
                          GET_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEAR_CYCLES);
  if (bad_spec_cycles == 0) {
    bad_spec_cycles++;
  }

  INC_STAT_EVENT(proc_id, TOPDOWN_BR_MISPREDICTS_BOUND,
                 bad_spec_bound * GET_STAT_EVENT(proc_id, TOPDOWN_BR_MISPRED_RETIRED_CYCLES) / bad_spec_cycles);
  INC_STAT_EVENT(proc_id, TOPDOWN_MACHINE_CLEARS_BOUND,
                 bad_spec_bound - GET_STAT_EVENT(proc_id, TOPDOWN_BR_MISPREDICTS_BOUND));

  /* Flush per-load memory-boundness CSVs */
  if (td_load_pc_fp) {
    fclose(td_load_pc_fp);
    td_load_pc_fp = NULL;
  }
  if (td_load_addr_fp) {
    fclose(td_load_addr_fp);
    td_load_addr_fp = NULL;
  }

  /* Close the in-sim eviction-tracking log (td_load_evict_track mode) */
  td_load_evict_finish();
}
