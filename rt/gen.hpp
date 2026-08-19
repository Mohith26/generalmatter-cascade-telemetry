// Seeded synthetic telemetry generator with injected fault episodes.
//
// All sensor ranges below are made up generic placeholders for a simulator.
// They do not describe any real machine. Every sample is a pure function of
// (seed, channel, tick), so generation is deterministic no matter how work is
// sharded across producer threads.
#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "frame.hpp"
#include "util.hpp"

namespace rt {

struct SensorModel {
  float rpm_nominal = 10000.0f;
  float rpm_noise = 15.0f;
  float vib_nominal = 1.0f;
  float vib_noise = 0.08f;
  float temp_nominal = 320.0f;
  float temp_noise = 0.3f;
  float cur_nominal = 40.0f;
  float cur_noise = 0.5f;
  // Fault shapes.
  float overspeed_frac = 0.08f;        // ramp target, fraction above nominal
  uint32_t overspeed_ramp_ticks = 30;  // ticks to reach the ramp target
  float vib_spike_min = 0.5f;          // added spike amplitude, low end
  float vib_spike_max = 3.5f;          // added spike amplitude, high end
  float thermal_drift_per_tick = 0.2f;
};

struct GenConfig {
  uint32_t channels = 64;
  uint32_t ticks = 1000;
  uint64_t seed = 1;
  uint32_t episodes_per_type = 0;  // 0 disables fault injection
  uint32_t min_episode_ticks = 20;
  uint32_t max_episode_ticks = 120;
  SensorModel model;
};

class Generator {
 public:
  explicit Generator(const GenConfig& cfg) : cfg_(cfg), by_channel_(cfg.channels) {
    if (cfg_.channels == 0 || cfg_.ticks == 0) {
      throw std::invalid_argument("channels and ticks must be positive");
    }
    plan_episodes();
  }

  const GenConfig& config() const { return cfg_; }
  const std::vector<Episode>& episodes() const { return episodes_; }

  // Fills f and returns true, or returns false when the frame is suppressed
  // by an active sensor dropout episode.
  bool make_frame(uint32_t ch, uint32_t tick, Frame& f) const {
    const Episode* ep = active_episode(ch, tick);
    if (ep && ep->type == Fault::Dropout) return false;
    const SensorModel& m = cfg_.model;
    f.channel = ch;
    f.tick = tick;
    f.emit_ns = 0;
    f.rpm = m.rpm_nominal + m.rpm_noise * static_cast<float>(gauss(mix4(cfg_.seed, ch, tick, 1)));
    f.vib = m.vib_nominal + m.vib_noise * static_cast<float>(gauss(mix4(cfg_.seed, ch, tick, 2)));
    f.temp = m.temp_nominal + m.temp_noise * static_cast<float>(gauss(mix4(cfg_.seed, ch, tick, 3)));
    f.cur = m.cur_nominal + m.cur_noise * static_cast<float>(gauss(mix4(cfg_.seed, ch, tick, 4)));
    if (ep) {
      uint32_t dt = tick - ep->start;
      switch (ep->type) {
        case Fault::Overspeed: {
          float ramp = std::min(1.0f, static_cast<float>(dt) /
                                          static_cast<float>(m.overspeed_ramp_ticks));
          f.rpm += m.rpm_nominal * m.overspeed_frac * ramp;
          break;
        }
        case Fault::VibSpike: {
          // Per episode amplitude. Some spikes land below the alarm
          // threshold on purpose so the detector has genuinely hard cases.
          double u = u01(mix4(cfg_.seed, ch, ep->start, 77));
          f.vib += m.vib_spike_min +
                   static_cast<float>(u) * (m.vib_spike_max - m.vib_spike_min);
          break;
        }
        case Fault::ThermalDrift:
          f.temp += m.thermal_drift_per_tick * static_cast<float>(dt);
          break;
        default:
          break;
      }
    }
    return true;
  }

 private:
  const Episode* active_episode(uint32_t ch, uint32_t tick) const {
    for (const Episode& e : by_channel_[ch]) {
      if (tick >= e.start && tick <= e.end) return &e;
    }
    return nullptr;
  }

  void plan_episodes() {
    if (cfg_.episodes_per_type == 0) return;
    if (cfg_.ticks < cfg_.max_episode_ticks + 200) {
      throw std::invalid_argument("ticks too small for the episode plan");
    }
    uint64_t s = cfg_.seed ^ 0xE19A5EEDCAFEF00DULL;
    auto next = [&s]() {
      s = splitmix64(s);
      return s;
    };
    const Fault types[kNumFaultTypes] = {Fault::Overspeed, Fault::VibSpike,
                                         Fault::ThermalDrift, Fault::Dropout};
    const uint32_t margin = 40;  // keep same channel episodes apart
    for (Fault t : types) {
      for (uint32_t i = 0; i < cfg_.episodes_per_type; ++i) {
        for (int attempt = 0; attempt < 400; ++attempt) {
          uint32_t ch = static_cast<uint32_t>(next() % cfg_.channels);
          uint32_t dur = cfg_.min_episode_ticks +
                         static_cast<uint32_t>(next() % (cfg_.max_episode_ticks -
                                                         cfg_.min_episode_ticks + 1));
          uint32_t lo = 50;
          uint32_t hi = cfg_.ticks - dur - 100;
          uint32_t start = lo + static_cast<uint32_t>(next() % (hi - lo));
          Episode cand{ch, t, start, start + dur - 1};
          bool clash = false;
          for (const Episode& e : by_channel_[ch]) {
            if (cand.start <= e.end + margin && cand.end + margin >= e.start) {
              clash = true;
              break;
            }
          }
          if (!clash) {
            episodes_.push_back(cand);
            by_channel_[ch].push_back(cand);
            break;
          }
        }
      }
    }
  }

  GenConfig cfg_;
  std::vector<Episode> episodes_;
  std::vector<std::vector<Episode>> by_channel_;
};

}  // namespace rt
