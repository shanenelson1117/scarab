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
 * File         : branch_load_dep.h
 * Description  : Marks loads whose values feed branch instructions (through ALU
 *                chains).  Records (branch_inst_uid, feeder_inst_uid, depth) pairs
 *                for trace replay with oracle L1 hit latency on feeder loads.
 ***************************************************************************************/

#ifndef __BRANCH_LOAD_DEP_H__
#define __BRANCH_LOAD_DEP_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once at simulator init with the number of cores. */
void bld_init(uns8 num_cores);

/* Called for every op that exits the decode stage. */
void bld_process_op(uns8 proc_id, Op* op);

/* Called at simulation end to flush and close any open trace file. */
void bld_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __BRANCH_LOAD_DEP_H__ */
