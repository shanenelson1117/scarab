/***************************************************************************************
 * File         : td_load_replay.cc
 * Description  : Replay side of the per-load memory-boundness feature.
 ***************************************************************************************/

#include "td_load_replay.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>

extern "C" {
#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "core.param.h"
}

/**************************************************************************************/
/* State */

static bool g_replay_loaded = false;
static std::unordered_map<uns64, double> g_ratio;  // key (PC or va) -> mean mem-bound fraction

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
