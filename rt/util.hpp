// Small deterministic utilities shared by the telemetry layer.
// No external dependencies, C++17 only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt {

// SplitMix64 finalizer. Used both as a tiny PRNG step and as a mixing hash.
inline uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Stateless deterministic hash keyed by (seed, a, b, c). Every synthetic
// sensor sample is derived from this, so frame generation does not depend on
// thread scheduling at all.
inline uint64_t mix4(uint64_t seed, uint64_t a, uint64_t b, uint64_t c) {
  uint64_t h = splitmix64(seed ^ (a * 0x9e3779b97f4a7c15ULL));
  h = splitmix64(h ^ (b * 0xc2b2ae3d27d4eb4fULL));
  h = splitmix64(h ^ (c * 0x165667b19e3779f9ULL));
  return h;
}

inline double u01(uint64_t h) {
  return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

// Approximate standard normal built from four uniforms (Irwin Hall, shifted
// and scaled to unit variance). Plenty good for synthetic sensor noise and
// fully deterministic.
inline double gauss(uint64_t h) {
  double s = 0.0;
  for (int i = 0; i < 4; ++i) {
    h = splitmix64(h);
    s += u01(h);
  }
  return (s - 2.0) * 1.7320508075688772;
}

// FNV-1a used for the deterministic event log hash.
constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline uint64_t fnv1a(const void* data, size_t n, uint64_t h = kFnvOffset) {
  const unsigned char* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

// Linear interpolation percentile over a copy free sorted vector.
// The vector is sorted in place.
inline double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  double idx = p * static_cast<double>(v.size() - 1);
  size_t lo = static_cast<size_t>(idx);
  size_t hi = std::min(lo + 1, v.size() - 1);
  double f = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - f) + v[hi] * f;
}

}  // namespace rt
