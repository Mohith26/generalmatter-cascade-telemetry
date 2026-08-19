// End to end in process pipeline:
//   producer threads (sharded channels) -> bounded MPSC queue -> single
//   consumer running detection and the cascade trip state machine.
//
// Determinism: sensor samples are pure functions of (seed, channel, tick),
// per channel frame order is preserved because each channel lives on exactly
// one producer, and global decisions (trip machine, dropout scan) only run
// at tick finalization on canonically sorted events. The event log hash is
// therefore identical across runs and thread interleavings. Wall clock
// latency samples are measured but never hashed.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "detect.hpp"
#include "frame.hpp"
#include "gen.hpp"
#include "queue.hpp"
#include "trip.hpp"

namespace rt {

struct PipelineConfig {
  GenConfig gen;
  DetectConfig det;
  TripConfig trip;
  uint32_t producers = 4;
  size_t queue_capacity = 1 << 16;
  double paced_tick_hz = 0.0;  // 0 means run unpaced at maximum rate
  bool drop_on_full = false;   // true uses try_push (lossy backpressure)
  uint32_t latency_sample_mask = 7;  // sample when (channel + tick) & mask == 0
};

struct PipelineResult {
  uint64_t frames_emitted = 0;
  uint64_t frames_accepted = 0;
  uint64_t frames_dropped = 0;
  uint64_t frames_processed = 0;
  double wall_seconds = 0.0;
  double frames_per_sec = 0.0;
  std::vector<double> latency_ns;  // sampled ingest to detection latency
  std::vector<LogEvent> log;
  uint64_t log_hash = 0;
  uint32_t trips = 0;
  std::vector<uint32_t> trip_latency_ticks;
  std::vector<Episode> episodes;
  uint32_t channels = 0;
  uint32_t ticks = 0;
  SysState final_state = SysState::Normal;
};

inline PipelineResult run_pipeline(const PipelineConfig& cfg) {
  using clock = std::chrono::steady_clock;
  const uint32_t channels = cfg.gen.channels;
  const uint32_t ticks = cfg.gen.ticks;
  const uint32_t nprod = cfg.producers;
  const uint32_t chunk = (channels + nprod - 1) / nprod;

  Generator gen(cfg.gen);
  BoundedQueue<Frame> q(cfg.queue_capacity);
  std::atomic<uint64_t> emitted{0};

  auto t0 = clock::now();

  std::vector<std::thread> producers;
  producers.reserve(nprod);
  for (uint32_t p = 0; p < nprod; ++p) {
    producers.emplace_back([&, p] {
      const uint32_t lo = p * chunk;
      const uint32_t hi = std::min(channels, lo + chunk);
      if (lo >= hi) return;
      std::vector<Frame> batch;
      batch.reserve(hi - lo);
      uint64_t local_emitted = 0;
      const bool paced = cfg.paced_tick_hz > 0.0;
      const auto period = paced
          ? std::chrono::nanoseconds(static_cast<int64_t>(1e9 / cfg.paced_tick_hz))
          : std::chrono::nanoseconds(0);
      const auto start = clock::now();
      Frame f;
      for (uint32_t tick = 0; tick < ticks; ++tick) {
        batch.clear();
        for (uint32_t ch = lo; ch < hi; ++ch) {
          if (!gen.make_frame(ch, tick, f)) continue;  // dropout episode
          if (((ch + tick) & cfg.latency_sample_mask) == 0) {
            f.emit_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    clock::now().time_since_epoch())
                    .count());
          }
          batch.push_back(f);
        }
        local_emitted += batch.size();
        if (cfg.drop_on_full) {
          q.try_push_batch(batch.data(), batch.size());
        } else {
          q.push_batch(batch.data(), batch.size());
        }
        if (paced) std::this_thread::sleep_until(start + (tick + 1) * period);
      }
      emitted.fetch_add(local_emitted, std::memory_order_relaxed);
    });
  }

  // Consumer: detection, tick finalization, trip state machine.
  PipelineResult res;
  res.channels = channels;
  res.ticks = ticks;
  res.episodes = gen.episodes();

  Detector det(channels, cfg.det);
  TripSM trip(channels, cfg.trip);
  const bool dropout_scan = cfg.det.dropout_gap > 0;

  std::thread consumer([&] {
    std::vector<std::vector<AlarmEvent>> pending(ticks);
    std::vector<std::vector<uint32_t>> seen;
    if (dropout_scan) seen.resize(ticks);
    std::vector<int64_t> shard_last(nprod, -1);
    int64_t finalized = -1;  // highest finalized tick

    auto finalize_through = [&](int64_t upto) {
      for (int64_t u = finalized + 1; u <= upto; ++u) {
        auto& ev = pending[static_cast<size_t>(u)];
        if (dropout_scan) {
          for (uint32_t ch : seen[static_cast<size_t>(u)]) {
            det.note_seen(ch, static_cast<uint32_t>(u), ev);
          }
          seen[static_cast<size_t>(u)].clear();
          seen[static_cast<size_t>(u)].shrink_to_fit();
          det.check_dropouts(static_cast<uint32_t>(u), ev);
        }
        std::sort(ev.begin(), ev.end(), alarm_event_less);
        trip.on_tick(static_cast<uint32_t>(u), ev.data(), ev.size());
        ev.clear();
        ev.shrink_to_fit();
      }
      finalized = std::max(finalized, upto);
    };

    std::vector<Frame> buf;
    buf.reserve(4096);
    uint64_t processed = 0;
    std::vector<double> lat;
    while (true) {
      buf.clear();
      size_t n = q.pop_batch(buf, 4096);
      if (n == 0) break;  // closed and drained
      uint64_t now_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              clock::now().time_since_epoch())
              .count());
      for (const Frame& f : buf) {
        if (f.emit_ns != 0) {
          lat.push_back(static_cast<double>(now_ns - f.emit_ns));
        }
        det.process(f, pending[f.tick]);
        if (dropout_scan) seen[f.tick].push_back(f.channel);
        uint32_t shard = f.channel / chunk;
        if (static_cast<int64_t>(f.tick) > shard_last[shard]) {
          shard_last[shard] = static_cast<int64_t>(f.tick);
        }
        processed++;
      }
      int64_t wm = INT64_MAX;
      for (uint32_t p = 0; p < nprod; ++p) {
        if (p * chunk >= channels) break;  // idle shard, no channels
        wm = std::min(wm, shard_last[p]);
      }
      if (wm != INT64_MAX) finalize_through(wm - 1);
    }
    finalize_through(static_cast<int64_t>(ticks) - 1);
    res.frames_processed = processed;
    res.latency_ns = std::move(lat);
  });

  for (auto& t : producers) t.join();
  q.close();
  consumer.join();
  auto t1 = clock::now();

  res.frames_emitted = emitted.load();
  res.frames_accepted = q.accepted();
  res.frames_dropped = q.dropped();
  res.wall_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
  res.frames_per_sec =
      res.wall_seconds > 0 ? static_cast<double>(res.frames_processed) / res.wall_seconds
                           : 0.0;
  res.log = trip.log();
  res.log_hash = trip.log_hash();
  res.trips = trip.trips();
  res.trip_latency_ticks = trip.trip_latency_ticks();
  res.final_state = trip.state();
  return res;
}

}  // namespace rt
