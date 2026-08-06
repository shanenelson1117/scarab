/***************************************************************************************
 * File         : td_load_replay.cc
 * Description  : Replay side of the per-load memory-boundness feature.
 ***************************************************************************************/

#include "td_load_replay.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <list>
#include <unordered_map>
#include <vector>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "core.param.h"

#include "dcache_stage.h"
}

/**************************************************************************************/
/* State */

static bool g_replay_loaded = false;
static std::unordered_map<uns64, double> g_ratio;  // key (PC or va) -> mean mem-bound fraction

/* td_load_evict_track: per-set shadow LRU of distinct filled lines (MRU front), used to
 * count how many distinct lines were filled into set(l) between two consecutive exceeding
 * accesses to line l. A line with an open interval is "tracked" (frozen: its own refills
 * do not move it) so its index = cumulative distinct-other-line fills since it was pinned. */
static const size_t TD_EVICT_CAP_MULT = 64;  // per-set list capped at TD_EVICT_CAP_MULT * assoc

struct EvictState {
  std::list<uns64> lru;                                        // distinct line addrs, MRU front
  std::unordered_map<uns64, std::list<uns64>::iterator> pos;   // line -> node
  std::unordered_map<uns64, bool> tracked;                     // line -> has open interval
};
static std::vector<EvictState> g_evict;  // indexed by set
static size_t g_evict_cap = 0;           // per-set length cap (0 until initialized)
static FILE* g_evict_fp = NULL;

/**************************************************************************************/
/* Path derivation (shared with the record side) */

void td_load_derive_csv_path(const char* dir, const char* suffix, char* out, size_t sz) {
  const char* trace = CBP_TRACE_R0;

  if (!trace || !*trace) {
    snprintf(out, sz, "%s/%s.csv", dir, suffix);
    return;
  }

  // The only per-simpoint-unique key shared by the record and replay runs is the full
  // trace path (CBP_TRACE_R0). Sanitize it into a flat filename token by replacing every
  // non-alphanumeric char with '_', so all simpoints coexist in one --td_load_dir without
  // collision (e.g. .../spec2006/rate_fp/bwaves/11529/traces/pt/trace.gz ->
  // <...>_spec2006_rate_fp_bwaves_11529_traces_pt_trace_gz). Identical across both runs.
  char token[MAX_STR_LENGTH + 1];
  size_t j = 0;
  for (size_t i = 0; trace[i] != '\0' && j < sizeof(token) - 1; i++) {
    char c = trace[i];
    Flag alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    token[j++] = alnum ? c : '_';
  }
  token[j] = '\0';

  // trim leading underscores (from the leading '/') for tidiness
  const char* t = token;
  while (*t == '_')
    t++;

  snprintf(out, sz, "%s/%s_%s.csv", dir, t, suffix);
}

void td_load_mkdir_p(const char* dir) {
  if (!dir || !*dir)
    return;

  char tmp[MAX_STR_LENGTH + 1];
  strncpy(tmp, dir, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  size_t len = strlen(tmp);
  if (len && tmp[len - 1] == '/')
    tmp[len - 1] = '\0';

  for (char* p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);  // ignore EEXIST and other benign errors
      *p = '/';
    }
  }
  mkdir(tmp, 0755);
}

/**************************************************************************************/
/* CSV load */

void td_load_replay_init(void) {
  if (g_replay_loaded)
    return;
  g_replay_loaded = true;

  const char* key = TD_LOAD_REPLAY_KEY;
  const char* suffix = (key && strcmp(key, "addr") == 0) ? "mem_bound_loads_addr" : "mem_bound_loads_pc";

  char path[MAX_STR_LENGTH + 1];
  td_load_derive_csv_path(TD_LOAD_DIR, suffix, path, sizeof(path));

  FILE* fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "[TD_LOAD] Replay CSV not found: %s (no loads will be forced)\n", path);
    return;
  }

  std::unordered_map<uns64, double> sum;
  std::unordered_map<uns64, uns64> cnt;

  char line[512];
  // skip header line ("pc,mem_bound_fraction" / "addr,mem_bound_fraction")
  if (!fgets(line, sizeof(line), fp)) {
    fclose(fp);
    return;
  }

  while (fgets(line, sizeof(line), fp)) {
    unsigned long long k = 0;
    double frac = 0.0;
    if (sscanf(line, "0x%llx,%lf", &k, &frac) == 2) {
      sum[(uns64)k] += frac;
      cnt[(uns64)k] += 1;
    }
  }
  fclose(fp);

  for (const auto& e : sum) {
    g_ratio[e.first] = e.second / (double)cnt[e.first];
  }

  fprintf(stderr, "[TD_LOAD] Loaded %zu keys from %s (key=%s, thresh=%f)\n", g_ratio.size(), path, suffix,
          (double)TD_LOAD_REPLAY_THRESH);
}

/**************************************************************************************/
/* Force-hit predicate */

Flag td_load_should_force_l1_hit(Op* op) {
  if (!TD_LOAD_REPLAY)
    return FALSE;

  td_load_replay_init();
  if (g_ratio.empty())
    return FALSE;

  const char* key = TD_LOAD_REPLAY_KEY;
  uns64 k = (key && strcmp(key, "addr") == 0) ? op->oracle_info.va : op->inst_info->addr;

  auto it = g_ratio.find(k);
  if (it == g_ratio.end())
    return FALSE;

  return (it->second > (double)TD_LOAD_REPLAY_THRESH) ? TRUE : FALSE;
}

/**************************************************************************************/
/* Eviction tracking: distinct lines filled into set(l) between consecutive exceeding
 * accesses to the same line l (line/address-keyed; self-contained, no record CSV). */

static void td_evict_lazy_init(void) {
  if (g_evict_cap != 0)
    return;
  uns assoc = dcache_get_assoc();
  uns num_sets = dcache_get_num_sets();
  g_evict_cap = (size_t)(assoc ? assoc : 1) * TD_EVICT_CAP_MULT;
  g_evict.resize(num_sets ? num_sets : 1);
}

/* Called on every L1D fill: push the filled line to MRU of its set, unless it is a
 * tracked line with an open interval (then leave it frozen so its distinct-fill count
 * keeps accumulating). Cap the per-set list length, censoring the far tail. */
void td_load_evict_note_fill(Addr fill_addr) {
  if (!TD_LOAD_EVICT_TRACK)
    return;
  td_evict_lazy_init();

  Addr line_addr;
  uns set = dcache_get_set_index(fill_addr, &line_addr);
  if (set >= g_evict.size())
    return;
  EvictState& s = g_evict[set];

  auto ti = s.tracked.find(line_addr);
  if (ti != s.tracked.end() && ti->second)
    return;  // frozen: an open interval owns this line's position

  auto pi = s.pos.find(line_addr);
  if (pi != s.pos.end())
    s.lru.erase(pi->second);
  s.lru.push_front(line_addr);
  s.pos[line_addr] = s.lru.begin();

  while (s.lru.size() > g_evict_cap) {
    uns64 victim = s.lru.back();
    s.lru.pop_back();
    s.pos.erase(victim);
    s.tracked.erase(victim);
  }
}

/* Called at retirement of an exceeding load: emit S (distinct fills into set(l) since the
 * line's previous exceeding access) and W, then re-pin the line to open the next interval. */
void td_load_evict_note_access(Op* op) {
  if (!TD_LOAD_EVICT_TRACK)
    return;
  if (op->inst_info->table_info.mem_type != MEM_LD)
    return;
  if (op->td_window_cycles == 0)
    return;

  // only accesses whose own instance memory-bound fraction exceeds the threshold
  double frac = (double)op->td_mem_cycles / (double)op->td_window_cycles;
  if (frac <= (double)TD_LOAD_REPLAY_THRESH)
    return;

  td_evict_lazy_init();

  Addr line_addr;
  uns set = dcache_get_set_index(op->oracle_info.va, &line_addr);
  if (set >= g_evict.size())
    return;
  EvictState& s = g_evict[set];
  uns ways = dcache_get_assoc();

  // If this line already had an open interval, this is its x+1: measure and emit.
  auto ti = s.tracked.find(line_addr);
  if (ti != s.tracked.end() && ti->second) {
    size_t S;
    auto pi = s.pos.find(line_addr);
    if (pi == s.pos.end()) {
      S = g_evict_cap;  // fell off the cap -> evicted by >= cap (censored)
    } else {
      S = (size_t)std::distance(s.lru.begin(), pi->second);
    }

    if (!g_evict_fp) {
      char suffix[64];
      snprintf(suffix, sizeof(suffix), "line_evict_p%02d", (int)(100.0 * (double)TD_LOAD_REPLAY_THRESH + 0.5));
      char path[MAX_STR_LENGTH + 1];
      td_load_mkdir_p(TD_LOAD_DIR);
      td_load_derive_csv_path(TD_LOAD_DIR, suffix, path, sizeof(path));
      g_evict_fp = fopen(path, "w");
      if (g_evict_fp)
        fprintf(g_evict_fp, "line,distinct_conflicts,ways\n");
      else
        fprintf(stderr, "[TD_LOAD] cannot open %s for writing\n", path);
    }
    if (g_evict_fp)
      fprintf(g_evict_fp, "0x%llx,%llu,%u\n", (unsigned long long)line_addr, (unsigned long long)S, ways);
  }

  // (Re-)pin this line at MRU and mark tracked -> opens the next interval from here.
  auto pi = s.pos.find(line_addr);
  if (pi != s.pos.end())
    s.lru.erase(pi->second);
  s.lru.push_front(line_addr);
  s.pos[line_addr] = s.lru.begin();
  s.tracked[line_addr] = true;
}

void td_load_evict_finish(void) {
  if (g_evict_fp) {
    fclose(g_evict_fp);
    g_evict_fp = NULL;
  }
}
