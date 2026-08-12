/***************************************************************************************
 * File         : td_load_replay.h
 * Description  :
 *    Replay side of the per-load memory-boundness feature. Loads a recorded CSV
 *    (produced by the topdown per-load record pass) into a key->ratio map and
 *    decides, per load, whether its access should be forced to L1-hit latency.
 ***************************************************************************************/

#ifndef __TD_LOAD_REPLAY_H__
#define __TD_LOAD_REPLAY_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "op.h"

/* Derive a per-simpoint CSV path in a single flat directory:
 *   <dir>/<sanitized CBP_TRACE_R0>_<suffix>.csv
 * The full trace path is the only per-simpoint-unique key shared by the record and replay
 * runs, so sanitizing it (non-alphanumeric -> '_') yields collision-free names that both
 * sides reproduce identically. Falls back to <dir>/<suffix>.csv when no trace path is set. */
void td_load_derive_csv_path(const char* dir, const char* suffix, char* out, size_t sz);

/* Create dir (and any missing parents), like `mkdir -p`. Existing dirs are fine. */
void td_load_mkdir_p(const char* dir);

/* Lazily load the recorded CSV (pc- or addr-keyed per TD_LOAD_REPLAY_KEY) into a
 * key->mean-ratio map. Idempotent -- safe to call every access. */
void td_load_replay_init(void);

/* TRUE iff this load's key (PC or data va, per TD_LOAD_REPLAY_KEY) is present in the
 * recorded map with a mean memory-bound ratio strictly above TD_LOAD_REPLAY_THRESH. */
Flag td_load_should_force_l1_hit(Op* op);

/* TRUE iff `key` (a PC or data address, matching TD_LOAD_REPLAY_KEY) is present in the
 * recorded averaged map with mean memory-bound ratio > TD_LOAD_REPLAY_THRESH. Used by the
 * marked-RRIP dcache policy to protect memory-bound lines at insertion. */
Flag td_load_is_marked_key(uns64 key);

/* td_load_evict_track mode (line/address-keyed, self-contained). Measures, between two
 * consecutive exceeding accesses to the same L1D line l, the number of DISTINCT lines
 * filled into set(l) (S) vs. the associativity (W); eviction <=> S >= W. */

/* Feed the per-set shadow LRU on every L1D fill. */
void td_load_evict_note_fill(Addr fill_addr);

/* At a retiring exceeding load: emit (line,S,W) if the line had a prior exceeding access,
 * then re-pin to open the next interval. Writes <td_load_dir>/<trace>_line_evict_p<NN>.csv. */
void td_load_evict_note_access(Op* op);

/* Close the eviction log at end of simulation. */
void td_load_evict_finish(void);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __TD_LOAD_REPLAY_H__ */
