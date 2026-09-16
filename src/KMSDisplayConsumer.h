#pragma once

#include <memory>
#include "SDL2DisplayConsumerBase.h"

// SDL owns the device/console; this consumer owns scanout and DRM events.
// Never create an SDL renderer or swap its window on this path.
struct KMSDisplayConsumer : public SDL2DisplayConsumerBase {
  KMSDisplayConsumer(int left_offset, int right_offset, int height_mod,
                     bool display_stats = false,
                     std::function<bool()> stopping = {},
                     bool full_frame = false);
  ~KMSDisplayConsumer() override;
  void InitRenderer(int width, int height) override;
protected:
  void renderFrame(const IFrame &frame) override;
private:
  struct Scanout;
  std::unique_ptr<Scanout> scanout;
  int left_offset, right_offset, height_mod;
  bool display_stats;
  bool full_frame;
  std::function<bool()> stopping;
};
