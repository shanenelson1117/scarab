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

  /* --mlp_lin_store_lambda: does this line carry WRITE traffic? Set when the fill was caused by
     a store (MRT_DSTORE) or was itself a writeback (MRT_WB), and set later by
     cache_mark_written() when a writeback hits an already-resident line -- which is how an MLC
     line actually becomes dirty here, the L1D absorbing stores and writing back.

     NOT Cache_Entry.dirty: that field is documented "used only in warmup now" and is never
     maintained for the MLC. The real dirty state lives in L1_Data.dirty, behind the opaque data
     pointer that cache_lib cannot read -- hence a bit of its own. */
  Flag was_written;

  /* --mlp_lin_offpath_lambda: this line was installed by a WRONG-PATH request AND has not yet
     been vindicated by an on-path demand hit.
     NOT named fill_was_offpath, because it is NOT immutable fill history: cache_clear_offpath()
     clears it the first time an on-path demand access hits the line. Scarab never squashes an
     off-path request -- it completes and fills -- so wrong-path prefetching is real here, and a
     line that a real access has since needed has PROVEN its worth. Penalising it after that
     would demote exactly the speculative fills that turned out to be useful.
     Set from the oracle req->off_path, matching L1_Data.fetched_by_offpath. The
     confirm-and-retract signal is unusable at fill time: a miss whose branch has not resolved
     is not yet known to be off-path. */
  Flag offpath_unproven;

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

  /* --reuse_dist_stats: the value of Cache.set_access_ctr[set] at this line's LAST ACCESS --
     its fill, or its most recent hit. The next hit subtracts it to get a reuse distance
     measured in ACCESSES TO THIS SET, which is the unit the replacement literature uses and is
     what makes the number comparable against associativity (a distance below assoc is one LRU
     would have caught). Deliberately NOT fill_cycle, which is cycles and runs from fill: that
     measures residency, so a line hit ten times then replaced still reads as short-lived.
     Here every hit restarts the clock, so what is measured is the gap between consecutive uses.

     reuse_seen records whether this line was EVER hit since its fill. Without it the histogram
     is a biased sample -- it can only contain lines that were reused at all -- and "marked
     lines are reused at distance 40" would hide "and 70% of them are never reused". Both are
     maintained on every policy, like membound_fill, so the measurement is policy-independent
     and a marked-RRIP run can be compared against an LRU one. */
  Counter last_access_count;
  Flag reuse_seen;

  /* --load_prio_stats: the line's PRIORITY LEVEL, stamped at fill and STICKY for its lifetime.
     An index into Cache.prio_boost[], which holds the distinct values of the REPL_MLP boost

         boost = lin_lam_mlp * mlp_lin_costq(mlp_cost) + mlp_lin_bound_term(line)

     sorted ascending. Only those TWO terms: the pref / store / offpath penalties in
     find_repl_entry are deliberately NOT included, which is what makes this level sticky at all.
     was_written is set later by cache_mark_written() and offpath_unproven is cleared later by
     cache_clear_offpath(), so a level built from them would silently drift from the value stamped
     here; the two terms kept are both fully determined at fill and never rewritten.

     Computed from the line AFTER consume_fill_bound has written mlp_cost / bound_frac /
     membound_fill / fe_bound_fill onto it, so it reads exactly the state the policy will later
     evict by -- the boost is not recomputed from the staging globals, which would diverge the
     moment a staging field is added and the copy here is forgotten.

     Meaningful ONLY on a cache running REPL_MLP; prio_boost[] is left unbuilt (prio_num_levels
     0) on every other policy and this field stays 0. See --load_prio_stats. */
  uns8 prio_level;

  /* --load_prio_stats: which traffic class the fill that installed this line belonged to, one of
     Load_Prio_Traffic. Staged by the caller (only memory.c knows the Mem_Req type) and consumed
     at fill like the bound flags, so it is fill history and never rewritten.

     A field of its own rather than derived from the existing bits, because none of them answer
     it: membound_fill / fe_bound_fill are the MARKED classes and are both FALSE for an
     unclassified line of either kind, fill_was_prefetch does not distinguish an instruction
     prefetch from a data one, and was_written is mutable. */
  uns8 fill_traffic;

  /* --load_prio_stats: the value of Cache.set_miss_ctr[set] at this line's FILL, and at its LAST
     ACCESS, respectively. Subtracting the first from the set's current miss count at eviction
     gives the line's RESIDENCY in misses to its set; subtracting the second at a hit gives its
     REUSE DISTANCE in misses to its set.

     MISSES, not accesses -- which is what makes these different from last_access_count above and
     why both counters exist. A miss to the set is the event that can actually displace a line
     (every miss allocates, barring a bypass), so a residency of N misses at a 16-way set says the
     line survived N allocation opportunities. Hits to the set do not tick: they cost the line
     nothing. --reuse_dist_stats measures the other unit, accesses, and the two must not be mixed.

     reuse_count is the number of hits this line has taken since its fill. The existing
     reuse_seen Flag answers only "ever", which cannot separate a line hit once from one hit forty
     times -- and the per-level mean reuse count is the whole point of the chain here. */
  Counter fill_miss_count;
  Counter last_access_miss_count;
  Counter reuse_count;

  Flag outcome;       /* for replacement policy */
} Cache_Entry;

/* --load_prio_stats: the traffic class of a fill, for the set-composition histograms.
 *
 * These six classes are DISJOINT and together they PARTITION Mem_Req_Type, so the per-class counts
 * over a set sum to its valid ways. That is the point: a line carries exactly one class, so the
 * stored classes have to partition, and any COARSER view (all instructions, all prefetch, all
 * data) has to be computed by the set walk while it still has every way's class in hand.
 *
 * It cannot be recovered afterwards. The chains are histograms of per-fill occupancy counts, and
 * histograms do not add: COMP_IFETCH_3 and COMP_IPREF_2 are separate events and say nothing about a
 * set holding 5 instruction lines of either kind. Hence the four ALL_* chains in memory.stat.def --
 * ALL_INSTR, ALL_PREF, ALL_READ and ALL_DATA_NONLOAD -- which the walk emits alongside these; see
 * load_prio_snapshot_set. ALL_READ is DEMAND reads only (IFETCH + LOAD), so it is not the
 * complement of the write classes.
 *
 * INSTRUCTION PREFETCH IS ITS OWN CLASS, split from demand IFETCH, and data prefetch likewise from
 * DFETCH. Merging prefetch into its demand class was the earlier design and it made prefetch
 * occupancy unobservable on PARAMS.google: the data prefetchers are off there
 * (--pref_framework_on 0, --pref_stream_on 0), so every prefetch is an FDIP instruction prefetch
 * and a data-prefetch-only chain reads zero while the real prefetch traffic hides among the
 * instruction lines.
 *
 * WRITEBACK IS ITS OWN CLASS, split from demand DSTORE. Both carry write traffic, but a writeback
 * is an eviction arriving from the level above rather than a program store, and they have no reason
 * to share a bucket. Cache_Entry.was_written still spans both, because the REPL_MLP store penalty
 * is about write traffic as such; this is about provenance.
 *
 * OTHER exists so the enum is total (MRT_MIN_PRIORITY, and any type added later) rather than
 * silently folding an unmapped type into a real class. It is walked but not histogrammed, so a set
 * whose six counts fall short of its valid ways holds OTHER lines. */
typedef enum Load_Prio_Traffic_enum {
  LOAD_PRIO_TC_IFETCH, /* IFETCH -- demand instruction fetch */
  LOAD_PRIO_TC_IPREF,  /* IPRF, UOCPRF, FDIPPRFON, FDIPPRFOFF -- instruction prefetch */
  LOAD_PRIO_TC_LOAD,   /* DFETCH -- demand data load */
  LOAD_PRIO_TC_DPREF,  /* DPRF -- data prefetch */
  LOAD_PRIO_TC_STORE,  /* DSTORE -- demand store */
  LOAD_PRIO_TC_WB,     /* WB, WB_NODIRTY -- writeback from the level above */
  LOAD_PRIO_TC_OTHER,  /* anything unmapped; walked but not histogrammed */
  LOAD_PRIO_NUM_TC
} Load_Prio_Traffic;

/* --load_prio_stats: the most levels the boost can take. mlp_lin_costq returns 0..7 (8 values)
 * and mlp_lin_bound_term returns one of {0, lin_lam_data, lin_lam_instr} (3 values), so the sum
 * takes at most 8*3 = 24 distinct values. Fewer in practice: with the default
 * --mlp_lin_data_lambda 0 / --mlp_lin_instr_lambda 0 the bound term is always 0 and there are
 * exactly 8. The stat chains are sized for the maximum and only the first prio_num_levels of
 * them are ever charged. */
#define LOAD_PRIO_MAX_LEVELS 24

/* --load_prio_stats: the highest way count the set-composition chains can name, so those chains
 * are LOAD_PRIO_COMP_MAX_WAYS + 1 entries long (0..16 inclusive). 16 is the associativity of both
 * tracked caches -- --mlc_assoc and --l1_assoc are 16 on every PARAMS file in the tree. A cache
 * configured wider still works: the counting side clamps, so the top bucket saturates rather than
 * the chain being overrun. */
#define LOAD_PRIO_COMP_MAX_WAYS 16

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

  /* --reuse_dist_stats: one monotonic access counter per set, incremented once per
     replacement-updating cache_access that indexes it (hits AND misses -- both are accesses the
     set sees). NULL when the knob is off, which is also the enable test on every hot path.
     Counter is 64-bit and never wraps, unlike Mockingjay's 8-bit sampler timestamp, so a long
     distance is measured rather than saturated.

     A probe with update_repl==FALSE (warmup / oracle lookups) does NOT tick it and does not
     record a distance: those accesses do not exist as far as replacement is concerned, and
     counting them would inflate every distance by however many probes happened to interleave. */
  Counter* set_access_ctr;

  /* --reuse_dist_stats: published by the most recent cache_access, for the caller to count.
     Same publish-then-count split as last_hit_membound: cache_lib measures, memory.c knows
     which cache it is holding and therefore which stat chain to charge. */
  Flag    last_hit_reuse_valid; /* the last access was a hit that produced a distance */
  Counter last_hit_reuse_dist;  /* that distance, in accesses to the set; >= 1 */

  /* --reuse_dist_stats: published by the most recent insert, describing the line it evicted.
     last_evict_valid (above) says whether there WAS a victim; these describe it. */
  Flag last_evict_reused;   /* the victim had been hit at least once since its fill */
  Flag last_evict_membound; /* the victim was filled by a membound access */
  Flag last_evict_fe_bound; /* ... or by an FE-bound one (mutually exclusive with membound) */

  /* --load_prio_stats: one monotonic MISS counter per set, incremented once per
     replacement-updating cache_access that indexes the set and does NOT find its line. NULL
     unless the knob is on AND this cache runs REPL_MLP, and that NULL doubles as the enable test
     on every hot path here, exactly as set_access_ctr does for --reuse_dist_stats.

     Deliberately a SECOND counter rather than a reuse of set_access_ctr, which counts hits and
     misses alike. The unit here is allocation pressure: a miss is what displaces a line, a hit
     costs the resident lines nothing, so residency and reuse distance measured in misses say
     directly how many allocation opportunities a line survived. The two counters are independent
     and either knob works without the other. */
  Counter* set_miss_ctr;

  /* --load_prio_stats: the distinct values the REPL_MLP boost can take, ascending, with
     prio_num_levels of them valid. Built ONCE at init from the 8 x 3 (costq, bound-class) grid --
     see Cache_Entry.prio_level for the expression -- and a line's prio_level is an index into it.

     Built from the GLOBAL --mlp_lin_* params, not from the per-cache lin_lambda_* override, and
     that is safe rather than lucky: cache_set_lin_lambdas is only ever called by the SBAR
     selector, and ps_enabled() requires MLC_CACHE_REPL_POLICY == REPL_MLP_PAPER, so no cache
     running REPL_MLP can be SBAR-owned. load_prio_level_of asserts lin_lambda_override is FALSE
     so that a future change making SBAR reachable from REPL_MLP fails loudly here instead of
     silently stamping levels against a stale table.

     prio_num_levels == 0 means "not tracked" and is the state on every non-REPL_MLP cache. */
  double prio_boost[LOAD_PRIO_MAX_LEVELS];
  uns    prio_num_levels;

  /* --load_prio_stats: published by the most recent cache_access, for the caller to count. Same
     publish-then-count split as last_hit_membound -- cache_lib keeps no stats and memory.c knows
     which cache it is holding and therefore which chain to charge. Cleared at the top of every
     access, so a reader sees the access it belongs to and never a stale value. */
  Flag    last_hit_prio_valid;  /* the last access was a hit on a tracked cache */
  uns8    last_hit_prio_level;  /* that line's sticky priority level */
  Counter last_hit_reuse_misses; /* its reuse distance, in MISSES to the set; >= 0 */

  /* --load_prio_stats: the level stamped on the line the most recent INSERT allocated. Separate
     from last_hit_* because a fill is not a hit, and separate from last_evict_* because the
     arriving line and the line it displaced are at independent levels -- the whole question the
     tracker asks is which levels displace which. Written by stamp_load_prio on every tracked
     fill, so a reader immediately after a cache_insert always sees that insert. */
  Flag    last_fill_prio_valid;
  uns8    last_fill_prio_level;

  /* --load_prio_stats: published by the most recent insert, describing the line it evicted.
     last_evict_valid (above) says whether there WAS a victim; these describe it. */
  uns8    last_evict_prio_level;      /* the victim's sticky priority level */
  Counter last_evict_residency_misses; /* its residency, in MISSES to the set */
  Counter last_evict_reuse_count;      /* how many times it was hit before being evicted */
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
/* --mlp_lin_offpath_lambda: stage the wrong-path flag for the next fill. */
void cache_set_next_fill_offpath(Flag is_offpath);

/* The complete one-shot fill staging, saved and restored as ONE object.
 *
 * There are six fields now. Saving them as separate get/put pairs meant every caller that
 * wraps a cache_insert had to remember all six, and forgetting one is silent: the ATD inserts
 * once stole the staged classification a real fill had set, costing -1.4% IPC with no symptom
 * but the number. One struct makes that class of bug unrepresentable. */
typedef struct Cache_Fill_Stage_struct {
  Flag   membound;
  Flag   fe_bound;
  double bound_frac;
  double mlp_cost;
  Flag   prefetch;
  Flag   store;
  Flag   offpath;
  uns8   traffic; /* --load_prio_stats: a Load_Prio_Traffic */
} Cache_Fill_Stage;

void cache_save_fill_stage(Cache_Fill_Stage* out);
void cache_restore_fill_stage(const Cache_Fill_Stage* in);
void cache_clear_fill_stage(void);
/* --mlp_lin_store_lambda: stage the write flag for the next fill, and set it on a resident
   line when a writeback hits it. */
void cache_set_next_fill_store(Flag is_store);
Flag cache_get_fill_store(void);
void cache_mark_written(Cache* cache, Addr addr);
/* --mlp_lin_offpath_lambda: an on-path demand hit vindicates a wrong-path fill; drop the flag. */
void cache_clear_offpath(Cache* cache, Addr addr);
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

/* --reuse_dist_stats: the REUSE DISTANCE of the most recent cache_access on `cache`, in
 * ACCESSES TO THAT SET, measured from the hit line's previous access (its fill, or its last
 * hit). 1 means the very next access to the set reused the line.
 *
 * Returns FALSE, leaving *dist untouched, when the last access was a miss, was a probe
 * (update_repl==FALSE), or when the knob is off. Valid only immediately after the cache_access
 * it describes. The caller adds the class split: read cache_last_hit_membound /
 * cache_last_hit_fe_bound alongside this to charge the right histogram.
 *
 * Compare against ASSOCIATIVITY, not against a cycle count: a distance below assoc is a reuse
 * LRU would have captured, so the mass above assoc is what any non-LRU policy is competing for. */
Flag cache_last_reuse_dist(Cache* cache, Counter* dist);

/* --reuse_dist_stats: describe the line the most recent INSERT on `cache` evicted.
 *
 * Returns FALSE when that insert evicted nothing (a free way). Otherwise returns TRUE and
 * writes whether the victim was ever reused, and which marked class it belonged to.
 *
 * This is the other half of the distance histogram and is NOT optional bookkeeping: the
 * histogram can only contain lines that were reused, so it is a biased sample on its own. A
 * policy that protects marked lines can raise their mean reuse distance simply by keeping the
 * hopeless ones alive longer, which shows up here as a rising *_NOREUSE and nowhere else.
 * Same validity window as cache_last_evict_age. */
Flag cache_last_evict_reuse(Cache* cache, Flag* reused, Flag* membound, Flag* fe_bound);

/**************************************************************************************/
/* --load_prio_stats: priority-bucketed load tracker. See --load_prio_stats in
 * memory.param.def for what the levels mean and why the unit is misses to the set.
 *
 * Every entry point below is inert on a cache that is not tracked (knob off, or a policy other
 * than REPL_MLP): the readers return FALSE / 0 and the writers do nothing. Callers therefore need
 * no policy test of their own. */

/* Stage the traffic class of the NEXT fill into any cache, one-shot, in the same style as
 * cache_set_next_fill_bound. Only the caller knows the Mem_Req type, so only the caller can
 * classify. Consumed and cleared by the next cache_insert, so a fill that never happens (a
 * bypass, or an early FAILURE return) cannot leak its class onto an unrelated later fill.
 * Never called = LOAD_PRIO_TC_OTHER, which is not histogrammed. */
void cache_set_next_fill_traffic(uns8 traffic);

/* How many priority levels this cache's boost can take, and what boost level `level` stands for.
 * 0 levels means the cache is not tracked. The boost is in LRU stack positions, so the value is
 * directly comparable against associativity. Pure. */
uns    cache_load_prio_num_levels(Cache* cache);
double cache_load_prio_boost(Cache* cache, uns level);

/* The priority level of the line the most recent cache_access on `cache` HIT, and that hit's
 * reuse distance in MISSES TO THE SET measured from the line's previous access (its fill, or its
 * last hit). 0 means no miss to the set intervened -- the line was reused before anything could
 * displace it.
 *
 * Returns FALSE, leaving both out-params untouched, when the last access was a miss, was a probe
 * (update_repl==FALSE), or when the cache is not tracked. Valid only immediately after the
 * cache_access it describes.
 *
 * Compare the distance against ASSOCIATIVITY: a line reused within fewer misses than the set has
 * ways was never at risk, so only the mass at or above assoc is what a priority-aware policy can
 * actually convert. */
Flag cache_last_hit_prio(Cache* cache, uns8* level, Counter* reuse_misses);

/* The priority level stamped on the line the most recent INSERT on `cache` allocated. Returns
 * FALSE on an untracked cache. Valid only immediately after the cache_insert it describes. */
Flag cache_last_fill_prio(Cache* cache, uns8* level);

/* Describe the line the most recent INSERT on `cache` evicted: its priority level, its residency
 * in MISSES to the set, and how many times it was hit before being evicted (0 = never reused).
 *
 * Returns FALSE when that insert evicted nothing (it took a free way). Same validity window as
 * cache_last_evict_age -- call it immediately after the insert, before any other insert on this
 * cache.
 *
 * READ THIS ALONGSIDE the hit chain, not instead of it: the reuse-distance histogram can only
 * contain lines that WERE reused, so on its own it is a biased sample. A policy that protects a
 * level can raise that level's mean reuse distance purely by keeping hopeless lines resident
 * longer, which shows up as a rising count of reuse_count==0 evictions here and nowhere else. */
Flag cache_last_evict_prio(Cache* cache, uns8* level, Counter* residency_misses, Counter* reuse_count);

/* Walk the set `addr` maps to and report what it currently holds. Both are meant to be called at
 * a FILL, before the inserting line has displaced anything, so what they describe is the set the
 * fill is arriving into.
 *
 * cache_load_prio_set_composition fills `counts[LOAD_PRIO_NUM_TC]` with the number of resident
 * lines of each traffic class. The four histogrammed classes need not sum to assoc -- invalid
 * ways and LOAD_PRIO_TC_OTHER lines are the difference.
 *
 * cache_load_prio_set_occupancy fills `counts[LOAD_PRIO_MAX_LEVELS]` with the number of resident
 * lines at each priority level. Summed over fills this is what the cache HOLDS rather than what
 * it admits, which is the view the per-fill event chains cannot give: a level can take a small
 * share of fills and still occupy most of the cache, or the reverse.
 *
 * Both are pure, and both no-op (leaving `counts` zeroed) on an untracked cache. */
void cache_load_prio_set_composition(Cache* cache, Addr addr, uns* counts);
void cache_load_prio_set_occupancy(Cache* cache, Addr addr, uns* counts);

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
