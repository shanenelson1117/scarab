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
#include <stdlib.h>
#include <string.h>

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
#include "memory/memory.h"

const static uns64 TOPDOWN_SCALE_FACTOR = 10000;
const static int TOPDOWN_RECOVERY_DEPTH = 2;

/* per-load memory-boundness CSV outputs (opened lazily on the first retired load) */
static FILE* td_load_pc_fp = NULL;
static FILE* td_load_addr_fp = NULL;

/* Dynamic combined-L2 depth/threshold: the GLOBAL policy dynamics bucket on the top-down
 * SLOT breakdown (Frontend Bound vs Memory Bound) -- the signal that best characterizes the
 * workload -- while each line's RRPV still uses its own cycle-level fe/mem fraction (which
 * best localizes that line's effect). We derive the top-down ratio per window by snapshotting
 * the already-accumulated component counters at each window boundary. */
static double  g_dyn_fe_frac[MAX_NUM_PROCS];   // last window's frontend/(frontend+mem) slot ratio
static Flag    g_dyn_valid[MAX_NUM_PROCS];     // has a window been computed yet?
static Counter g_dyn_win_start[MAX_NUM_PROCS]; // cycle the current window opened
/* snapshots of cumulative top-down counters at the last window boundary */
static Counter g_snap_fb[MAX_NUM_PROCS];  // TOPDOWN_FETCH_BUBBLES_SLOTS
static Counter g_snap_ts[MAX_NUM_PROCS];  // TOPDOWN_TOTAL_SLOTS
static Counter g_snap_is[MAX_NUM_PROCS];  // TOPDOWN_ISSUED_SLOTS
static Counter g_snap_rb[MAX_NUM_PROCS];  // TOPDOWN_RECOVERY_BUBBLES_SLOTS
static Counter g_snap_bs[MAX_NUM_PROCS];  // TOPDOWN_BACKEND_STALLS_CYCLES
static Counter g_snap_ml[MAX_NUM_PROCS];  // TOPDOWN_MEM_LOAD_STALLS_CYCLES
static Counter g_snap_ms[MAX_NUM_PROCS];  // TOPDOWN_MEM_STORE_STALLS_CYCLES

/* Top-down Frontend-Bound share of (Frontend + Memory) bound slots over the window, used to
 * bucket the dynamic combined-L2 depths/thresholds. Neutral (0.5) until the first window. */
double topdown_fe_bound_fraction(uns8 proc_id) {
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
  if (TD_LOAD_TRACK_ENABLE || TD_LOAD_EVICT_TRACK || TD_LOAD_RRIP_MARK || TD_COMBINED_ON_MLC ||
      TD_COMBINED_ON_L1) {
    Flag backend_stall = (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0);
    Flag mem_bound_cycle = backend_stall && (lsq_get_in_flight_load_num() > 0);
    lsq_tag_inflight_loads(mem_bound_cycle);
  }

  // td_fe_rrip_*: credit the demand icache miss the front end is blocked on. A front-end-
  // bound cycle = the machine took no ops and it is NOT a backend stall (miss on crit path).
  // Needed for the L1I (td_fe_rrip_mark), the L2 (td_fe_rrip_on_mlc), or the combined L2 policy.
  if (TD_FE_RRIP_MARK || TD_FE_RRIP_ON_MLC || TD_COMBINED_ON_MLC || TD_COMBINED_ON_L1) {
    Flag backend_stall = (count_issued == 0 && idq_stage_get_stage_data()->op_count > 0);
    Flag fe_bound_cycle = !backend_stall && (count_available == 0);
    icache_tag_inflight_miss(proc_id, fe_bound_cycle);
  }

  // dynamic combined-L2 (depth or threshold): recompute the top-down Frontend-vs-Memory-Bound
  // slot ratio from the accumulated counters each window (window 0 = cumulative, snapshots
  // stay 0 so deltas equal the running totals; recomputed periodically to stay fresh).
  if (TD_COMBINED_ON_MLC && (TD_COMBINED_DYNAMIC_DEPTH || TD_COMBINED_DYNAMIC_THRESH)) {
    uns win = TD_COMBINED_DYN_WINDOW;
    Counter period = win ? (Counter)win : 8192;
    if ((cycle_count - g_dyn_win_start[proc_id]) >= period) {
      Counter fb = GET_STAT_EVENT(proc_id, TOPDOWN_FETCH_BUBBLES_SLOTS);
      Counter ts = GET_STAT_EVENT(proc_id, TOPDOWN_TOTAL_SLOTS);
      Counter is = GET_STAT_EVENT(proc_id, TOPDOWN_ISSUED_SLOTS);
      Counter rb = GET_STAT_EVENT(proc_id, TOPDOWN_RECOVERY_BUBBLES_SLOTS);
      Counter bs = GET_STAT_EVENT(proc_id, TOPDOWN_BACKEND_STALLS_CYCLES);
      Counter ml = GET_STAT_EVENT(proc_id, TOPDOWN_MEM_LOAD_STALLS_CYCLES);
      Counter ms = GET_STAT_EVENT(proc_id, TOPDOWN_MEM_STORE_STALLS_CYCLES);
      // window deltas (cumulative: snapshots are 0 -> deltas == totals)
      double d_fb = (double)(fb - g_snap_fb[proc_id]);
      double d_ts = (double)(ts - g_snap_ts[proc_id]);
      double d_is = (double)(is - g_snap_is[proc_id]);
      double d_rb = (double)(rb - g_snap_rb[proc_id]);
      double d_bs = (double)(bs - g_snap_bs[proc_id]);
      double d_mem = (double)((ml - g_snap_ml[proc_id]) + (ms - g_snap_ms[proc_id]));
      // top-down: frontend_bound slots = FetchBubbles; backend_bound slots = Total - Frontend
      // - BadSpec - Retiring = ts - fb - is - rb; mem_bound = backend * mem_stalls/backend_stalls
      double backend_slots = d_ts - d_fb - d_is - d_rb;
      if (backend_slots < 0.0)
        backend_slots = 0.0;
      double mem_slots = (d_bs > 0.0) ? backend_slots * d_mem / d_bs : 0.0;
      double denom = d_fb + mem_slots;
      g_dyn_fe_frac[proc_id] = (denom > 0.0) ? d_fb / denom : 0.5;
      g_dyn_valid[proc_id] = TRUE;
      if (win) {  // sliding window advances the snapshot; cumulative keeps snapshots at 0
        g_snap_fb[proc_id] = fb;
        g_snap_ts[proc_id] = ts;
        g_snap_is[proc_id] = is;
        g_snap_rb[proc_id] = rb;
        g_snap_bs[proc_id] = bs;
        g_snap_ml[proc_id] = ml;
        g_snap_ms[proc_id] = ms;
      }
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

/**************************************************************************************/
/* Marked-load record / replay (--marked_load_record, --marked_load_replay).
 *
 * THE IDENTITY. A dynamic load instance is identified by its RETIRED-LOAD ORDINAL: the Nth
 * load to retire on this core. That is the only identity stable across two runs of different
 * configurations. op->unique_num is not -- it is handed out at FETCH and consumed by wrong-path
 * ops too, so it shifts with branch-predictor behaviour, which shifts with cache configuration.
 * Retired loads, by contrast, follow the trace's architectural order in every run. A separate
 * per-load counter is used rather than inst_count because one instruction can decode to
 * several uops (and in principle more than one load), so instruction index is not 1:1 here.
 *
 * RECORD RUN writes a bitmap with one bit per retired load: set iff that load's membound
 * fraction (td_mem_cycles/td_window_cycles, final once the load has completed) exceeded
 * --td_load_replay_thresh. The bitmap is indexed by ordinal, so it is ~1 bit per load rather
 * than a row per load.
 *
 * REPLAY RUN reloads the bitmap, re-derives the same ordinals, and for each retiring load whose
 * bit is set counts the L2/LLC outcome that op carried from its access (Op.td_mlc_outcome /
 * td_llc_outcome, stamped by td_stamp_op_outcome in memory.c). The population is therefore
 * FIXED BY THE BASELINE while the treatment varies -- which is the point: the membound fraction
 * is endogenous (the policy changes the very stall it is measured from), so classifying in the
 * policy run would move the population and confound the comparison. Read the resulting stats as
 * "loads that WOULD HAVE BEEN membound under the record run's configuration".
 *
 * The file is keyed by the sanitized trace path (td_load_derive_csv_path), the one per-simpoint
 * token both runs share. */

static uns8* g_mload_bits[MAX_NUM_PROCS];      /* bitmap, indexed by retired-load ordinal */
static uns64 g_mload_cap[MAX_NUM_PROCS];       /* allocated bits */
static uns64 g_mload_idx[MAX_NUM_PROCS];       /* next retired-load ordinal */
static Flag  g_mload_loaded[MAX_NUM_PROCS];    /* replay: bitmap read attempted */
static uns64 g_mload_valid[MAX_NUM_PROCS];     /* replay: bits actually present in the file */

static void mload_path(uns proc_id, char* out, size_t sz) {
  char suffix[MAX_STR_LENGTH + 1];
  snprintf(suffix, sizeof(suffix), "marked_loads_p%02u", proc_id);
  td_load_derive_csv_path(TD_LOAD_DIR, suffix, out, sz);
}

static void mload_ensure(uns proc_id, uns64 bit) {
  if (bit < g_mload_cap[proc_id])
    return;
  uns64 want = g_mload_cap[proc_id] ? g_mload_cap[proc_id] : (1ULL << 20);
  while (want <= bit)
    want <<= 1;
  uns64 old_bytes = (g_mload_cap[proc_id] + 7) / 8;
  uns64 new_bytes = (want + 7) / 8;
  g_mload_bits[proc_id] = (uns8*)realloc(g_mload_bits[proc_id], new_bytes);
  ASSERT(proc_id, g_mload_bits[proc_id]);
  memset(g_mload_bits[proc_id] + old_bytes, 0, new_bytes - old_bytes);
  g_mload_cap[proc_id] = want;
}

/* Replay: read the record run's bitmap once. A missing file is fatal rather than silently
   counting nothing -- an empty marked-load population looks exactly like "the policy helped
   every marked load", which is the worst way to be wrong. */
static void mload_load(uns proc_id) {
  if (g_mload_loaded[proc_id])
    return;
  g_mload_loaded[proc_id] = TRUE;

  char path[MAX_STR_LENGTH + 1];
  mload_path(proc_id, path, sizeof(path));
  FILE* fp = fopen(path, "rb");
  ASSERTM(proc_id, fp, "--marked_load_replay: cannot open record file '%s'. Run the baseline "
                       "first with --marked_load_record 1 and the same --td_load_dir.\n", path);
  uns64 nbits = 0;
  size_t got = fread(&nbits, sizeof(nbits), 1, fp);
  ASSERTM(proc_id, got == 1, "--marked_load_replay: '%s' is truncated (no header).\n", path);
  uns64 nbytes = (nbits + 7) / 8;
  g_mload_bits[proc_id] = (uns8*)calloc(1, nbytes ? nbytes : 1);
  ASSERT(proc_id, g_mload_bits[proc_id]);
  got = fread(g_mload_bits[proc_id], 1, nbytes, fp);
  ASSERTM(proc_id, got == nbytes, "--marked_load_replay: '%s' truncated body (%llu of %llu "
                                  "bytes).\n", path, (unsigned long long)got,
          (unsigned long long)nbytes);
  fclose(fp);
  g_mload_cap[proc_id] = nbits;
  g_mload_valid[proc_id] = nbits;
}

void marked_load_finish(void) {
  if (!MARKED_LOAD_RECORD)
    return;
  for (uns p = 0; p < NUM_CORES; p++) {
    char path[MAX_STR_LENGTH + 1];
    mload_path(p, path, sizeof(path));
    td_load_mkdir_p(TD_LOAD_DIR);
    FILE* fp = fopen(path, "wb");
    ASSERTM(p, fp, "--marked_load_record: cannot write '%s'\n", path);
    uns64 nbits = g_mload_idx[p];              /* only the ordinals actually retired */
    uns64 nbytes = (nbits + 7) / 8;
    fwrite(&nbits, sizeof(nbits), 1, fp);
    if (nbytes && g_mload_bits[p])
      fwrite(g_mload_bits[p], 1, nbytes, fp);
    fclose(fp);
  }
}

/*
 * Called at retirement of a load -- the anchor for everything retire-scoped.
 *
 * The membound record row IS written here (not at completion). Retirement is on-path only, so
 * the recorded sequence follows the trace's architectural order and is reproducible across
 * configurations; recording at completion also caught wrong-path loads, whose count varies with
 * branch prediction and so made the sequence useless for matching instances between runs.
 */
void topdown_load_retire(uns proc_id, Op* op) {
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  if (op->td_window_cycles == 0)
    return;

  // in-sim eviction tracking: distinct fills into set(l) between exceeding reuses of line l
  if (TD_LOAD_EVICT_TRACK)
    td_load_evict_note_access(op);

  // The per-load membound CSVs (PC- and address-keyed), emitted once per retiring load.
  topdown_load_record(proc_id, op);

  if (!MARKED_LOAD_RECORD && !MARKED_LOAD_REPLAY)
    return;

  // The stable identity: this load's retired-load ordinal. Advanced for EVERY retiring load in
  // both runs, so the two runs agree bit-for-bit on which ordinal is which load.
  const uns64 ord = g_mload_idx[proc_id]++;

  if (MARKED_LOAD_RECORD) {
    double frac = (double)op->td_mem_cycles / (double)op->td_window_cycles;
    if (frac > (double)TD_LOAD_REPLAY_THRESH) {
      mload_ensure(proc_id, ord);
      g_mload_bits[proc_id][ord >> 3] |= (uns8)(1u << (ord & 7));
    }
    return;
  }

  // ---- replay ----
  mload_load(proc_id);
  if (ord >= g_mload_valid[proc_id]) {
    // The replay run retired more loads than the record run did. The two runs execute the same
    // architectural stream, so this means they did not cover the same window (different
    // inst_limit / warmup, or a truncated record). Count it rather than guessing.
    STAT_EVENT(proc_id, MARKED_LOAD_BEYOND_RECORD);
    return;
  }
  if (!(g_mload_bits[proc_id][ord >> 3] & (uns8)(1u << (ord & 7))))
    return;  // this instance was not membound in the record run

  // Scope to the region of interest, the same window the membound counters use, so a
  // marked-load MPKI can be divided by Periodic_Instructions.
  if (!membound_in_roi())
    return;

  STAT_EVENT(proc_id, MARKED_LOAD_RETIRED);
  switch (op->td_dcache_outcome) {
    case TD_CACHE_HIT:  STAT_EVENT(proc_id, DCACHE_MARKED_LOAD_HIT);  break;
    case TD_CACHE_MISS: STAT_EVENT(proc_id, DCACHE_MARKED_LOAD_MISS); break;
    default:            STAT_EVENT(proc_id, DCACHE_MARKED_LOAD_NA);   break;
  }
  switch (op->td_mlc_outcome) {
    case TD_CACHE_HIT:  STAT_EVENT(proc_id, MLC_MARKED_LOAD_HIT);  break;
    case TD_CACHE_MISS: STAT_EVENT(proc_id, MLC_MARKED_LOAD_MISS); break;
    default:            STAT_EVENT(proc_id, MLC_MARKED_LOAD_NA);   break;
  }
  switch (op->td_llc_outcome) {
    case TD_CACHE_HIT:  STAT_EVENT(proc_id, L1_MARKED_LOAD_HIT);  break;
    case TD_CACHE_MISS: STAT_EVENT(proc_id, L1_MARKED_LOAD_MISS); break;
    default:            STAT_EVENT(proc_id, L1_MARKED_LOAD_NA);   break;
  }
}

/*
 * Emits one row per load to two sibling CSVs -- keyed by instruction PC and by data access
 * address -- recording the fraction of the load's dispatch->done window that was classified
 * memory-bound. Called at COMPLETION (done_cycle reached), to match the marked-RRIP policy,
 * which writes the RRPV at the fill when the load's data returns. Path-agnostic: any load that
 * returns is recorded, on- or off-path, mirroring the policy's path-agnostic RRPV write.
 * Every load is logged unconditionally; threshold filtering happens downstream at replay.
 */
void topdown_load_record(uns proc_id, Op* op) {
  if (!TD_LOAD_TRACK_ENABLE)
    return;
  if (op->td_recorded)  // emit once, whether from the completion hook or the retire fallback
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  if (op->td_window_cycles == 0)
    return;
  op->td_recorded = TRUE;

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
  marked_load_finish();   /* --marked_load_record: flush the retired-load bitmap */
}
