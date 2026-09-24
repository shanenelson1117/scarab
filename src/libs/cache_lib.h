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
 * File         : libs/cache_lib.h
 * Author       : HPS Research Group
 * Date         : 2/6/1998
 * Description  : Header for libs/cache_lib.c
 ***************************************************************************************/

#ifndef __CACHE_LIB_H__
#define __CACHE_LIB_H__

#include "globals/global_defs.h"

#include "libs/list_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/

/* set data pointers to this initially */
#define INIT_CACHE_DATA_VALUE ((void*)0x8badbeef)

/**************************************************************************************/

typedef enum Repl_Policy_enum {
  REPL_TRUE_LRU,              /* actual least-recently-used replacement */
  REPL_RANDOM,                /* random replacement */
  REPL_NOT_MRU,               /* not-most-recently-used replacement */
  REPL_ROUND_ROBIN,           /* round-robin replacement */
  REPL_IDEAL,                 /* ideal replacement */
  REPL_ISO_PREF,              /* lru with some entries (isolated misses) higher priority */
  REPL_LOW_PREF,              /* prefetched data have lower priority */
  REPL_SHADOW_IDEAL,          /* ideal replacement with shadow cache */
  REPL_IDEAL_STORAGE,         /* if the data doesn't have a temporal locality then it
                                 isn't stored at the cache */
  REPL_MLP,                   /* MLP-aware LIN (Qureshi et al. ISCA'06): evict min of
                                 Recency + lambda*cost. Knobs are --mlp_lin_* in
                                 memory.param.def; needs --mlp_cost_stats 1 or every line
                                 carries cost 0 and it degenerates to true LRU. */
  REPL_PARTITION,             /* Based on the partition*/
  REPL_RESTEER,               /* Prioritize the instr following a resteered branch or fetch barrier */
  REPL_STICKY_PRIORITY_LINES, /* Prioritize lines tagged with priority bit. */

  REPL_VOID,    /* void policy for loop ending and policy seperation */
  REPL_LRU_REF, /* least-recently-used replacement */
  REPL_NRU,     /* not recently used replacement */
  REPL_SRRIP,   /* static re-reference interval prediction */
  REPL_BRRIP,   /* bimodal re-reference interval prediction */
  REPL_DRRIP,   /* dynamic re-reference interval prediction */
  REPL_SHIP,    /* signature-based hit predictor */
  REPL_MARKED_RRIP, /* SRRIP variant: marked (memory-bound) lines insert at RRPV 0 */
  REPL_PLRU_TREE,   /* tree-based pseudo-LRU (binary tree of direction bits per set) */
  REPL_MOCKINGJAY,  /* Mockingjay (HPCA'22): PC-signature reuse-distance prediction + ETR */
  REPL_MLP_PAPER,   /* MLP-aware LIN exactly as published (Qureshi et al. ISCA'06): Recency +
                       lambda*costq and NOTHING else -- no boundness terms, no prefetch term,
                       wrong-path handled by confirm-and-retract. Optional SBAR (two ATDs, PSEL
                       on disagreement). Knobs are --mlp_paper_* in memory.param.def. Distinct
                       from REPL_MLP, which is the extended version. */

  NUM_REPL
} Repl_Policy;

typedef enum Cache_Repl_Signiture_enum {
  CACHE_REPL_SIGH_PC,
  CACHE_REPL_SIGH_MEM,
  CACHE_REPL_SIGH_NUM
} Cache_Repl_Signiture;

typedef struct Cache_Entry_struct {
  uns8 proc_id;
  Flag valid;               /* valid bit for the line */
  Addr tag;                 /* tag for the line */
  Addr base;                /* address of first element */
  Counter last_access_time; /* for replacement policy */
  Counter insertion_time;   /* for replacement policy */
  void* data;               /* pointer to arbitrary data */
  Flag pref;                /* extra replacement info */
  Flag dirty;               /* Dirty bit should have been here, however this is used only in warmup now */
  Addr pw_start_addr;       /* for uop cache: start addr of prediction window */

  int  reference_val; /* for re-reference replacement policy (signed: REPL_MARKED_RRIP
                         may insert marked memory-bound lines at a negative RRPV) */
  int  marked_promote_rrpv; /* REPL_MARKED_RRIP: RRPV this line was inserted at; the hit
                               handler promotes to min(0, this) so aging can't erase the
                               membound protection */
  Flag marked_protected;    /* REPL_MARKED_RRIP: was this line MARKED at insert (its fraction
                               cleared the class threshold), as opposed to inserted at basic?
                               Sticky for the line's lifetime -- marked_promote_rrpv is
                               rewritten on every hit, so it cannot answer this after the
                               first reuse. Read by the set-duel "protected hits" metric. */
  /* --membound_stats: what the ACCESS THAT BROUGHT THIS LINE IN looked like, recorded at fill
     and sticky for the line's lifetime. Independent of the replacement policy, so these hold
     under SRRIP / tPLRU / Mockingjay exactly as under REPL_MARKED_RRIP -- unlike
     marked_protected, which is only written by marked_rrip_update_insert and additionally
     folds in where `basic` sat for that set.
     A line is classified from the fill-time fraction, because that is the only point the
     signal exists: td_mem_cycles / td_window_cycles accumulates over the load's dispatch->done
     window (lsq_tag_inflight_loads), so at MISS time the window has barely started and the
     fraction is meaningless. Data and instruction lines are classified on separate gates and
     are mutually exclusive -- a line is one or the other or neither. */
  Flag membound_fill;  /* data line, demanding load's membound fraction > TD_LOAD_REPLAY_THRESH */
  Flag fe_bound_fill;  /* instruction line, fetch miss's FE-bound fraction > TD_FE_RRIP_THRESH */

  /* GRADED versions of the two signals above, for a cost-scaled replacement policy. The Flags
     say only whether a fraction cleared its gate; these say how far, which is what a policy
     that WEIGHTS a line by its cost needs rather than one that merely marks it.

     bound_frac is the raw fraction in [0,1] -- membound for a data line, FE-bound for an
     instruction line. One field because the two are mutually exclusive (see the Flags above);
     read membound_fill / fe_bound_fill to know which signal it came from. mlp_cost is the
     MLP-based cost in cycles of the miss that filled this line, mirrored from
     Mem_Req.mlp_cost.

     Both are stored RAW, not quantized. Real hardware would keep a few bits (the paper uses 3
     for cost); keeping full precision here lets the quantization be swept as a policy
     parameter without re-running the simulation to regenerate line state. Both are 0 for
     lines with no signal -- prefetch and writeback fills, and store fills, which have no
     demanding load. A cost-scaled policy MUST decide explicitly what a 0 means, or it will
     evict every prefetched line first. */
  double bound_frac;
  double mlp_cost;

  /* --mlp_lin_pref_lambda: was this line installed by a PREFETCH request? Staged at fill like
     the bound flags rather than reusing Cache_Entry.pref, which is only written by
     cache_insert_lru_replpos and is therefore stale on the plain cache_insert path the MLC
     uses. Deliberately separate so the existing prefetch accounting keeps its meaning.

     On PARAMS.google every prefetch is an FDIP instruction prefetch -- the data prefetchers are
     off (--pref_framework_on 0, --pref_stream_on 0) -- so on those traces this flag means
     "FDIP instruction line". */
  Flag fill_was_prefetch;

  /* --early_evict_stats: cycle_count at the fill that installed THIS line. Subtracted from
     cycle_count when the line is replaced to give its residency, which is what the early-
     eviction counters threshold. Stamped by every insert path, whether or not the stat is
     enabled -- it is one store on a line that is already being written, and making it
     conditional would mean a line filled with the knob off could later be measured against a
     stale stamp.

     Deliberately cycle_count and NOT sim_time: the other timestamps in this struct
     (last_access_time, insertion_time) are sim_time, which freq.c defines in FEMTOSECONDS, so
     a sim_time delta is larger than the cycle count by a factor of the domain's cycle time.
     The thresholds here are expressed in cycles (a multiple of the cache's own access latency
     in cycles), so the stamp has to be in cycles too. */
  Counter fill_cycle;

  Flag outcome;       /* for replacement policy */
} Cache_Entry;

// DO NOT CHANGE THIS ORDER
typedef enum Cache_Insert_Repl_enum {
  INSERT_REPL_DEFAULT = 0, /* Insert with default replacement information */
  INSERT_REPL_LRU,         /* Insert into LRU position */
  INSERT_REPL_LOWQTR,      /* Insert such that it is Quarter(Roughly) of the repl order*/
  INSERT_REPL_MID,         /* Insert such that it is Middle(Roughly) of the repl order*/
  INSERT_REPL_MRU,         /* Insert into MRU position */
  NUM_INSERT_REPL
} Cache_Insert_Repl;

typedef struct Cache_struct {
  char name[MAX_STR_LENGTH + 1]; /* name to identify the cache (for debugging) */
  uns data_size;                 /* how big are the data items in each cache entry? (for malloc) */

  uns assoc;               /* associativity */
  uns num_lines;           /* number of lines in the cache */
  uns num_sets;            /* number of sets in the cache */
  uns line_size;           /* size in bytes of one line */
  Repl_Policy repl_policy; /* the replacement policy of the cache */

  uns set_bits;     /* number of bits used in the set mask */
  uns shift_bits;   /* number of bits to shift an address before using (assuming it is shifted) */
  Addr set_mask;    /* mask applied after shifting to get the index */
  Addr tag_mask;    /* mask used to get the tag after shifting */
  Addr offset_mask; /* mask used to get the line offset */

  uns* repl_ctrs; /* replacement info */

  uns64* plru_tree; /* REPL_PLRU_TREE: one per set; packed direction bits of the pseudo-LRU
                       binary tree over the ways (internal node n in [1, assoc-1] -> bit n) */

  /* A dynamically allocated array of all of the cache entries. The array is two-dimensional, sets are row major. */
  Cache_Entry** entries;

  /* A linked list for each set in the cache that is used when simulating ideal replacement policies */
  List* unsure_lists;

  Flag perfect;                 /* is the cache perfect (for henry mem system) */
  uns repl_pref_thresh;         /* threshhold for how many entries are high-priority. */
  Cache_Entry** shadow_entries; /* A dynamically allocated array for shadow cache */
  uns* queue_end;               /* queue pointer for ideal storage */

  Counter num_demand_access;
  Counter last_update; /* last update cycle */

  uns* num_ways_allocted_core; /* For cache partitioning */
  uns* num_ways_occupied_core; /* For cache partitioning */
  uns* lru_index_core;         /* For cache partitioning */
  Counter* lru_time_core;      /* For cache partitioning */

  Flag tag_incl_offset; /* The uop cache is byte-addressable, so the tag includes offset bits as well */

  /* For DRRIP repl */
  uns* dedicated_policy_set; /* For dedicated set map */
  Counter* miss_count;       /* For sampling */
  Counter bimodal_count;

  /* For repl with predictor */
  void* predictor;

  /* REPL_MARKED_RRIP, --marked_rrip_age_period > 1: one aging counter per set. The argmax
     eviction path bumps RRPVs only when this rolls over, so a set ages once every N real
     evictions instead of on every one. NULL for every other policy (and for period 1). */
  uns* marked_age_ctr;

  /* --marked_rrip_stream_buf: single-entry victim buffer holding the line the bypass filter
     most recently declined to allocate. Probed in parallel with the data array on every
     access; a hit is served in place and touches NO set's replacement state (the line is not
     in a set). `sb.data` is a full data_size payload, so the caller's dirty bit and the
     existing writeback path work on it unchanged.

     The buffer is shared by every set, so a tag alone does not identify a line -- two
     addresses in different sets can carry the same tag. Every compare is on the line address
     (`base`) as well.

     sb_last_hit records whether the most recent cache_access on THIS cache was served by the
     buffer rather than the array; it is cleared at the top of every access, so a caller reads
     it right after the access it belongs to (see cache_stream_buf_last_hit). */
  Flag sb_enabled;
  Flag sb_last_hit;
  Cache_Entry sb;

  /* --membound_stats: did the most recent cache_access on THIS cache hit a line that had been
     brought in by a membound (resp. FE-bound) access? Cleared at the top of every access, so a
     caller reads it immediately after the access it describes. Same publish-one-shot idea as
     cache_marked_last_hit_protected, but per-cache rather than global, so the LLC and MLC
     cannot clobber each other. cache_lib.c keeps no stats of its own; memory.c counts. */
  Flag last_hit_membound;
  Flag last_hit_fe_bound;

  /* --early_evict_stats: residency of the line the most recent INSERT on THIS cache evicted.
     last_evict_valid is FALSE when that insert took a free way and so evicted nothing; when it
     is TRUE, last_evict_age is cycle_count - the victim's fill_cycle, in cycles.

     Published rather than counted here for the same reason as last_hit_membound: cache_lib.c
     keeps no stats and knows no cache's access latency, so the owner (memory.c, dcache_stage.c,
     icache_stage.c) reads this right after its cache_insert returns and applies its own
     threshold. Written by every insert path, so a reader always sees the insert it just made
     and never a stale value from an earlier one. Per-cache, so the LLC and MLC cannot clobber
     each other. */
  Flag    last_evict_valid;
  Counter last_evict_age;

  /* REPL_MLP: the MLP-based cost carried by the line the most recent cache_access on THIS cache
     HIT, in cycles; 0 on a miss or on a line that never had one. Same publish-one-shot idea as
     last_hit_membound above, and it exists for the same reason: cache_access returns
     entry->data, not the Cache_Entry, so a caller with only the data pointer cannot reach
     mlp_cost.

     Read by the SBAR selector. When an ATD misses on an address the REAL cache still holds,
     no new cost is measured for that access, so the charge is this line's stored cost -- what
     that exact address cost the last time it was actually fetched. */
  double last_hit_mlp_cost;
  double last_hit_bound_frac;

  /* REPL_MLP lambdas for THIS cache. When lin_lambda_override is FALSE the value function uses
     the global --mlp_lin_* params, which is the plain static-LIN configuration. The SBAR
     selector sets it TRUE on every cache it owns: each ATD gets its candidate triple and holds
     it for the run, while the real MLC gets whichever triple is currently selected and has it
     rewritten at each window boundary.

     Per-cache rather than global so the ATDs and the MTD can run DIFFERENT lambdas through the
     SAME find_repl_entry code -- that is what makes the ATDs a faithful shadow of the policy
     rather than a reimplementation of it. */
  Flag   lin_lambda_override;
  double lin_lambda_mlp;
  double lin_lambda_data;
  double lin_lambda_instr;
} Cache;

/**************************************************************************************/
/* Strategy Design */
struct repl_policy_func {
  Repl_Policy repl_policy_type;

  void (*action_init)(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
  void (*action_repl)(Cache*, Cache_Entry*, uns8, Addr, Addr*, Addr*);

  void (*update_hit)(Cache*, uns, uns, void*);
  void (*update_insert)(Cache*, uns8, uns, uns, void*);
  Cache_Entry* (*update_evict)(Cache*, uns8, uns, uns*, void*, Flag);
};

/* Driven Table */
extern struct repl_policy_func repl_policy_func_table[NUM_REPL];

/* Strategy Function */
void init_cache_strategy(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_insert_strategy(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr);
void* cache_access_strategy(Cache* cache, Addr addr, Addr* line_addr, Flag update_repl);
Cache_Entry* cache_evict_strategy(Cache* cache, uns8 proc_id, uns set, uns* way);

const static Flag CACHE_DEBUG_ENABLE = FALSE;  // To be Changed into DEBUG_PARA

/**************************************************************************************/
/* prototypes */

void init_cache(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_access(Cache*, Addr, Addr*, Flag);
void* cache_insert(Cache*, uns8, Addr, Addr*, Addr*);
/* REPL_MARKED_RRIP: set right before a cache_insert to control the inserted line's RRPV.
 * have_rrpv==TRUE inserts the line at the caller-supplied `rrpv` (each cache computes it from
 * its own signal + knob set via marked_rrip_rrpv_from_frac); FALSE inserts at the normal
 * SRRIP distant value. One-shot: consumed and cleared by the next marked_rrip insert. */
void cache_set_marked_next_insert(Flag have_rrpv, int rrpv);
/* Map a fraction (membound, front-end-bound, ...) to an initial RRPV using an explicit knob
 * set. Callers precompute the RRPV so REPL_MARKED_RRIP stays signal-agnostic. */
int marked_rrip_rrpv_from_frac(double f, int min_rrpv, Flag extrapolate, double anchor, double thresh);
/* The "basic" (unprotected) initial RRPV for REPL_MARKED_RRIP: --marked_rrip_basic_rrpv,
 * clamped to RRIP_DISTANT_VAL (above that a line can never match the eviction test). Callers
 * that need to name the unprotected depth themselves must use this rather than a literal. */
int marked_rrip_basic_rrpv(void);
/* As marked_rrip_rrpv_from_frac but with the unprotected ("basic") end supplied explicitly.
 * Set dueling selects basic PER SET, so it cannot come from the global param. */
int marked_rrip_rrpv_from_frac_basic(double f, int min_rrpv, Flag extrapolate, double anchor, double thresh,
                                     int basic_rrpv);
/* One-shot per-set basic RRPV for the next marked_rrip insert, staged like the RRPV itself.
 * Governs the unstaged fallback (prefetch / off-path fills). have_basic==FALSE restores the
 * global --marked_rrip_basic_rrpv. Consumed by the next marked_rrip insert. */
void cache_set_marked_next_basic(Flag have_basic, int basic);
/* TRUE if the line reused by the most recent REPL_MARKED_RRIP hit had been MARKED at insert
 * (its fraction cleared the class threshold) rather than inserted at basic. Only meaningful
 * immediately after a cache_access that hit; gate on the hit before reading it. */
Flag cache_marked_last_hit_protected(void);
/* REPL_MARKED_RRIP hit predictor (--td_load_rrip_hit_predict): stage the accessing load's
 * PC-predicted membound fraction so the next hit re-derives its RRPV from it. have==FALSE
 * (or never called) keeps the legacy min(0, inserted RRPV) hit behavior. One-shot: consumed
 * by the next marked_rrip hit. */
void cache_set_hit_promote_frac(Flag have, double frac);

/* --membound_stats: stage how the NEXT fill into any cache should be classified, one-shot, in
 * the same style as cache_set_marked_next_insert. The caller computes it (only memory.c can --
 * the membound / FE-bound fractions live on the op and the icache stage), and the next
 * cache_insert stamps it onto the line it allocates. Consumed and cleared by that insert, so a
 * fill that never happens (bypass, or an early FAILURE return) cannot leak its classification
 * onto an unrelated later fill. Both FALSE = the line is neither, which is the default. */
void cache_set_next_fill_bound(Flag membound, Flag fe_bound);
/* Graded companion to cache_set_next_fill_bound, staged at the same point and consumed by the
   same insert paths. Separate entry point rather than more arguments on that one so the
   marked-RRIP callers that only need the Flags are untouched. */
void cache_set_next_fill_cost(double bound_frac, double mlp_cost);
/* TRUE if the most recent cache_access on `cache` hit a line that had been brought in by a
 * membound (resp. FE-bound) access. Valid only immediately after that access. */
Flag cache_last_hit_membound(Cache* cache);
/* REPL_MLP / SBAR: cost carried by the line the last cache_access on `cache` hit. */
double cache_last_hit_mlp_cost(Cache* cache);
double cache_last_hit_bound_frac(Cache* cache);
/* Save / restore the one-shot fill staging. Used by the SBAR ATDs so their inserts cannot
   steal the classification a pending real fill staged -- see cache_get_fill_stage. */
void cache_get_fill_stage(Flag* membound, Flag* fe_bound, double* bound_frac, double* mlp_cost);
void cache_put_fill_stage(Flag membound, Flag fe_bound, double bound_frac, double mlp_cost);
/* --mlp_lin_pref_lambda: stage / read the prefetch flag for the next fill. */
void cache_set_next_fill_prefetch(Flag is_prefetch);
Flag cache_get_fill_prefetch(void);
/* REPL_MLP / SBAR: pin this cache's LIN lambdas (ATD candidate, or the MTD's selection). */
void cache_set_lin_lambdas(Cache* cache, double lam_mlp, double lam_data, double lam_instr);
Flag cache_last_hit_fe_bound(Cache* cache);

/* --early_evict_stats: how long the line evicted by the most recent INSERT on `cache` had been
 * resident, in CYCLES (cycle_count at eviction minus cycle_count at its fill).
 *
 * Returns FALSE, leaving *age untouched, when that insert evicted nothing -- it found a free
 * way, or the policy declined to allocate. Returns TRUE and writes the residency otherwise.
 * Valid only immediately after the cache_insert / cache_insert_replpos / cache_insert_lru it
 * describes; call it before any other insert on the same cache. The caller compares *age
 * against its own threshold, because cache_lib knows no cache's access latency. */
Flag cache_last_evict_age(Cache* cache, Counter* age);

/* --td_load_rrip_fixup: rewrite a resident line's RRPV after the fact.
 *
 * Finds the line for `addr` in its set and, ONLY IF it is still the same fill -- its
 * Cache_Entry.fill_cycle equals `fill_cycle` -- overwrites reference_val with `new_rrpv`.
 * The generation check is what makes this safe to call late: a line evicted and refilled at
 * the same address carries a different fill_cycle and is left untouched, so a stale correction
 * can never land on an unrelated line that happens to share the address.
 *
 * Touches ONLY the replacement value (plus marked_promote_rrpv, so a later hit promotes to the
 * corrected home level rather than the provisional one, and marked_protected, recomputed as
 * new_rrpv < basic_rrpv for the protected-hits metric). It does NOT count as an access: no
 * update_hit, no LRU timestamp, no aging, no stream-buffer probe. The line's position in the
 * set changes only because its RRPV did.
 *
 * Returns TRUE if a line was corrected, FALSE if it was already evicted or had been refilled. */
Flag cache_rrpv_fixup(Cache* cache, Addr addr, Counter fill_cycle, int new_rrpv, int basic_rrpv);

/* REPL_MARKED_RRIP allocation filter (--marked_rrip_bypass). TRUE when a fill arriving at
 * `insert_rrpv` would be strictly more distant than every resident line in its set -- i.e. it
 * would be the very next victim, so caching it can only evict something more useful. Only
 * meaningful for an UNPROTECTED fill; callers must not offer a marked line. Scarab's
 * update_evict must name a way, so the caller checks this BEFORE cache_insert and skips the
 * fill entirely. Pure -- it reads state but does not change it. FALSE for any cache not
 * running REPL_MARKED_RRIP, whenever --marked_rrip_bypass is off, and whenever the set has an
 * invalid way (a free way is always taken, matching the Mockingjay rule). Writebacks must
 * never be bypassed -- that is the caller's check, as it is for Mockingjay.
 * Unlike Mockingjay there is NO state update for a bypassed fill: marked-RRIP has no sampler
 * to train, the set is untouched, and in particular it does not age. */
Flag cache_marked_should_bypass(Cache* cache, Addr addr, int insert_rrpv);
/* Peek the one-line stream buffer's current occupant so the caller can drain it before it is
 * displaced. Returns its data payload (NULL when the buffer is empty or disabled) and, via
 * out-params, whether it is valid plus the line address to write back and the proc that owns
 * it. cache_lib cannot read the payload's dirty bit, so the caller does that. Pure. */
void* cache_stream_buf_peek(Cache* cache, Flag* valid, Addr* line_addr, uns8* proc_id);
/* Install `addr` as the stream buffer's occupant, discarding the previous one. The caller MUST
 * have drained a dirty previous occupant first (see cache_stream_buf_peek). Returns the new
 * occupant's data pointer, zeroed, for the caller to fill in as it would a normal fill; NULL
 * if the buffer is disabled, in which case the fill is simply dropped. */
void* cache_stream_buf_install(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr);
/* TRUE if the most recent cache_access on `cache` was served by the stream buffer rather than
 * the data array. Cleared at the top of every access, so read it immediately after the one it
 * describes. Exists because cache_lib.c keeps no stats of its own. */
Flag cache_stream_buf_last_hit(Cache* cache);

/* REPL_MOCKINGJAY: the strategy dispatcher passes no per-access context, so the accessing
 * PC / traffic class is staged one-shot right before the cache_access or cache_insert that
 * the policy should see it on (same convention as cache_set_marked_next_insert). It is
 * consumed and cleared by the next mockingjay hit/insert/bypass update. have_ctx==FALSE
 * makes the next update behave as a PC-less demand access (signature of PC 0). */
void cache_set_mockingjay_next_access(Flag have_ctx, Addr pc, Flag is_prefetch, Flag is_writeback, uns8 proc_id);
/* REPL_MOCKINGJAY bypass predicate: TRUE when Mockingjay would decline to allocate this fill
 * (its predicted reuse distance is longer than every resident line's remaining time). Scarab's
 * update_evict must name a way, so the caller checks this BEFORE cache_insert and skips the
 * fill entirely. Pure -- it reads state but does not change it. Returns FALSE for any cache
 * not running REPL_MOCKINGJAY, and whenever the set has an invalid way (an available way is
 * always taken, matching the reference implementation). Writebacks must never be bypassed. */
Flag cache_mockingjay_should_bypass(Cache* cache, Addr addr, Addr pc, Flag is_prefetch, uns8 proc_id);
/* REPL_MOCKINGJAY: run the replacement-state update for a fill the caller bypassed. The
 * reference policy still trains the sampler and ages the set on a bypassed fill; only the
 * per-line ETR write is skipped. Call with the same staged context as the skipped insert. */
void cache_mockingjay_note_bypass(Cache* cache, Addr addr);
void* cache_insert_replpos(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr,
                           Cache_Insert_Repl insert_repl_policy, Flag isPrefetch);
void* cache_insert_lru(Cache*, uns8, Addr, Addr*, Addr*);
void cache_invalidate(Cache*, Addr, Addr*);
void cache_flush(Cache*);
void* get_next_repl_line(Cache*, uns8, Addr, Addr*, Flag*);
void* get_next_valid_repl_line(Cache* cache, uns8 proc_id, Addr addr);
uns ext_cache_index(Cache*, Addr, Addr*, Addr*);
Addr get_cache_line_addr(Cache*, Addr);
uns cache_get_invalid_line_count(Cache* cache, Addr addr);
void update_repl_resteer_policy(Cache*, Addr);

void* shadow_cache_insert(Cache* cache, uns set, Addr tag, Addr base);
void* access_shadow_lines(Cache* cache, uns set, Addr tag);
void* access_ideal_storage(Cache* cache, uns set, Addr tag, Addr addr);
void reset_cache(Cache*);
int cache_find_pos_in_lru_stack(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr);
void set_partition_allocate(Cache* cache, uns8 proc_id, uns num_ways);
uns get_partition_allocated(Cache* cache, uns8 proc_id);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __CACHE_LIB_H__ */
