#pragma once

#include <chrono>
#include "SDL2DisplayConsumerBase.h"

// SDL owns DRM/KMS and EGL; no Raspberry Pi firmware libraries are needed.
struct KMSDisplayConsumer : public SDL2DisplayConsumerBase {
  KMSDisplayConsumer(int left_offset, int right_offset, int height_mod,
                     bool display_stats = false);
  void InitRenderer(int width, int height) override;

protected:
  void renderFrame(const IFrame &frame) override;

private:
  int left_offset, right_offset, height_mod;
  std::chrono::steady_clock::time_point next_frame{};
  std::chrono::nanoseconds frame_period{};
  bool display_stats, stats_started = false;
  std::chrono::steady_clock::time_point stats_start{}, previous_present{};
  unsigned intervals = 0, short_intervals = 0, long_intervals = 0;
  double min_gap_ms = 0, max_gap_ms = 0;

  void reportPresent(std::chrono::steady_clock::time_point now);
};
