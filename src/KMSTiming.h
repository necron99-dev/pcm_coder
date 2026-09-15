#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace kms {
struct Stamp {
  uint32_t sequence = 0;
  uint64_t us = 0;
};

// Counters wrap at 32 bits. Compare only nearby events (< 2^31 ticks apart).
inline int32_t distance(uint32_t newer, uint32_t older) {
  return static_cast<int32_t>(newer - older);
}

inline unsigned ticksPerFrame(std::vector<double> tick_us, double frame_us) {
  if (tick_us.size() < 4)
    throw std::runtime_error("Not enough DRM vblank samples.");
  std::sort(tick_us.begin(), tick_us.end());
  const double tick = tick_us[tick_us.size() / 2];
  for (unsigned ticks : {1u, 2u}) {
    const double expected = frame_us / ticks;
    if (tick > expected * .9 && tick < expected * 1.1) return ticks;
  }
  throw std::runtime_error("DRM vblank cadence does not match the interlaced mode.");
}

// With two ticks per image, submit during the intervening field so the flip
// lands on the same relative phase as the previous completed flip.
// When late, keep this phase rather than submitting a burst to catch up.
inline uint32_t submissionTick(uint32_t previous, uint32_t current,
                               unsigned ticks) {
  if (ticks == 1) return current;
  const int32_t elapsed = distance(current, previous);
  if (elapsed < 0)
    throw std::runtime_error("DRM vblank counter moved backwards.");
  return current + ((static_cast<unsigned>(elapsed) % ticks == 0) ? 1 : 0);
}

inline unsigned completedFrames(Stamp previous, Stamp current,
                                 unsigned ticks, double frame_us) {
  const int32_t elapsed = distance(current.sequence, previous.sequence);
  if (!ticks || elapsed <= 0 || elapsed % ticks != 0 || current.us <= previous.us ||
      current.us - previous.us < frame_us * .75)
    throw std::runtime_error(
        "DRM page flip lost frame phase; stopping to avoid mixing PCM fields.");
  const double expected = (elapsed / ticks) * frame_us;
  if (std::abs(double(current.us - previous.us) - expected) > frame_us * .25)
    throw std::runtime_error("DRM flip timestamps disagree with the frame counter.");
  return static_cast<unsigned>(elapsed) / ticks;
}
} // namespace kms
