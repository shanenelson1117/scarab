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
 * File         : node_stage.h
 * Author       : HPS Research Group
 * Date         : 1/28/1999
 * Description  :
 ***************************************************************************************/

#ifndef __NODE_STAGE_H__
#define __NODE_STAGE_H__

#include "node_issue_queue.h"
#include "stage_data.h"

/**************************************************************************************/
// Types

typedef struct Node_Stage_struct {
  uns proc_id;
  Stage_Data sd;  // stage interface data

  Op* node_head;       // linked-list of ops in the node stage
  Op* node_tail;       // linked-list of ops in the node stage
  Op* node_precommit;  // the pre-commit pointer in the ROB
  int32 node_count;    // number of ops in the node table

  Flag prev_op_fusable;  // if the next dispatched op is macro-fusable

  /* linked-list of ops that are ready to schedule. Ops are put in here when they are issued,
   * or after they are issued and another op wakes them up. */
  Op* rdy_head;

  /* Length of the rdy_head list, maintained O(1) at every in_rdy_list transition so the count is
     available without walking. BOTH entry paths into the list (node_issue_queue_fill_rs and the
     wake-up path in cmp_model.c) are gated on op_sources_not_rdy_is_clear(op), i.e. an op joins
     exactly when its operands become available. That is what makes
        sum(rs[i].rs_op_count) - rdy_count  ==  ops in the scheduling window WAITING ON OPERANDS
     an exact identity rather than an approximation -- see node_ops_waiting_on_operands(). */
  int32 rdy_count;

  Counter ret_op;                // next op number to retire
  Counter last_scheduled_opnum;  // op num of the last scheduled op

  Op* next_op_into_rs;      // oldest issued op not yet in the scheduling window (RS)
  Reservation_Station* rs;  // information about all of the reservation stations

  int32* fu_to_rs_map;  // mapping from FU ID to RS ID (-1 for unconnected FUs)

  Flag mem_blocked;      // are we out of mem req buffers for this core
  uns mem_block_length;  // length of the current memory block
  uns ret_stall_length;  // length of the current retirement stall

  /* The most recent node_retire() pass: the cycle it ran, and how many ops it retired in that
     pass. Both are written at the TOP of node_retire -- before its node-table-empty early
     return -- so a stamp equal to cycle_count always means "retirement ran this cycle and
     last_ret_count is its final answer". Read through node_retired_this_cycle(). */
  Counter last_ret_cycle;
  uns     last_ret_count;
} Node_Stage;

/**************************************************************************************/
// External Variables

extern Node_Stage* node;

/**************************************************************************************/
// Prototypes

void set_node_stage(Node_Stage*);
void init_node_stage(uns8, const char*);
void reset_node_stage(void);
void reset_all_ops_node_stage(void);
void recover_node_stage(void);
void debug_node_stage(void);
void update_node_stage(Stage_Data*);
Flag is_node_stage_stalled(void);
/* TRUE when the scheduling window holds at least one op whose operands are not yet ready.
 * Exact, by the rdy_count invariant above; O(NUM_RS), no list walk. */
Flag node_ops_waiting_on_operands(void);

/* --memproxy_stats: "the issue window is full", in the two senses Scarab distinguishes. They are
 * NOT the same predicate and give different numbers, so both are exported and named for what
 * they test rather than for the colloquial phrase.
 *
 *   is_node_table_full()        the REORDER BUFFER is at NODE_TABLE_SIZE. This is the classic
 *                               memory-bound signature: a long-latency load blocks retirement,
 *                               the ROB backs up behind it, and dispatch stalls. Already defined
 *                               in node_stage.c; exported here so topdown.c can read it.
 *   node_issue_window_full()    every RESERVATION STATION is at capacity, i.e. the SCHEDULING
 *                               window cannot accept another op. An RS with size 0 is infinite
 *                               and makes this permanently FALSE. */
Flag is_node_table_full(void);
Flag node_issue_window_full(void);

/* TRUE when retirement COMMITTED at least one op this cycle.
 *
 * Counts UOPS, not eom instructions: any retired op is forward progress at the ROB head, so a
 * cycle that drains only the non-final uops of a multi-uop instruction is NOT a no-commit cycle.
 * That makes this strictly stronger than "no x86 instruction committed". Every retired op is
 * on-path -- node_retire asserts !op->off_path -- so there is no path qualifier to apply.
 *
 * Reads the cycle STAMP and not the count alone: node_retire is reached only through
 * update_node_stage, which a core can skip in a given cycle (frequency domain not ready, core
 * finished), and without the stamp such a cycle would report the previous pass's count forever.
 *
 * Answers for the CURRENT cycle only, and only for callers that run AFTER update_node_stage in
 * the back-to-front stage order of cmp_cores() -- update_idq_stage and update_icache_stage do.
 * A caller ahead of the node stage would read last cycle's retirement. */
Flag node_retired_this_cycle(void);

/**************************************************************************************/

#endif /* #ifndef __NODE_STAGE_H__ */
