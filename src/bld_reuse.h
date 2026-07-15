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
 * File         : bld_reuse.h
 * Description  : Reuse-distance study for branch-delaying loads. Measurement
 *                only (does not alter cache replacement). Maintains per-core
 *                shadow LRU-stack analyzers and records the true stack (reuse)
 *                distance of lines touched by branch-delaying loads, over both
 *                the delaying-load-only stream and the full demand stream.
 *                Enabled by --branch_load_dep_reuse_study.
 ***************************************************************************************/

#ifndef __BLD_REUSE_H__
#define __BLD_REUSE_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate per-core analyzer state. Called once at startup (no-op unless
   branch_load_dep_reuse_study is set). */
void bld_reuse_init(uns8 num_cores);

/* Register a load PC as branch-delaying. Called from br_exec_wait's attribution
   (br_charge_line) whenever a stall cycle is charged to a load (category A/B). */
void bld_reuse_mark_delaying_pc(uns8 proc_id, Addr pc);

/* Note one on-path demand memory access at the dcache stage. Always advances the
   full-stream shadow stack; if pc is a known delaying-load PC and is_load, also
   records the line's feeder-only and full-stream stack distances (split by the
   real-L1 outcome real_hit). */
void bld_reuse_note_access(uns8 proc_id, Addr pc, Addr line_addr, Flag is_load, Flag real_hit);

/* Write <bench>_<simpt>_reuse.csv to the trace dir and free state. Called once at
   end of simulation. */
void bld_reuse_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __BLD_REUSE_H__ */
