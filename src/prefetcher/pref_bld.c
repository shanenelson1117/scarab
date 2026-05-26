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
 * File         : prefetcher/pref_bld.c
 * Description  : Branch-load-dep trace-replay prefetcher.
 ***************************************************************************************/

#include "prefetcher/pref_bld.h"

#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "core.param.h"
#include "memory/memory.param.h"

static HWP_Info* g_bld_hwp_info = NULL;

void pref_bld_init(HWP* hwp) {
  hwp->hwp_info->enabled = BRANCH_LOAD_DEP_TRACE_REPLAY;
  g_bld_hwp_info         = hwp->hwp_info;
}

Flag pref_bld_send(uns8 proc_id, Addr va) {
  if (!g_bld_hwp_info || !va)
    return FALSE;
  return pref_addto_dl0req_queue(proc_id, va >> LOG2(DCACHE_LINE_SIZE),
                                 g_bld_hwp_info->id);
}
