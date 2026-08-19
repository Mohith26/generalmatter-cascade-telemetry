// Cascade level protection state machine: NORMAL -> ALARM -> TRIP with
// hysteresis, coordinated quiesce of every channel on trip, and an auto
// reset cooldown so long runs can exercise many episodes.
//
// The machine consumes alarm events one finalized tick at a time, already
// sorted into a canonical order, so the event log and its hash are fully
// deterministic for a given seed.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "detect.hpp"
#include "util.hpp"

namespace rt {

enum class SysState : uint8_t { Normal = 0, Alarm = 1, Trip = 2 };

struct TripConfig {
  uint32_t persist_ticks = 10;  // single channel alarm persistence to trip
  uint32_t multi_channel = 2;   // distinct alarmed channels for instant trip
  uint32_t clear_ticks = 12;    // alarm free ticks to drop back to normal
  uint32_t reset_ticks = 50;    // cooldown after trip before auto reset
};

enum class LogType : uint8_t {
  AlarmRaised = 0,
  AlarmCleared = 1,
  StateAlarm = 2,
  StateNormal = 3,
  Trip = 4,
  QuiesceDone = 5,
  Reset = 6,
};

struct LogEvent {
  uint32_t tick;
  LogType type;
  uint32_t channel;
  uint8_t kind;
  uint32_t data;
};

// Canonical within tick ordering for determinism.
inline bool alarm_event_less(const AlarmEvent& a, const AlarmEvent& b) {
  if (a.channel != b.channel) return a.channel < b.channel;
  if (a.kind != b.kind) return a.kind < b.kind;
  return a.raised < b.raised;
}

class TripSM {
 public:
  TripSM(uint32_t channels, const TripConfig& cfg)
      : cfg_(cfg), mask_(channels, 0), quiesced_(channels, 0) {}

  // Feed the (sorted) alarm events of one tick. Must be called for every
  // tick in order, including empty ones, so timing hysteresis is exact.
  void on_tick(uint32_t tick, const AlarmEvent* evs, size_t n) {
    if (state_ == SysState::Trip && tick - trip_tick_ >= cfg_.reset_ticks) {
      state_ = SysState::Normal;
      std::fill(quiesced_.begin(), quiesced_.end(), uint8_t{0});
      push(tick, LogType::Reset, 0, 0, 0);
    }
    for (size_t i = 0; i < n; ++i) {
      const AlarmEvent& e = evs[i];
      uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(e.kind));
      uint8_t& m = mask_[e.channel];
      if (e.raised) {
        if (!(m & bit)) {
          if (m == 0) alarmed_channels_++;
          m = static_cast<uint8_t>(m | bit);
          push(e.tick, LogType::AlarmRaised, e.channel,
               static_cast<uint8_t>(e.kind), 0);
        }
      } else {
        if (m & bit) {
          m = static_cast<uint8_t>(m & ~bit);
          if (m == 0) alarmed_channels_--;
          push(e.tick, LogType::AlarmCleared, e.channel,
               static_cast<uint8_t>(e.kind), 0);
        }
      }
    }
    if (alarmed_channels_ > 0) last_alarm_tick_ = tick;
    switch (state_) {
      case SysState::Normal:
        if (alarmed_channels_ > 0) {
          state_ = SysState::Alarm;
          alarm_entered_ = tick;
          push(tick, LogType::StateAlarm, 0, 0, alarmed_channels_);
        }
        break;
      case SysState::Alarm:
        if (alarmed_channels_ == 0) {
          if (tick - last_alarm_tick_ >= cfg_.clear_ticks) {
            state_ = SysState::Normal;
            push(tick, LogType::StateNormal, 0, 0, 0);
          }
        } else if (alarmed_channels_ >= cfg_.multi_channel ||
                   tick - alarm_entered_ >= cfg_.persist_ticks) {
          state_ = SysState::Trip;
          trip_tick_ = tick;
          trips_++;
          trip_latency_ticks_.push_back(tick - alarm_entered_);
          uint32_t qn = 0;
          for (auto& q : quiesced_) {
            q = 1;
            ++qn;
          }
          last_quiesce_count_ = qn;
          push(tick, LogType::Trip, 0, 0, alarmed_channels_);
          push(tick, LogType::QuiesceDone, 0, 0, qn);
        }
        break;
      case SysState::Trip:
        break;  // latched until the reset cooldown expires
    }
  }

  SysState state() const { return state_; }
  uint32_t trips() const { return trips_; }
  uint32_t last_quiesce_count() const { return last_quiesce_count_; }
  uint32_t alarmed_channels() const { return alarmed_channels_; }
  bool all_quiesced() const {
    for (auto q : quiesced_)
      if (!q) return false;
    return true;
  }
  const std::vector<LogEvent>& log() const { return log_; }
  const std::vector<uint32_t>& trip_latency_ticks() const {
    return trip_latency_ticks_;
  }

  // Deterministic hash of the whole event log. Fields are hashed one by one
  // so struct padding can never leak in.
  uint64_t log_hash() const {
    uint64_t h = kFnvOffset;
    for (const LogEvent& e : log_) {
      uint32_t t = e.tick;
      uint8_t ty = static_cast<uint8_t>(e.type);
      uint32_t ch = e.channel;
      uint8_t k = e.kind;
      uint32_t d = e.data;
      h = fnv1a(&t, sizeof(t), h);
      h = fnv1a(&ty, sizeof(ty), h);
      h = fnv1a(&ch, sizeof(ch), h);
      h = fnv1a(&k, sizeof(k), h);
      h = fnv1a(&d, sizeof(d), h);
    }
    return h;
  }

 private:
  void push(uint32_t tick, LogType type, uint32_t ch, uint8_t kind, uint32_t data) {
    log_.push_back({tick, type, ch, kind, data});
  }

  TripConfig cfg_;
  std::vector<uint8_t> mask_;
  std::vector<uint8_t> quiesced_;
  std::vector<LogEvent> log_;
  std::vector<uint32_t> trip_latency_ticks_;
  SysState state_ = SysState::Normal;
  uint32_t alarmed_channels_ = 0;
  uint32_t alarm_entered_ = 0;
  uint32_t last_alarm_tick_ = 0;
  uint32_t trip_tick_ = 0;
  uint32_t trips_ = 0;
  uint32_t last_quiesce_count_ = 0;
};

}  // namespace rt
