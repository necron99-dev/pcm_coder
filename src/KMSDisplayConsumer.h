#pragma once

#include <chrono>
#include "SDL2DisplayConsumerBase.h"

// SDL owns DRM/KMS and EGL; no Raspberry Pi firmware libraries are needed.
struct KMSDisplayConsumer : public SDL2DisplayConsumerBase {
  KMSDisplayConsumer(int left_offset, int right_offset, int height_mod);
  void InitRenderer(int width, int height) override;

protected:
  void renderFrame(const IFrame &frame) override;

private:
  int left_offset, right_offset, height_mod;
  std::chrono::steady_clock::time_point next_frame{};
  std::chrono::nanoseconds frame_period{};
};
