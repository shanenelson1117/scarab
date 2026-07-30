# Cap branch-delay attribution to the N most-important loads per branch

## Context

The `--branch_load_dep_attribute_record` machinery in `src/br_exec_wait.cc`
charges **one** delaying load per stall cycle of a mispredicted branch. Over a
branch's full stall the charged load changes cycle-to-cycle (whichever miss is
currently critical), so a single branch can end up marking an unbounded number
of distinct load instances in `branch_delay_loads.csv` and the `_pf.csv`
prefetch-trigger stream.

This change caps that to **N distinct load instances per dynamic branch
instance**, while still keeping the *most important* loads (not just the first N
seen). The ranking metric is selectable by a flag so both notions of "important"
can be compared for prefetchability:

1. **Total stall cycles** charged to the load under that branch (matches how the
   CSV already sorts).
2. **Criticality tier** — ANCESTOR (in-cone) over WINDOW_CLOG, then deeper cone
   depth.

`N = 0` (default) reproduces the pre-change output exactly.

## Key insight

`tracked` (the branch being charged each cycle) advances **monotonically** in
program order — the oldest stalled branch stays oldest until it resolves, then
the window moves strictly forward. So only **one branch's worth** of load
candidates is ever live. We buffer that branch's per-load accumulation and
**commit the top-N when the branch changes** (and once more at sim end). No
unbounded per-branch map is needed.

The pre-change charge path wrote straight into the global aggregate maps
(`br_charge_line`) and pf stream (`br_pf_emit`); that immediacy is what prevents
"keep the top-N", so we insert a per-branch buffer in between.

## Parameters (`src/core.param.def`)

```c
/* Cap distinct delaying loads marked per dynamic mispredicted-branch instance
   in the attribute-record CSV/pf stream. 0 = unlimited (pre-change behavior). */
DEF_PARAM(branch_load_dep_attr_max_loads, BRANCH_LOAD_DEP_ATTR_MAX_LOADS, uns, uns, 0, )
/* Ranking used to pick the kept loads when the cap bites:
   0 = total stall cycles charged to the load; 1 = criticality tier
   (ANCESTOR before WINDOW_CLOG, then deeper cone depth). */
DEF_PARAM(branch_load_dep_attr_rank, BRANCH_LOAD_DEP_ATTR_RANK, uns, uns, 0, )
```

## Implementation (`src/br_exec_wait.cc`)

### Per-current-branch buffer

```c
struct BrDelayLoad {
  Addr    pc, line;
  Counter cycles, cold;   // accumulated over the branch's stall
  Counter seqnum;         // for br_pf_emit; captured at first sighting
  uns     cone_depth;     // deepest cone depth seen (0 = window-clog)
  uns8    category;       // 0 = ANCESTOR (g_anc), 1 = WINDOW_CLOG (g_clog)
  uns8    proc_id;
};
static Counter g_cur_branch_uid;      // branch unique_num
static Addr    g_cur_branch_pc;       // branch PC (for stream CSV)
static Counter g_cur_branch_seqnum;   // branch op_num (stream order)
static std::unordered_map<Counter, BrDelayLoad> g_cur_loads;  // key = load unique_num
static Counter g_capped_total;        // cycles dropped by the cap

struct BrStreamRow { Counter branch_seqnum; Addr branch_pc; std::vector<Addr> load_lines; };
static std::vector<BrStreamRow> g_brstream_rows;
```

### Cone-depth out-param

`br_find_cone_critical_miss` already tracks `best_depth`; it takes an
`int* out_depth` out-param so the caller records the depth of the chosen miss.
Window-clog loads get depth 0.

### `br_record_delay_load` — buffer instead of charge

Per cycle:
- If `branch_op->unique_num != g_cur_branch_uid`: commit the previous branch,
  clear `g_cur_loads`, update uid/pc/seqnum.
- Pick the miss: cone-critical first (capture `cone_depth`, `category=0`), else
  window-clog (`category=1`, depth 0), else `g_none_total++` and return.
- Reuse study stays immediate: `bld_reuse_mark_delaying_pc(proc_id, pc)` is
  still called every charged cycle (un-capped; separate measurement).
- Accumulate into `g_cur_loads[m->unique_num]`: capture pc/line/category/proc_id
  /cone_depth/seqnum on first sighting; every cycle `cycles++`,
  `cone_depth=max(...)`, `cold += (g_line_miss_count[line] <= 1)`.

### `br_commit_current_branch` — fold the top-N into the globals

- Sort buffered loads by the selected comparator:
  - **rank 0 (cycles):** cycles desc, category (ANCESTOR first), seqnum asc.
  - **rank 1 (criticality):** category asc, cone_depth desc, cycles desc,
    seqnum asc.
- Keep first `min(N, size)` (`N==0` keeps all). Kept loads fold into
  `g_anc`/`g_clog` exactly as `br_charge_line` did, and push `{seqnum, line}`
  into `g_pf_rows` guarded by `g_pf_seen`.
- Dropped loads: `g_none_total += cycles` and `g_capped_total += cycles`, so
  `total = anc + clog + none` still equals `BR_EXEC_OPERAND_WAIT_MISPRED`.
- Record a `BrStreamRow` with the kept load lines in ranked order.

### Outputs (`br_exec_wait_finish`)

`br_commit_current_branch()` runs first so the last in-flight branch commits
before CSVs are written. `capped_cycles` is added to the
`branch_delay_loads.csv` header.

- **`_pf.csv` (prefetch experiment)** — unchanged `(seqnum, line)` format;
  `seqnum` is the load's commit/retire `op_num` (off-path window-clog loads
  anchor to the consuming branch's `op_num`). Emitted for committed top-N loads.
- **`_brstream.csv` (new)** — one row per committed dynamic branch instance:
  `branch_seqnum,branch_pc,num_loads,load_lines`, where `load_lines` is the
  space-separated kept (top-N) load line addresses in ranked order.

## Compatibility with the pf-replay prefetcher

The seqnum-driven replay prefetcher (`--branch_load_dep_pf_replay`) reads
`_pf.csv` via `br_pf_replay_load` (`sscanf(buf, "%llu,0x%llx", ...)`) and
advances a single non-resetting retire-driven cursor over triggers sorted by
seqnum. Because the format, header/writer, and `seqnum = load op_num` semantics
are unchanged, the replay path parses the capped output with **no code changes**
— it just sees a smaller trigger set.

### Seqnum monotonicity

The replay cursor is correct only if triggers are sorted ascending by seqnum.
Preserved because: seqnum values are unchanged, and sortedness comes from
explicit sorts at write and read time (commit-time ranked appends are locally
out-of-seqnum but get sorted). Dedup fires only for kept loads, so a load
dropped by one branch's cap remains eligible for a later branch.

## Verification

1. Build: `cd src && make`.
2. **Regression (N=0):** `diff` `branch_delay_loads.csv` / `_pf.csv` against the
   pre-change build — must be identical.
3. **Cap:** `max_loads=1,2` — `_pf.csv` rows non-increasing, `capped_cycles`
   rises, `total_wait_cycles` constant.
4. **Rank:** `attr_rank=0` vs `1` at fixed `max_loads` — kept sets differ,
   `total_wait_cycles` constant.
5. **Stream CSV:** every row `num_loads <= max_loads`; `load_lines` match that
   branch's `_pf.csv` contribution; ordered by `branch_seqnum`.
6. **pf-replay end-to-end:** feed a capped `_pf.csv` to a
   `--branch_load_dep_pf_replay 1` run; `br_pf_replay: loaded N triggers` matches
   the file row count and MRT_DPRF prefetches issue.
