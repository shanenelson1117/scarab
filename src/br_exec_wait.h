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
 * File         : br_exec_wait.h
 * Description  : Per-cycle accounting of cycles an on-path mispredicted branch
 *                spends past its zero-operand resolve point while at least one
 *                source operand is still not ready and the branch has not yet
 *                reached exec_cycle.
 *
 *                At most one branch is charged per core per cycle: the oldest
 *                qualifying on-path mispredict-at-exec branch (nested younger
 *                mispredicts are ignored until the older one completes).
 ***************************************************************************************/

#ifndef __BR_EXEC_WAIT_H__
#define __BR_EXEC_WAIT_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once per core per cycle from the exec stage.  Increments
   BR_EXEC_RESOLVE_TOTAL / BR_EXEC_OPERAND_WAIT (and category splits) for the
   oldest on-path mispredicted branch that is past its zero-operand resolve
   point, still has unresolved operands, and has not yet reached exec_cycle. */
void br_exec_wait_cycle(uns8 proc_id, Counter cycle_count);

/* Flush the per-PC/per-cacheline branch-delay-load attribution (category A =
   dataflow-ancestor miss, B = window-clog miss) to branch_delay_loads.csv, and
   the flat line,cycles replay file to the trace dir. No-op unless
   branch_load_dep_attribute_record is set. Called once at end of simulation. */
void br_exec_wait_finish(void);

/* Note an on-path load miss line for compulsory-vs-warm classification
   (branch_load_dep_attribute_record). Called from the dcache miss path. */
void br_note_load_miss(Addr line_addr);

/* TRUE if this data cacheline is in the addr-replay force-hit set
   (branch_load_dep_addr_replay). Lazily loads the recorded line file. */
Flag br_addr_replay_hit(Addr line_addr);

/* Seqnum-driven replay prefetcher: called once per retired op with its op_num
 * (the retire pointer). Issues MRT_DPRF prefetches for recorded delaying loads
 * within BRANCH_LOAD_DEP_PF_LOOKAHEAD of that pointer. No-op unless enabled. */
void br_pf_replay_on_retire(uns8 proc_id, Counter op_num);

/* True if a prefetch fill of this line should mark it as a branch feeder (marking
 * enabled and the line is a prefetch target). The caller then calls
 * cache_set_feeds_branch on the L1D or MLC cache per branch_load_dep_pf_repl_level. */
Flag br_pf_should_mark(Addr line_addr);

/* Feeder-access stream profiler (branch_load_dep_feeder_access_profile). Call on
   every on-path demand-load access to a data cacheline; hit=TRUE for an L1D hit,
   FALSE for a miss. Only accesses to lines in the attribution feeder set are
   counted, split into cold / reuse-hit / reuse-miss (see the
   DCACHE_FEEDER_ACCESS_* stats). Measurement-only; does not affect replacement. */
void feeder_access_profile_note(uns8 proc_id, Addr line_addr, Flag hit);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __BR_EXEC_WAIT_H__ */
