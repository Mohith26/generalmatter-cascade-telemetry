// Assert based test harness for the C++ telemetry and protection layer.
// Run via: make test

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "eval.hpp"
#include "pipeline.hpp"

using namespace rt;

static int g_checks = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      std::printf("  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,   \
                  #cond);                                                \
      return false;                                                      \
    }                                                                    \
  } while (0)

// ---------------------------------------------------------------- queue ----

static bool queue_fifo_single_thread() {
  BoundedQueue<int> q(8);
  for (int i = 0; i < 5; ++i) CHECK(q.push(i));
  std::vector<int> out;
  CHECK(q.pop_batch(out, 16) == 5);
  for (int i = 0; i < 5; ++i) CHECK(out[static_cast<size_t>(i)] == i);
  CHECK(q.accepted() == 5);
  CHECK(q.dropped() == 0);
  return true;
}

static bool queue_try_push_drops_when_full() {
  BoundedQueue<int> q(4);
  int items[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  size_t acc = q.try_push_batch(items, 10);
  CHECK(acc == 4);
  CHECK(q.accepted() == 4);
  CHECK(q.dropped() == 6);
  return true;
}

static bool queue_counters_account_every_attempt() {
  BoundedQueue<int> q(3);
  int x = 7;
  for (int i = 0; i < 10; ++i) q.try_push(x);
  CHECK(q.accepted() + q.dropped() == 10);
  CHECK(q.accepted() == 3);
  return true;
}

static bool queue_blocking_push_never_drops() {
  BoundedQueue<int> q(4);
  const int n = 5000;
  std::thread prod([&] {
    for (int i = 0; i < n; ++i) q.push(i);
    q.close();
  });
  std::vector<int> out;
  std::vector<int> got;
  while (true) {
    out.clear();
    if (q.pop_batch(out, 64) == 0) break;
    got.insert(got.end(), out.begin(), out.end());
  }
  prod.join();
  CHECK(got.size() == n);
  CHECK(q.dropped() == 0);
  CHECK(q.accepted() == n);
  for (int i = 0; i < n; ++i) CHECK(got[static_cast<size_t>(i)] == i);
  return true;
}

static bool queue_mpsc_contention_totals() {
  BoundedQueue<uint64_t> q(256);
  const int nprod = 4, per = 50000;
  std::vector<std::thread> ts;
  for (int p = 0; p < nprod; ++p) {
    ts.emplace_back([&q, p] {
      for (int i = 0; i < per; ++i) {
        q.push((static_cast<uint64_t>(p) << 32) | static_cast<uint64_t>(i));
      }
    });
  }
  uint64_t popped = 0;
  uint64_t sum_lo = 0;
  std::thread cons([&] {
    std::vector<uint64_t> out;
    while (popped < static_cast<uint64_t>(nprod) * per) {
      out.clear();
      size_t k = q.pop_batch(out, 128);
      popped += k;
      for (uint64_t v : out) sum_lo += (v & 0xffffffffULL);
    }
  });
  for (auto& t : ts) t.join();
  cons.join();
  CHECK(popped == static_cast<uint64_t>(nprod) * per);
  CHECK(q.accepted() == popped);
  CHECK(q.dropped() == 0);
  uint64_t expect = static_cast<uint64_t>(nprod) * (static_cast<uint64_t>(per) *
                                                    (per - 1) / 2);
  CHECK(sum_lo == expect);
  return true;
}

static bool queue_preserves_per_producer_order() {
  BoundedQueue<uint64_t> q(128);
  const int nprod = 3, per = 20000;
  std::vector<std::thread> ts;
  for (int p = 0; p < nprod; ++p) {
    ts.emplace_back([&q, p] {
      for (int i = 0; i < per; ++i) {
        q.push((static_cast<uint64_t>(p) << 32) | static_cast<uint64_t>(i));
      }
    });
  }
  std::vector<int64_t> last(nprod, -1);
  uint64_t popped = 0;
  bool ok = true;
  std::thread cons([&] {
    std::vector<uint64_t> out;
    while (popped < static_cast<uint64_t>(nprod) * per) {
      out.clear();
      size_t k = q.pop_batch(out, 64);
      popped += k;
      for (uint64_t v : out) {
        int p = static_cast<int>(v >> 32);
        int64_t seq = static_cast<int64_t>(v & 0xffffffffULL);
        if (seq <= last[static_cast<size_t>(p)]) ok = false;
        last[static_cast<size_t>(p)] = seq;
      }
    }
  });
  for (auto& t : ts) t.join();
  cons.join();
  CHECK(ok);
  for (int p = 0; p < nprod; ++p) CHECK(last[static_cast<size_t>(p)] == per - 1);
  return true;
}

static bool queue_close_unblocks_consumer() {
  BoundedQueue<int> q(4);
  std::atomic<bool> done{false};
  std::thread cons([&] {
    std::vector<int> out;
    size_t k = q.pop_batch(out, 8);
    if (k == 0) done = true;
  });
  q.close();
  cons.join();
  CHECK(done.load());
  return true;
}

static bool queue_batch_roundtrip() {
  BoundedQueue<int> q(16);
  int items[10];
  for (int i = 0; i < 10; ++i) items[i] = i * 3;
  CHECK(q.push_batch(items, 10));
  std::vector<int> out;
  CHECK(q.pop_batch(out, 4) == 4);
  CHECK(q.pop_batch(out, 100) == 6);
  for (int i = 0; i < 10; ++i) CHECK(out[static_cast<size_t>(i)] == i * 3);
  return true;
}

// ------------------------------------------------------------ generator ----

static GenConfig small_gen(uint32_t epp) {
  GenConfig g;
  g.channels = 16;
  g.ticks = 2000;
  g.seed = 42;
  g.episodes_per_type = epp;
  return g;
}

static bool generator_is_deterministic() {
  Generator a(small_gen(3)), b(small_gen(3));
  CHECK(a.episodes().size() == b.episodes().size());
  for (size_t i = 0; i < a.episodes().size(); ++i) {
    CHECK(a.episodes()[i].channel == b.episodes()[i].channel);
    CHECK(a.episodes()[i].start == b.episodes()[i].start);
  }
  Frame fa, fb;
  for (uint32_t ch = 0; ch < 16; ch += 5) {
    for (uint32_t t = 0; t < 2000; t += 137) {
      bool ea = a.make_frame(ch, t, fa);
      bool eb = b.make_frame(ch, t, fb);
      CHECK(ea == eb);
      if (ea) {
        CHECK(fa.rpm == fb.rpm && fa.vib == fb.vib && fa.temp == fb.temp &&
              fa.cur == fb.cur);
      }
    }
  }
  return true;
}

static bool generator_dropout_suppresses_frames() {
  Generator g(small_gen(3));
  const Episode* drop = nullptr;
  for (const Episode& e : g.episodes()) {
    if (e.type == Fault::Dropout) {
      drop = &e;
      break;
    }
  }
  CHECK(drop != nullptr);
  Frame f;
  CHECK(!g.make_frame(drop->channel, drop->start, f));
  CHECK(!g.make_frame(drop->channel, drop->end, f));
  CHECK(g.make_frame(drop->channel, drop->end + 1, f));
  return true;
}

static bool generator_fault_effects_visible() {
  GenConfig with = small_gen(3);
  GenConfig without = small_gen(0);
  Generator gf(with), gc(without);
  Frame ff, fc;
  int checked = 0;
  for (const Episode& e : gf.episodes()) {
    uint32_t t = std::min(e.end, e.start + 30);
    if (e.type == Fault::Dropout) continue;
    CHECK(gf.make_frame(e.channel, t, ff));
    CHECK(gc.make_frame(e.channel, t, fc));
    if (e.type == Fault::Overspeed) CHECK(ff.rpm - fc.rpm > 500.0f);
    if (e.type == Fault::VibSpike) CHECK(ff.vib - fc.vib >= 0.5f - 1e-4f);
    if (e.type == Fault::ThermalDrift) CHECK(ff.temp - fc.temp > 1.5f);
    checked++;
  }
  CHECK(checked >= 6);
  return true;
}

static bool generator_episodes_do_not_overlap_per_channel() {
  Generator g(small_gen(4));
  const auto& eps = g.episodes();
  for (size_t i = 0; i < eps.size(); ++i) {
    for (size_t j = i + 1; j < eps.size(); ++j) {
      if (eps[i].channel != eps[j].channel) continue;
      bool disjoint = eps[i].end < eps[j].start || eps[j].end < eps[i].start;
      CHECK(disjoint);
    }
  }
  return true;
}

// ------------------------------------------------------------- detector ----

static Frame mk(uint32_t ch, uint32_t tick, float rpm, float vib, float temp) {
  Frame f;
  f.channel = ch;
  f.tick = tick;
  f.emit_ns = 0;
  f.rpm = rpm;
  f.vib = vib;
  f.temp = temp;
  f.cur = 40.0f;
  return f;
}

static bool detector_raises_overspeed_after_streak() {
  Detector d(2, DetectConfig{});
  std::vector<AlarmEvent> ev;
  d.process(mk(0, 0, 10500, 1, 320), ev);
  d.process(mk(0, 1, 10500, 1, 320), ev);
  CHECK(ev.empty());
  d.process(mk(0, 2, 10500, 1, 320), ev);
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == AlarmKind::Overspeed && ev[0].raised && ev[0].tick == 2);
  return true;
}

static bool detector_hysteresis_ignores_short_excursion() {
  Detector d(1, DetectConfig{});
  std::vector<AlarmEvent> ev;
  d.process(mk(0, 0, 10500, 1, 320), ev);
  d.process(mk(0, 1, 10500, 1, 320), ev);
  for (uint32_t t = 2; t < 20; ++t) d.process(mk(0, t, 10000, 1, 320), ev);
  CHECK(ev.empty());
  return true;
}

static bool detector_clears_after_good_streak() {
  DetectConfig c;
  Detector d(1, c);
  std::vector<AlarmEvent> ev;
  for (uint32_t t = 0; t < 5; ++t) d.process(mk(0, t, 10500, 1, 320), ev);
  CHECK(ev.size() == 1 && ev[0].raised);
  for (uint32_t t = 5; t < 5 + c.clear_streak; ++t)
    d.process(mk(0, t, 10000, 1, 320), ev);
  CHECK(ev.size() == 2);
  CHECK(!ev[1].raised && ev[1].kind == AlarmKind::Overspeed);
  return true;
}

static bool detector_vibration_threshold() {
  Detector d(1, DetectConfig{});
  std::vector<AlarmEvent> ev;
  for (uint32_t t = 0; t < 3; ++t) d.process(mk(0, t, 10000, 3.5f, 320), ev);
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == AlarmKind::VibSpike && ev[0].raised);
  return true;
}

static bool detector_thermal_rate_of_change() {
  DetectConfig c;
  Detector d(1, c);
  std::vector<AlarmEvent> ev;
  // Rising 0.5 per frame: window delta 8.0 exceeds the 1.5 limit once the
  // window is full.
  uint32_t t = 0;
  for (; t < 25; ++t) d.process(mk(0, t, 10000, 1, 320.0f + 0.5f * t), ev);
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == AlarmKind::ThermalDrift && ev[0].raised);
  // Plateau: rate of change decays to zero, alarm clears.
  for (; t < 70; ++t) d.process(mk(0, t, 10000, 1, 332.0f), ev);
  CHECK(ev.size() == 2);
  CHECK(ev[1].kind == AlarmKind::ThermalDrift && !ev[1].raised);
  return true;
}

static bool detector_dropout_gap_detection() {
  DetectConfig c;
  Detector d(2, c);
  std::vector<AlarmEvent> ev;
  for (uint32_t t = 0; t <= 10; ++t) {
    d.note_seen(0, t, ev);
    d.note_seen(1, t, ev);
    d.check_dropouts(t, ev);
  }
  CHECK(ev.empty());
  // Channel 0 goes silent, channel 1 keeps reporting.
  for (uint32_t t = 11; t <= 19; ++t) {
    d.note_seen(1, t, ev);
    d.check_dropouts(t, ev);
  }
  CHECK(ev.size() == 1);
  CHECK(ev[0].kind == AlarmKind::Dropout && ev[0].raised && ev[0].channel == 0);
  CHECK(ev[0].tick == 19);
  d.note_seen(0, 25, ev);
  CHECK(ev.size() == 2);
  CHECK(!ev[1].raised && ev[1].channel == 0);
  return true;
}

static bool detector_quiet_on_nominal_noise() {
  Generator g(small_gen(0));
  Detector d(16, DetectConfig{});
  std::vector<AlarmEvent> ev;
  Frame f;
  for (uint32_t t = 0; t < 500; ++t) {
    for (uint32_t ch = 0; ch < 16; ++ch) {
      CHECK(g.make_frame(ch, t, f));
      d.process(f, ev);
    }
  }
  CHECK(ev.empty());
  return true;
}

// ---------------------------------------------------------------- trip -----

static AlarmEvent ae(uint32_t tick, uint32_t ch, AlarmKind k, bool raised) {
  return AlarmEvent{tick, ch, k, raised};
}

static bool trip_normal_to_alarm_on_raise() {
  TripSM sm(4, TripConfig{});
  AlarmEvent e = ae(0, 0, AlarmKind::Overspeed, true);
  sm.on_tick(0, &e, 1);
  CHECK(sm.state() == SysState::Alarm);
  CHECK(sm.trips() == 0);
  return true;
}

static bool trip_alarm_clears_with_hysteresis() {
  TripConfig c;
  TripSM sm(4, c);
  AlarmEvent r = ae(0, 0, AlarmKind::Overspeed, true);
  sm.on_tick(0, &r, 1);
  AlarmEvent cl = ae(2, 0, AlarmKind::Overspeed, false);
  sm.on_tick(1, nullptr, 0);
  sm.on_tick(2, &cl, 1);
  for (uint32_t t = 3; t < 13; ++t) {
    sm.on_tick(t, nullptr, 0);
    CHECK(sm.state() == SysState::Alarm);  // still inside the clear window
  }
  sm.on_tick(13, nullptr, 0);
  CHECK(sm.state() == SysState::Normal);
  return true;
}

static bool trip_realarm_within_window_stays_alarmed() {
  TripConfig c;
  TripSM sm(4, c);
  AlarmEvent r = ae(0, 0, AlarmKind::VibSpike, true);
  sm.on_tick(0, &r, 1);
  AlarmEvent cl = ae(1, 0, AlarmKind::VibSpike, false);
  sm.on_tick(1, &cl, 1);
  AlarmEvent r2 = ae(5, 0, AlarmKind::VibSpike, true);
  sm.on_tick(5, &r2, 1);
  for (uint32_t t = 6; t < 9; ++t) sm.on_tick(t, nullptr, 0);
  CHECK(sm.state() != SysState::Normal);
  return true;
}

static bool trip_persistent_alarm_escalates() {
  TripConfig c;
  TripSM sm(4, c);
  AlarmEvent r = ae(0, 0, AlarmKind::Overspeed, true);
  sm.on_tick(0, &r, 1);
  for (uint32_t t = 1; t < c.persist_ticks; ++t) {
    sm.on_tick(t, nullptr, 0);
    CHECK(sm.state() == SysState::Alarm);
  }
  sm.on_tick(c.persist_ticks, nullptr, 0);
  CHECK(sm.state() == SysState::Trip);
  CHECK(sm.trips() == 1);
  CHECK(sm.trip_latency_ticks().size() == 1);
  CHECK(sm.trip_latency_ticks()[0] == c.persist_ticks);
  return true;
}

static bool trip_multi_channel_escalates_next_tick() {
  TripSM sm(8, TripConfig{});
  AlarmEvent evs[2] = {ae(0, 1, AlarmKind::Overspeed, true),
                       ae(0, 5, AlarmKind::VibSpike, true)};
  sm.on_tick(0, evs, 2);
  CHECK(sm.state() == SysState::Alarm);
  sm.on_tick(1, nullptr, 0);
  CHECK(sm.state() == SysState::Trip);
  CHECK(sm.trip_latency_ticks()[0] == 1);
  return true;
}

static bool trip_quiesces_all_channels() {
  TripSM sm(32, TripConfig{});
  AlarmEvent evs[2] = {ae(0, 0, AlarmKind::Dropout, true),
                       ae(0, 9, AlarmKind::Overspeed, true)};
  sm.on_tick(0, evs, 2);
  sm.on_tick(1, nullptr, 0);
  CHECK(sm.state() == SysState::Trip);
  CHECK(sm.last_quiesce_count() == 32);
  CHECK(sm.all_quiesced());
  return true;
}

static bool trip_auto_reset_after_cooldown() {
  TripConfig c;
  TripSM sm(4, c);
  AlarmEvent r = ae(0, 0, AlarmKind::Overspeed, true);
  sm.on_tick(0, &r, 1);
  for (uint32_t t = 1; t <= c.persist_ticks; ++t) sm.on_tick(t, nullptr, 0);
  CHECK(sm.state() == SysState::Trip);
  uint32_t trip_tick = c.persist_ticks;
  AlarmEvent cl = ae(trip_tick + 1, 0, AlarmKind::Overspeed, false);
  sm.on_tick(trip_tick + 1, &cl, 1);
  for (uint32_t t = trip_tick + 2; t < trip_tick + c.reset_ticks; ++t)
    sm.on_tick(t, nullptr, 0);
  CHECK(sm.state() == SysState::Trip);
  sm.on_tick(trip_tick + c.reset_ticks, nullptr, 0);
  CHECK(sm.state() == SysState::Normal);
  bool saw_reset = false;
  for (const LogEvent& e : sm.log())
    if (e.type == LogType::Reset) saw_reset = true;
  CHECK(saw_reset);
  return true;
}

// ------------------------------------------------------------- pipeline ----

static bool pipeline_zero_loss_blocking() {
  PipelineConfig cfg;
  cfg.gen.channels = 16;
  cfg.gen.ticks = 500;
  cfg.gen.seed = 5;
  cfg.gen.episodes_per_type = 0;
  cfg.det.dropout_gap = 0;
  cfg.producers = 3;
  cfg.queue_capacity = 64;
  PipelineResult r = run_pipeline(cfg);
  CHECK(r.frames_emitted == 16ull * 500ull);
  CHECK(r.frames_accepted == r.frames_emitted);
  CHECK(r.frames_dropped == 0);
  CHECK(r.frames_processed == r.frames_accepted);
  return true;
}

static PipelineConfig determinism_cfg(uint64_t seed) {
  PipelineConfig cfg;
  cfg.gen.channels = 32;
  cfg.gen.ticks = 3000;
  cfg.gen.seed = seed;
  cfg.gen.episodes_per_type = 4;
  cfg.producers = 4;
  cfg.queue_capacity = 512;
  return cfg;
}

static bool pipeline_replay_hash_identical() {
  PipelineResult a = run_pipeline(determinism_cfg(7));
  PipelineResult b = run_pipeline(determinism_cfg(7));
  CHECK(!a.log.empty());
  CHECK(a.log_hash == b.log_hash);
  CHECK(a.log.size() == b.log.size());
  CHECK(a.trips == b.trips);
  CHECK(a.trips >= 1);
  PipelineResult c = run_pipeline(determinism_cfg(8));
  CHECK(c.log_hash != a.log_hash);
  return true;
}

static bool pipeline_backpressure_drop_mode() {
  PipelineConfig cfg;
  cfg.gen.channels = 512;
  cfg.gen.ticks = 200;
  cfg.gen.seed = 11;
  cfg.gen.episodes_per_type = 0;
  cfg.det.dropout_gap = 0;
  cfg.producers = 4;
  cfg.queue_capacity = 16;  // deliberately starved: per tick batches are 128
  cfg.drop_on_full = true;
  PipelineResult r = run_pipeline(cfg);
  CHECK(r.frames_dropped > 0);
  CHECK(r.frames_accepted + r.frames_dropped == r.frames_emitted);
  CHECK(r.frames_processed == r.frames_accepted);
  return true;
}

static bool eval_scores_hand_built_case() {
  std::vector<Episode> eps = {
      {0, Fault::Overspeed, 100, 150},
      {1, Fault::VibSpike, 200, 240},
      {2, Fault::ThermalDrift, 300, 360},
  };
  std::vector<LogEvent> log = {
      {110, LogType::AlarmRaised, 0, static_cast<uint8_t>(AlarmKind::Overspeed), 0},
      {160, LogType::AlarmCleared, 0, static_cast<uint8_t>(AlarmKind::Overspeed), 0},
      {205, LogType::AlarmRaised, 1, static_cast<uint8_t>(AlarmKind::VibSpike), 0},
      {245, LogType::AlarmCleared, 1, static_cast<uint8_t>(AlarmKind::VibSpike), 0},
      {500, LogType::AlarmRaised, 5, static_cast<uint8_t>(AlarmKind::VibSpike), 0},
      {520, LogType::AlarmCleared, 5, static_cast<uint8_t>(AlarmKind::VibSpike), 0},
  };
  EvalResult r = evaluate(eps, build_alarm_intervals(log, 999));
  const TypeEval& over = r.by_type[static_cast<int>(AlarmKind::Overspeed)];
  const TypeEval& vib = r.by_type[static_cast<int>(AlarmKind::VibSpike)];
  const TypeEval& th = r.by_type[static_cast<int>(AlarmKind::ThermalDrift)];
  const TypeEval& dr = r.by_type[static_cast<int>(AlarmKind::Dropout)];
  CHECK(over.tp == 1 && over.fp == 0 && over.fn == 0);
  CHECK(over.precision() == 1.0 && over.recall() == 1.0);
  CHECK(vib.tp == 1 && vib.fp == 1 && vib.fn == 0);
  CHECK(vib.precision() == 0.5 && vib.recall() == 1.0);
  CHECK(th.tp == 0 && th.fn == 1);
  CHECK(!th.precision_defined());
  CHECK(!dr.precision_defined() && !dr.recall_defined());
  CHECK(r.detection_latency_ticks.size() == 2);
  CHECK(r.detection_latency_ticks[0] == 10);
  CHECK(r.detection_latency_ticks[1] == 5);
  return true;
}

static bool eval_end_to_end_small_run() {
  PipelineResult r = run_pipeline(determinism_cfg(7));
  EvalResult ev = evaluate(r.episodes, build_alarm_intervals(r.log, r.ticks - 1));
  CHECK(ev.episodes_total == 16);
  CHECK(ev.episodes_detected >= 8);
  CHECK(ev.by_type[static_cast<int>(AlarmKind::Overspeed)].recall() > 0.5);
  return true;
}

// ----------------------------------------------------------------- main ----

struct TestCase {
  const char* name;
  bool (*fn)();
};

int main() {
  const TestCase tests[] = {
      {"queue_fifo_single_thread", queue_fifo_single_thread},
      {"queue_try_push_drops_when_full", queue_try_push_drops_when_full},
      {"queue_counters_account_every_attempt", queue_counters_account_every_attempt},
      {"queue_blocking_push_never_drops", queue_blocking_push_never_drops},
      {"queue_mpsc_contention_totals", queue_mpsc_contention_totals},
      {"queue_preserves_per_producer_order", queue_preserves_per_producer_order},
      {"queue_close_unblocks_consumer", queue_close_unblocks_consumer},
      {"queue_batch_roundtrip", queue_batch_roundtrip},
      {"generator_is_deterministic", generator_is_deterministic},
      {"generator_dropout_suppresses_frames", generator_dropout_suppresses_frames},
      {"generator_fault_effects_visible", generator_fault_effects_visible},
      {"generator_episodes_do_not_overlap_per_channel",
       generator_episodes_do_not_overlap_per_channel},
      {"detector_raises_overspeed_after_streak", detector_raises_overspeed_after_streak},
      {"detector_hysteresis_ignores_short_excursion",
       detector_hysteresis_ignores_short_excursion},
      {"detector_clears_after_good_streak", detector_clears_after_good_streak},
      {"detector_vibration_threshold", detector_vibration_threshold},
      {"detector_thermal_rate_of_change", detector_thermal_rate_of_change},
      {"detector_dropout_gap_detection", detector_dropout_gap_detection},
      {"detector_quiet_on_nominal_noise", detector_quiet_on_nominal_noise},
      {"trip_normal_to_alarm_on_raise", trip_normal_to_alarm_on_raise},
      {"trip_alarm_clears_with_hysteresis", trip_alarm_clears_with_hysteresis},
      {"trip_realarm_within_window_stays_alarmed",
       trip_realarm_within_window_stays_alarmed},
      {"trip_persistent_alarm_escalates", trip_persistent_alarm_escalates},
      {"trip_multi_channel_escalates_next_tick", trip_multi_channel_escalates_next_tick},
      {"trip_quiesces_all_channels", trip_quiesces_all_channels},
      {"trip_auto_reset_after_cooldown", trip_auto_reset_after_cooldown},
      {"pipeline_zero_loss_blocking", pipeline_zero_loss_blocking},
      {"pipeline_replay_hash_identical", pipeline_replay_hash_identical},
      {"pipeline_backpressure_drop_mode", pipeline_backpressure_drop_mode},
      {"eval_scores_hand_built_case", eval_scores_hand_built_case},
      {"eval_end_to_end_small_run", eval_end_to_end_small_run},
  };
  int passed = 0, failed = 0;
  for (const TestCase& t : tests) {
    bool ok = t.fn();
    std::printf("%-50s %s\n", t.name, ok ? "PASS" : "FAIL");
    ok ? ++passed : ++failed;
  }
  std::printf("\n%d/%d C++ tests passed (%d assertions)\n", passed,
              passed + failed, g_checks);
  return failed == 0 ? 0 : 1;
}
