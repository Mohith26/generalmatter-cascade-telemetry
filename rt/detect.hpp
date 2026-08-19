// Per channel rolling statistics and alarm detection with hysteresis.
//
// Threshold alarms (overspeed, vibration) fire after raise_streak
// consecutive bad frames and clear after clear_streak consecutive good
// frames. Thermal drift uses a rolling rate of change window. Sensor dropout
// is detected on tick watermarks: the consumer tells the detector which
// channels were seen at each finalized tick, so the decision depends only on
// the frame history and stays deterministic under any thread interleaving.
#pragma once

#include <cstdint>
#include <vector>

#include "frame.hpp"

namespace rt {

enum class AlarmKind : uint8_t {
  Overspeed = 0,
  VibSpike = 1,
  ThermalDrift = 2,
  Dropout = 3,
};

inline const char* alarm_name(AlarmKind k) {
  switch (k) {
    case AlarmKind::Overspeed: return "overspeed_ramp";
    case AlarmKind::VibSpike: return "vibration_spike";
    case AlarmKind::ThermalDrift: return "thermal_drift";
    case AlarmKind::Dropout: return "sensor_dropout";
  }
  return "unknown";
}

struct AlarmEvent {
  uint32_t tick;
  uint32_t channel;
  AlarmKind kind;
  bool raised;
};

struct DetectConfig {
  float rpm_limit = 10250.0f;
  float vib_limit = 2.0f;
  float temp_roc_limit = 1.5f;  // rise across the roc window that alarms
  uint32_t roc_window = 16;     // frames in the rate of change window
  uint32_t raise_streak = 3;
  uint32_t clear_streak = 8;
  uint32_t dropout_gap = 8;  // ticks without a frame; 0 disables dropout scan
};

class Detector {
 public:
  Detector(uint32_t channels, const DetectConfig& cfg)
      : cfg_(cfg), st_(channels) {
    for (auto& s : st_) s.temp_ring.assign(cfg_.roc_window, 0.0f);
  }

  // Threshold and rate of change checks for one frame. Deterministic per
  // channel because each channel's frames arrive in tick order.
  void process(const Frame& f, std::vector<AlarmEvent>& out) {
    ChannelState& s = st_[f.channel];
    step(s, 0, f.rpm > cfg_.rpm_limit, f.channel, f.tick, AlarmKind::Overspeed, out);
    step(s, 1, f.vib > cfg_.vib_limit, f.channel, f.tick, AlarmKind::VibSpike, out);
    bool thermal_bad = false;
    if (s.temp_count >= cfg_.roc_window) {
      float oldest = s.temp_ring[s.temp_count % cfg_.roc_window];
      thermal_bad = (f.temp - oldest) > cfg_.temp_roc_limit;
    }
    step(s, 2, thermal_bad, f.channel, f.tick, AlarmKind::ThermalDrift, out);
    s.temp_ring[s.temp_count % cfg_.roc_window] = f.temp;
    s.temp_count++;
  }

  // Watermark path, called only during deterministic tick finalization.
  void note_seen(uint32_t ch, uint32_t tick, std::vector<AlarmEvent>& out) {
    ChannelState& s = st_[ch];
    if (s.dropout_active) {
      s.dropout_active = false;
      out.push_back({tick, ch, AlarmKind::Dropout, false});
    }
    s.last_seen = tick;
    s.seen_any = true;
  }

  void check_dropouts(uint32_t tick, std::vector<AlarmEvent>& out) {
    if (cfg_.dropout_gap == 0) return;
    for (uint32_t ch = 0; ch < st_.size(); ++ch) {
      ChannelState& s = st_[ch];
      if (s.dropout_active) continue;
      bool gap = s.seen_any ? (tick - s.last_seen > cfg_.dropout_gap)
                            : (tick + 1 > cfg_.dropout_gap);
      if (gap) {
        s.dropout_active = true;
        out.push_back({tick, ch, AlarmKind::Dropout, true});
      }
    }
  }

 private:
  struct ChannelState {
    uint32_t last_seen = 0;
    bool seen_any = false;
    bool dropout_active = false;
    // index 0 overspeed, 1 vibration, 2 thermal
    uint32_t bad_streak[3] = {0, 0, 0};
    uint32_t good_streak[3] = {0, 0, 0};
    bool active[3] = {false, false, false};
    std::vector<float> temp_ring;
    uint32_t temp_count = 0;
  };

  void step(ChannelState& s, int idx, bool bad, uint32_t ch, uint32_t tick,
            AlarmKind kind, std::vector<AlarmEvent>& out) {
    if (bad) {
      s.bad_streak[idx]++;
      s.good_streak[idx] = 0;
    } else {
      s.good_streak[idx]++;
      s.bad_streak[idx] = 0;
    }
    if (!s.active[idx] && s.bad_streak[idx] >= cfg_.raise_streak) {
      s.active[idx] = true;
      out.push_back({tick, ch, kind, true});
    } else if (s.active[idx] && s.good_streak[idx] >= cfg_.clear_streak) {
      s.active[idx] = false;
      out.push_back({tick, ch, kind, false});
    }
  }

  DetectConfig cfg_;
  std::vector<ChannelState> st_;
};

}  // namespace rt
