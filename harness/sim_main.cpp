// Simulator entry point used by OpenEvolve evaluator.
// Usage:
//   sim_main --layout=<path.json> --cache-bytes=<N> [--num-threads=N]
//            [--num-ops=N] [--found-rate=F] [--seed=S] [--dist=uniform|zipfian]
//            [--zipfian=0.99]
// Output: JSON on stdout (single object) with base_us_per_op, effective_us_per_op,
// cache_hit_rate, etc. Non-zero exit on failure.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "layout_loader.hpp"
#include "sim_engine.hpp"
#include "workload.hpp"

using namespace hymeta;

struct Args {
  std::string layout = "/home/godong/hymeta_evolve/bench_results/sst_layout.json";
  int64_t cache_bytes = 64LL << 20;
  int num_threads = 1;
  int64_t num_ops = 1'000'000;
  double found_rate = 0.63;
  uint64_t seed = 42;
  std::string dist = "uniform";
  double zipfian = 0.99;
  int des_mode = 0;  // 0 = post-hoc saturation; 1 = event-driven FIFO disk
  int64_t reapply_every = 0;  // 0 = static (call select_scheme once); >0 = re-apply every N ops
  // Mixgraph params (used only when --dist=mixgraph).
  double mix_get_ratio = 0.83;
  double mix_seek_ratio = 0.17;
  int64_t mix_keyrange_num = 30;
  double mix_keyrange_a = 14.18;
  double mix_keyrange_b = -0.3093;
  double mix_key_a = 0.002312;
  double mix_key_b = 0.3467;
};

static bool parse_flag(const std::string& arg, const char* name,
                       std::string* out) {
  std::string pre = std::string("--") + name + "=";
  if (arg.rfind(pre, 0) == 0) {
    *out = arg.substr(pre.size());
    return true;
  }
  return false;
}

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    std::string v;
    if (parse_flag(s, "layout", &v))
      a.layout = v;
    else if (parse_flag(s, "cache-bytes", &v))
      a.cache_bytes = std::stoll(v);
    else if (parse_flag(s, "num-threads", &v))
      a.num_threads = std::stoi(v);
    else if (parse_flag(s, "num-ops", &v))
      a.num_ops = std::stoll(v);
    else if (parse_flag(s, "found-rate", &v))
      a.found_rate = std::stod(v);
    else if (parse_flag(s, "seed", &v))
      a.seed = (uint64_t)std::stoull(v);
    else if (parse_flag(s, "dist", &v))
      a.dist = v;
    else if (parse_flag(s, "zipfian", &v))
      a.zipfian = std::stod(v);
    else if (parse_flag(s, "des-mode", &v))
      a.des_mode = std::stoi(v);
    else if (parse_flag(s, "reapply-every", &v))
      a.reapply_every = std::stoll(v);
    else if (parse_flag(s, "mix-get-ratio", &v))
      a.mix_get_ratio = std::stod(v);
    else if (parse_flag(s, "mix-seek-ratio", &v))
      a.mix_seek_ratio = std::stod(v);
    else if (parse_flag(s, "mix-keyrange-num", &v))
      a.mix_keyrange_num = std::stoll(v);
    else if (parse_flag(s, "mix-keyrange-a", &v))
      a.mix_keyrange_a = std::stod(v);
    else if (parse_flag(s, "mix-keyrange-b", &v))
      a.mix_keyrange_b = std::stod(v);
    else if (parse_flag(s, "mix-key-a", &v))
      a.mix_key_a = std::stod(v);
    else if (parse_flag(s, "mix-key-b", &v))
      a.mix_key_b = std::stod(v);
    else {
      std::cerr << "unknown flag: " << s << "\n";
      std::exit(2);
    }
  }
  return a;
}

// Build SSTStats snapshot from current SSTFile counters and apply select_scheme.
// Returns count of SSTs whose scheme changed (0 on initial call when all schemes
// equal SCHEME_FULL default and policy picks SCHEME_FULL — still counted as no-op).
static int apply_policy(std::vector<SSTFile>& ssts) {
  int changed = 0;
  for (auto& s : ssts) {
    SSTStats stats;
    stats.sst_id = s.sst_id;
    stats.level = s.level;
    stats.num_keys = s.num_keys;
    stats.access_count = s.access_count;
    stats.point_lookup_count = s.point_lookup_count;
    stats.scan_count = s.scan_count;
    stats.filter_rejection_rate = (s.filter_checked == 0)
        ? 0.0
        : (double)s.filter_rejected / (double)s.filter_checked;
    uint64_t total = s.cache_hits + s.cache_misses;
    stats.cache_hit_rate =
        (total == 0) ? 0.0 : (double)s.cache_hits / (double)total;
    stats.full_metadata_size = s.full_index_size + s.full_filter_size;
    stats.partitioned_num_partitions =
        s.partitioned_num_filter_partitions + s.partitioned_num_index_partitions;
    stats.unify_num_partitions = s.unify_num_partitions;
    Scheme new_scheme = select_scheme(stats);
    if (new_scheme != s.active_scheme) {
      s.active_scheme = new_scheme;
      changed++;
    }
  }
  return changed;
}

int main(int argc, char** argv) {
  Args a = parse_args(argc, argv);

  SimConfig cfg;
  cfg.cache.capacity = a.cache_bytes;
  cfg.num_threads = a.num_threads;
  cfg.wl.num_operations = a.num_ops;
  cfg.wl.found_rate = a.found_rate;
  cfg.wl.seed = a.seed;
  cfg.wl.distribution = a.dist;
  cfg.wl.zipfian_constant = a.zipfian;
  cfg.wl.mix_get_ratio = a.mix_get_ratio;
  cfg.wl.mix_seek_ratio = a.mix_seek_ratio;
  cfg.wl.mix_keyrange_num = a.mix_keyrange_num;
  cfg.wl.mix_keyrange_a = a.mix_keyrange_a;
  cfg.wl.mix_keyrange_b = a.mix_keyrange_b;
  cfg.wl.mix_key_a = a.mix_key_a;
  cfg.wl.mix_key_b = a.mix_key_b;
  cfg.layout_path = a.layout;

  auto ssts_raw = load_sst_layout_json(cfg.layout_path, cfg.lsm.key_size,
                                         cfg.lsm.value_size);
  if (ssts_raw.empty()) {
    std::cerr << "layout is empty\n";
    return 2;
  }

  LSMTree lsm(cfg.lsm, std::move(ssts_raw));

  // Initial apply: stats are all-zero (no ops processed yet). Subsequent
  // re-applies (if reapply_every > 0) inside the workload loop see live stats.
  apply_policy(lsm.all_ssts());

  HyperClockCache cache(cfg.cache.capacity);
  IOModel io(cfg.io, &cache);

  int64_t max_key = 0;
  for (auto& s : lsm.all_ssts()) max_key = std::max(max_key, s.max_key);
  WorkloadGen wg(cfg.wl, max_key);

  std::mt19937_64 rng_filter(cfg.wl.seed ^ 0xDEADBEEFDEADBEEFULL);

  auto t0 = std::chrono::high_resolution_clock::now();
  double total_cost = 0.0;
  int64_t op_count = 0;
  std::vector<SSTFile*> cand;
  cand.reserve(cfg.lsm.num_levels + 8);

  // DES state: per-thread timelines + shared disk queue.
  DiskQueue disk(&cfg.io);
  std::vector<double> thread_now(std::max(1, a.num_threads), 0.0);
  const bool des_on = (a.des_mode != 0) && (a.num_threads > 1);

  // Dynamic-policy bookkeeping. When reapply_every>0, select_scheme is called
  // for every SST after every N ops with current live stats. Old scheme's
  // cache entries are NOT explicitly invalidated; they age out via CLOCK
  // eviction (different schemes use different cache key tags).
  int reapply_count = 0;
  int64_t scheme_transitions = 0;
  int64_t total_gets = 0;
  int64_t total_seeks = 0;

  for (int64_t i = 0; i < cfg.wl.num_operations; ++i) {
    op_count++;
    int64_t key = wg.next_key();
    bool present = wg.next_key_exists();
    OpType op = wg.next_op_type();
    if (op == OP_SEEK) total_seeks += 1; else total_gets += 1;
    lsm.get_ssts_for_key(key, cand);
    if (cand.empty()) continue;
    // Deepest level = "owner" if the key is present.
    std::sort(cand.begin(), cand.end(),
              [](SSTFile* a, SSTFile* b) { return a->level < b->level; });
    SSTFile* owner = present ? cand.back() : nullptr;

    if (!des_on) {
      for (SSTFile* sst : cand) {
        bool key_in_sst = (sst == owner);
        total_cost += io.point_lookup(*sst, key, key_in_sst, rng_filter);
        // Tag this SST's stats as a seek (in addition to point_lookup_count
        // which the engine bumps internally). Policies can read
        // scan_count to gauge scan-heavy SSTs.
        if (op == OP_SEEK) sst->scan_count += 1;
        if (key_in_sst) break;
      }
    } else {
      // Round-robin across virtual threads. Each thread carries its own
      // t_now; misses contend on the shared disk FIFO.
      int tid = (int)(i % a.num_threads);
      double t = thread_now[tid];
      double t_start = t;
      for (SSTFile* sst : cand) {
        bool key_in_sst = (sst == owner);
        t = io.point_lookup_des(*sst, key, key_in_sst, rng_filter, t, disk);
        if (op == OP_SEEK) sst->scan_count += 1;
        if (key_in_sst) break;
      }
      thread_now[tid] = t;
      total_cost += (t - t_start);
    }

    // Periodic policy re-application. Stats are accumulated (not reset).
    if (a.reapply_every > 0 && (i + 1) % a.reapply_every == 0) {
      scheme_transitions += apply_policy(lsm.all_ssts());
      reapply_count += 1;
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double wall_s = std::chrono::duration<double>(t1 - t0).count();

  double base_us = (op_count == 0) ? 0.0 : total_cost / (double)op_count;
  double eff_us = base_us;
  if (des_on) {
    // In DES mode `base_us` is already the per-op avg latency; throughput-wise
    // us_per_op across N threads is max(thread_now) / (N_ops / N_threads).
    double max_t = 0.0;
    for (double v : thread_now) max_t = std::max(max_t, v);
    eff_us = (op_count == 0) ? 0.0 : max_t * (double)a.num_threads / (double)op_count;
  } else {
    eff_us = io.effective_us_per_op(base_us, op_count, cfg.num_threads);
  }
  uint64_t h = cache.total_hits();
  uint64_t m = cache.total_misses();
  double hit_rate = (h + m == 0) ? 0.0 : (double)h / (double)(h + m);
  uint64_t mh = cache.metadata_hits();
  uint64_t mm = cache.metadata_misses();
  double meta_rate = (mh + mm == 0) ? 0.0 : (double)mh / (double)(mh + mm);

  std::ostringstream json;
  json.setf(std::ios::fixed);
  json.precision(6);
  json << "{";
  json << "\"base_us_per_op\":" << base_us << ",";
  json << "\"effective_us_per_op\":" << eff_us << ",";
  // Throughput (matches db_bench convention: ops/sec = N × 1e6 / us_per_op).
  // throughput_per_thread is single-thread effective; total is aggregate
  // across num_threads with bandwidth saturation already folded into eff_us.
  double thr_per_thread = (eff_us > 0.0) ? (1e6 / eff_us) : 0.0;
  double thr_total = thr_per_thread * (double)cfg.num_threads;
  json << "\"throughput_ops_sec_per_thread\":" << thr_per_thread << ",";
  json << "\"throughput_ops_sec_total\":" << thr_total << ",";
  json << "\"op_count\":" << op_count << ",";
  json << "\"total_miss_bytes\":" << io.total_miss_bytes << ",";
  json << "\"total_io_count\":" << io.total_io_count << ",";
  json << "\"cache_hit_rate\":" << hit_rate << ",";
  json << "\"metadata_hit_rate\":" << meta_rate << ",";
  json << "\"evictions\":" << cache.eviction_count() << ",";
  json << "\"num_ssts\":" << lsm.all_ssts().size() << ",";
  json << "\"num_threads\":" << cfg.num_threads << ",";
  json << "\"cache_bytes\":" << cfg.cache.capacity << ",";
  json << "\"sim_wall_s\":" << wall_s << ",";
  json << "\"des_mode\":" << a.des_mode << ",";

  // Dynamic-policy stats.
  int n_full = 0, n_part = 0, n_unify = 0;
  for (const auto& s : lsm.all_ssts()) {
    if (s.active_scheme == SCHEME_FULL) n_full += 1;
    else if (s.active_scheme == SCHEME_PARTITIONED) n_part += 1;
    else n_unify += 1;
  }
  json << "\"reapply_every\":" << a.reapply_every << ",";
  json << "\"reapply_count\":" << reapply_count << ",";
  json << "\"scheme_transitions\":" << scheme_transitions << ",";
  json << "\"distribution\":\"" << cfg.wl.distribution << "\",";
  json << "\"total_gets\":" << total_gets << ",";
  json << "\"total_seeks\":" << total_seeks << ",";
  json << "\"final_scheme_full\":" << n_full << ",";
  json << "\"final_scheme_partitioned\":" << n_part << ",";
  json << "\"final_scheme_unify\":" << n_unify << ",";

  // Phase-level stats (ALL_HIT / HAD_MISS per scheme/phase).
  static const char* scheme_names[3] = {"full", "partitioned", "unify"};
  static const char* phase_names[2] = {"filter", "index"};
  json << "\"phase_stats\":{";
  bool first = true;
  for (int sc = 0; sc < 3; ++sc) {
    for (int ph = 0; ph < 2; ++ph) {
      const auto& ps = io.phase_stats_[sc][ph];
      if (ps.all_hit_count + ps.had_miss_count == 0) continue;
      double ah_us = ps.all_hit_count ? ps.all_hit_total_us / ps.all_hit_count : 0.0;
      double hm_us = ps.had_miss_count ? ps.had_miss_total_us / ps.had_miss_count : 0.0;
      uint64_t tot = ps.all_hit_count + ps.had_miss_count;
      double hit_ratio = tot ? (double)ps.all_hit_count / (double)tot : 0.0;
      if (!first) json << ",";
      first = false;
      json << "\"" << scheme_names[sc] << "_" << phase_names[ph] << "\":{";
      json << "\"all_hit\":" << ps.all_hit_count << ",";
      json << "\"had_miss\":" << ps.had_miss_count << ",";
      json << "\"all_hit_avg_us\":" << ah_us << ",";
      json << "\"had_miss_avg_us\":" << hm_us << ",";
      json << "\"all_hit_ratio\":" << hit_ratio;
      json << "}";
    }
  }
  json << "}";
  json << "}\n";
  std::cout << json.str();
  return 0;
}
