// Episode level detection scoring against ground truth labels.
//
// Alarm intervals are rebuilt from the event log (raise to clear, open
// intervals close at run end). A ground truth episode counts as detected
// when an alarm interval of the same channel and matching kind overlaps it
// (with a small slack for detection lag). Unmatched alarm intervals are
// false positives, unmatched episodes are false negatives. If detector
// flicker splits one fault into two alarm intervals, the second interval
// counts as a false positive; that is intentional and strict.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "frame.hpp"
#include "trip.hpp"

namespace rt {

struct AlarmInterval {
  uint32_t channel;
  AlarmKind kind;
  uint32_t start;
  uint32_t end;
  bool matched = false;
};

inline std::vector<AlarmInterval> build_alarm_intervals(
    const std::vector<LogEvent>& log, uint32_t run_end) {
  std::vector<AlarmInterval> out;
  std::map<uint64_t, uint32_t> open;  // (channel, kind) -> start tick
  for (const LogEvent& e : log) {
    if (e.type != LogType::AlarmRaised && e.type != LogType::AlarmCleared) continue;
    uint64_t key = (static_cast<uint64_t>(e.channel) << 8) | e.kind;
    if (e.type == LogType::AlarmRaised) {
      open[key] = e.tick;
    } else {
      auto it = open.find(key);
      if (it != open.end()) {
        out.push_back({e.channel, static_cast<AlarmKind>(e.kind), it->second,
                       e.tick, false});
        open.erase(it);
      }
    }
  }
  for (const auto& kv : open) {
    out.push_back({static_cast<uint32_t>(kv.first >> 8),
                   static_cast<AlarmKind>(kv.first & 0xff), kv.second, run_end,
                   false});
  }
  return out;
}

struct TypeEval {
  uint32_t tp = 0;
  uint32_t fp = 0;
  uint32_t fn = 0;
  bool precision_defined() const { return tp + fp > 0; }
  bool recall_defined() const { return tp + fn > 0; }
  double precision() const {
    return precision_defined() ? static_cast<double>(tp) / (tp + fp) : 0.0;
  }
  double recall() const {
    return recall_defined() ? static_cast<double>(tp) / (tp + fn) : 0.0;
  }
};

struct EvalResult {
  TypeEval by_type[kNumFaultTypes];
  std::vector<uint32_t> detection_latency_ticks;  // matched episodes only
  uint32_t episodes_total = 0;
  uint32_t episodes_detected = 0;
};

inline EvalResult evaluate(const std::vector<Episode>& episodes,
                           std::vector<AlarmInterval> intervals,
                           uint32_t slack = 25) {
  EvalResult r;
  for (const Episode& ep : episodes) {
    int ti = static_cast<int>(ep.type) - 1;  // Fault::None offset
    bool found = false;
    for (AlarmInterval& iv : intervals) {
      if (iv.matched || iv.channel != ep.channel ||
          static_cast<int>(iv.kind) != ti)
        continue;
      if (iv.start <= ep.end + slack && iv.end >= ep.start) {
        iv.matched = true;
        found = true;
        r.detection_latency_ticks.push_back(
            iv.start > ep.start ? iv.start - ep.start : 0);
        break;
      }
    }
    r.episodes_total++;
    if (found) {
      r.by_type[ti].tp++;
      r.episodes_detected++;
    } else {
      r.by_type[ti].fn++;
    }
  }
  for (const AlarmInterval& iv : intervals) {
    if (!iv.matched) r.by_type[static_cast<int>(iv.kind)].fp++;
  }
  return r;
}

}  // namespace rt
