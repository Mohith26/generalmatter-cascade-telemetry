// Command line driver: bench, eval, replay. Writes results/*.json.
//
// Sim time note: one tick is nominally 10 ms of simulated plant time
// (a generic 100 Hz per channel sensor rate). Tick based latencies are
// reported in both ticks and nominal milliseconds.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "eval.hpp"
#include "pipeline.hpp"

using namespace rt;

static constexpr double kTickMs = 10.0;

static const char* arch_string() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "macOS arm64 (Apple silicon)";
#elif defined(__aarch64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

static void write_machine(FILE* f) {
  fprintf(f,
          "  \"machine\": {\n"
          "    \"platform\": \"%s\",\n"
          "    \"logical_cores\": %u,\n"
          "    \"compiler\": \"clang++ -std=c++17 -O2 -pthread\",\n"
          "    \"note\": \"all performance numbers are machine specific\"\n"
          "  },\n",
          arch_string(), std::thread::hardware_concurrency());
}

static double pct(std::vector<double> v, double p) { return percentile(v, p); }

// ---------------------------------------------------------------- bench ----

static PipelineConfig bench_config(uint32_t channels, uint32_t ticks,
                                   uint32_t producers, double paced_hz) {
  PipelineConfig cfg;
  cfg.gen.channels = channels;
  cfg.gen.ticks = ticks;
  cfg.gen.seed = 20260819;
  cfg.gen.episodes_per_type = 0;  // clean stream for throughput measurement
  cfg.det.dropout_gap = 0;        // dropout scan off when nothing can drop out
  cfg.producers = producers;
  cfg.queue_capacity = 1 << 16;
  cfg.paced_tick_hz = paced_hz;
  return cfg;
}

static int cmd_bench(const std::string& outdir) {
  const uint32_t producers = 6;
  struct Row {
    uint32_t channels, ticks;
    PipelineResult r;
  };
  std::vector<Row> thr = {{1000, 48000, {}}, {5000, 12000, {}}, {10000, 6000, {}}};
  for (auto& row : thr) {
    fprintf(stderr, "bench throughput: %u channels...\n", row.channels);
    row.r = run_pipeline(bench_config(row.channels, row.ticks, producers, 0.0));
  }
  // Rated operating point: paced 100 Hz tick rate per channel.
  std::vector<Row> paced = {{1000, 800, {}}, {5000, 800, {}}, {10000, 800, {}}};
  for (auto& row : paced) {
    fprintf(stderr, "bench paced 100 Hz: %u channels...\n", row.channels);
    row.r = run_pipeline(bench_config(row.channels, row.ticks, producers, 100.0));
  }

  std::string path = outdir + "/rt_bench.json";
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    perror("fopen");
    return 1;
  }
  fprintf(f, "{\n");
  write_machine(f);
  fprintf(f,
          "  \"producers\": %u,\n  \"consumers\": 1,\n"
          "  \"queue_capacity\": %d,\n  \"latency_sampling\": \"every 8th frame,"
          " stamped at emit, measured at consumer dequeue\",\n",
          producers, 1 << 16);
  fprintf(f, "  \"throughput_unpaced\": [\n");
  for (size_t i = 0; i < thr.size(); ++i) {
    const auto& r = thr[i].r;
    bool zl = r.frames_dropped == 0 && r.frames_accepted == r.frames_emitted &&
              r.frames_processed == r.frames_accepted;
    std::vector<double> lat = r.latency_ns;
    fprintf(f,
            "    {\"channels\": %u, \"ticks\": %u, \"frames_emitted\": %" PRIu64
            ", \"frames_accepted\": %" PRIu64 ", \"frames_dropped\": %" PRIu64
            ", \"frames_processed\": %" PRIu64
            ", \"wall_s\": %.4f, \"frames_per_sec\": %.0f, \"zero_loss\": %s,"
            " \"saturated_latency_p50_us\": %.1f, \"saturated_latency_p99_us\": %.1f}%s\n",
            thr[i].channels, thr[i].ticks, r.frames_emitted, r.frames_accepted,
            r.frames_dropped, r.frames_processed, r.wall_seconds,
            r.frames_per_sec, zl ? "true" : "false", pct(lat, 0.50) / 1e3,
            pct(lat, 0.99) / 1e3, i + 1 < thr.size() ? "," : "");
  }
  fprintf(f, "  ],\n");
  fprintf(f,
          "  \"rated_operating_point\": {\"tick_hz\": 100.0, \"note\": \"100 Hz"
          " per channel frame rate, blocking backpressure, zero loss required\"},\n");
  fprintf(f, "  \"latency_at_rated_point\": [\n");
  for (size_t i = 0; i < paced.size(); ++i) {
    const auto& r = paced[i].r;
    bool zl = r.frames_dropped == 0 && r.frames_accepted == r.frames_emitted &&
              r.frames_processed == r.frames_accepted;
    std::vector<double> lat = r.latency_ns;
    fprintf(f,
            "    {\"channels\": %u, \"target_frames_per_sec\": %.0f,"
            " \"sustained_frames_per_sec\": %.0f, \"duration_s\": %.2f,"
            " \"latency_samples\": %zu, \"p50_us\": %.1f, \"p99_us\": %.1f,"
            " \"zero_loss\": %s}%s\n",
            paced[i].channels, 100.0 * paced[i].channels, r.frames_per_sec,
            r.wall_seconds, r.latency_ns.size(), pct(lat, 0.50) / 1e3,
            pct(lat, 0.99) / 1e3, zl ? "true" : "false",
            i + 1 < paced.size() ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
  fprintf(stderr, "wrote %s\n", path.c_str());
  return 0;
}

// ----------------------------------------------------------------- eval ----

static PipelineConfig eval_config(uint64_t seed) {
  PipelineConfig cfg;
  cfg.gen.channels = 64;
  cfg.gen.ticks = 20000;
  cfg.gen.seed = seed;
  cfg.gen.episodes_per_type = 12;
  cfg.producers = 4;
  cfg.queue_capacity = 8192;
  return cfg;
}

static int cmd_eval(const std::string& outdir) {
  const uint64_t seed = 20260819;
  PipelineConfig cfg = eval_config(seed);
  PipelineResult r = run_pipeline(cfg);
  auto intervals = build_alarm_intervals(r.log, r.ticks - 1);
  EvalResult ev = evaluate(r.episodes, intervals);

  std::vector<double> det_lat(ev.detection_latency_ticks.begin(),
                              ev.detection_latency_ticks.end());
  std::vector<double> trip_lat(r.trip_latency_ticks.begin(),
                               r.trip_latency_ticks.end());
  bool zl = r.frames_dropped == 0 && r.frames_accepted == r.frames_emitted &&
            r.frames_processed == r.frames_accepted;

  std::string path = outdir + "/detection_eval.json";
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    perror("fopen");
    return 1;
  }
  fprintf(f, "{\n");
  write_machine(f);
  fprintf(f,
          "  \"config\": {\"channels\": %u, \"ticks\": %u, \"seed\": %" PRIu64
          ", \"episodes_per_type\": 12, \"tick_ms_nominal\": %.1f},\n",
          r.channels, r.ticks, seed, kTickMs);
  fprintf(f, "  \"frames\": {\"emitted\": %" PRIu64 ", \"accepted\": %" PRIu64
             ", \"dropped\": %" PRIu64 ", \"processed\": %" PRIu64
             ", \"zero_loss\": %s},\n",
          r.frames_emitted, r.frames_accepted, r.frames_dropped,
          r.frames_processed, zl ? "true" : "false");
  fprintf(f, "  \"per_fault_type\": [\n");
  for (int t = 0; t < kNumFaultTypes; ++t) {
    const TypeEval& te = ev.by_type[t];
    fprintf(f, "    {\"type\": \"%s\", \"episodes\": %u, \"tp\": %u, \"fp\": %u,"
               " \"fn\": %u, ",
            alarm_name(static_cast<AlarmKind>(t)), te.tp + te.fn, te.tp, te.fp,
            te.fn);
    if (te.precision_defined())
      fprintf(f, "\"precision\": %.4f, ", te.precision());
    else
      fprintf(f, "\"precision\": null, ");
    if (te.recall_defined())
      fprintf(f, "\"recall\": %.4f}%s\n", te.recall(),
              t + 1 < kNumFaultTypes ? "," : "");
    else
      fprintf(f, "\"recall\": null}%s\n", t + 1 < kNumFaultTypes ? "," : "");
  }
  fprintf(f, "  ],\n");
  fprintf(f,
          "  \"episodes_total\": %u,\n  \"episodes_detected\": %u,\n"
          "  \"detection_latency\": {\"unit_note\": \"sim ticks, 1 tick = %.0f ms"
          " nominal\", \"p50_ticks\": %.1f, \"p99_ticks\": %.1f, \"p50_ms\": %.1f,"
          " \"p99_ms\": %.1f},\n",
          ev.episodes_total, ev.episodes_detected, kTickMs, pct(det_lat, 0.5),
          pct(det_lat, 0.99), pct(det_lat, 0.5) * kTickMs,
          pct(det_lat, 0.99) * kTickMs);
  fprintf(f,
          "  \"trips\": %u,\n"
          "  \"detection_to_trip_latency\": {\"samples\": %zu, \"p50_ticks\": %.1f,"
          " \"p99_ticks\": %.1f, \"p50_ms\": %.1f, \"p99_ms\": %.1f},\n",
          r.trips, trip_lat.size(), pct(trip_lat, 0.5), pct(trip_lat, 0.99),
          pct(trip_lat, 0.5) * kTickMs, pct(trip_lat, 0.99) * kTickMs);
  fprintf(f, "  \"event_log_entries\": %zu,\n  \"event_log_hash\": \"%016" PRIx64
             "\"\n}\n",
          r.log.size(), r.log_hash);
  fclose(f);
  fprintf(stderr, "wrote %s\n", path.c_str());
  return 0;
}

// --------------------------------------------------------------- replay ----

static int cmd_replay(const std::string& outdir) {
  const uint64_t seed = 20260819;
  PipelineResult a = run_pipeline(eval_config(seed));
  PipelineResult b = run_pipeline(eval_config(seed));
  PipelineResult c = run_pipeline(eval_config(seed + 1));
  bool same = a.log_hash == b.log_hash && a.log.size() == b.log.size();
  bool differs = a.log_hash != c.log_hash;

  std::string path = outdir + "/determinism.json";
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    perror("fopen");
    return 1;
  }
  fprintf(f,
          "{\n  \"seed\": %" PRIu64 ",\n  \"run1_event_log_hash\": \"%016" PRIx64
          "\",\n  \"run2_event_log_hash\": \"%016" PRIx64
          "\",\n  \"run1_events\": %zu,\n  \"run2_events\": %zu,\n"
          "  \"replay_identical\": %s,\n  \"different_seed_hash\": \"%016" PRIx64
          "\",\n  \"different_seed_differs\": %s\n}\n",
          seed, a.log_hash, b.log_hash, a.log.size(), b.log.size(),
          same ? "true" : "false", c.log_hash, differs ? "true" : "false");
  fclose(f);
  fprintf(stderr, "wrote %s (identical=%s)\n", path.c_str(),
          same ? "true" : "false");
  return same && differs ? 0 : 1;
}

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  std::string outdir = "results";
  for (int i = 2; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0) outdir = argv[i + 1];
  }
  if (cmd == "bench") return cmd_bench(outdir);
  if (cmd == "eval") return cmd_eval(outdir);
  if (cmd == "replay") return cmd_replay(outdir);
  fprintf(stderr, "usage: %s {bench|eval|replay} [--out dir]\n", argv[0]);
  return 2;
}
