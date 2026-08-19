// Telemetry frame and fault episode types.
#pragma once

#include <cstdint>

namespace rt {

// Injected fault classes. Ground truth labels come from the generator.
enum class Fault : uint8_t {
  None = 0,
  Overspeed = 1,
  VibSpike = 2,
  ThermalDrift = 3,
  Dropout = 4,
};

constexpr int kNumFaultTypes = 4;

inline const char* fault_name(Fault f) {
  switch (f) {
    case Fault::Overspeed: return "overspeed_ramp";
    case Fault::VibSpike: return "vibration_spike";
    case Fault::ThermalDrift: return "thermal_drift";
    case Fault::Dropout: return "sensor_dropout";
    default: return "none";
  }
}

// One telemetry frame: all four sensors for one channel at one sim tick.
// emit_ns carries a steady clock stamp for latency sampled frames, 0 else.
struct Frame {
  uint32_t channel;
  uint32_t tick;
  uint64_t emit_ns;
  float rpm;
  float vib;
  float temp;
  float cur;
};

// Ground truth fault episode, ticks inclusive on both ends.
struct Episode {
  uint32_t channel;
  Fault type;
  uint32_t start;
  uint32_t end;
};

}  // namespace rt
