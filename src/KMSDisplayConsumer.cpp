#include "KMSDisplayConsumer.h"

#include <memory>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void checkSDL(int result, const char *operation) {
  if (result < 0) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}
}

KMSDisplayConsumer::KMSDisplayConsumer(int left_offset, int right_offset,
                                     int height_mod, bool display_stats)
    : left_offset(left_offset), right_offset(right_offset), height_mod(height_mod),
      display_stats(display_stats) {}

void KMSDisplayConsumer::InitRenderer(int width, int height) {
  const char *driver = SDL_GetCurrentVideoDriver();
  if (!driver || std::string(driver) != "KMSDRM") {
    throw std::runtime_error("Raspberry Pi playback requires SDL_VIDEODRIVER=kmsdrm from a local console.");
  }
  this->width = width;
  this->heigth = height;
  if (left_offset < 0 || right_offset < 0 ||
      static_cast<int64_t>(left_offset) + right_offset >= width) {
    throw std::runtime_error("Left and right offsets must leave a positive display width.");
  }

  // Preserve the boot-selected interlaced mode. Fullscreen desktop avoids
  // SDL selecting a different mode with matching dimensions.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  window = SDL_CreateWindow("PCM", SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
                            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0), width, height,
                            SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window) {
    throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
  }
  renderer = SDL_CreateRenderer(window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
  }
  SDL_ShowCursor(SDL_DISABLE);
  checkSDL(SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255), "SDL_SetRenderDrawColor");
  // Each PCM image contains both fields: 25 PAL or 30000/1001 NTSC images/s.
  // Explicit pacing also covers drivers whose vsync is reported per field.
  frame_period = height == 576 ? std::chrono::nanoseconds(40000000)
                               : std::chrono::nanoseconds(33366667);
  next_frame = std::chrono::steady_clock::now();
}

void KMSDisplayConsumer::renderFrame(const IFrame &frame) {
  auto pixels = frame.render();
  // Use the actual cropped source height, never the display height, as the
  // buffer extent. Vertical scaling would move PCM data between scan lines.
  std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> surface(
      SDL_CreateRGBSurfaceFrom(pixels.pixels.data(), frame.width(), frame.heigth(),
                               8, frame.width(), 0, 0, 0, 0), SDL_FreeSurface);
  if (!surface) {
    throw std::runtime_error(std::string("SDL_CreateRGBSurfaceFrom: ") + SDL_GetError());
  }
  checkSDL(SDL_SetSurfacePalette(surface.get(), palete), "SDL_SetSurfacePalette");
  std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(
      SDL_CreateTextureFromSurface(renderer, surface.get()), SDL_DestroyTexture);
  if (!texture) {
    throw std::runtime_error(std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError());
  }
  const int64_t dest_height = static_cast<int64_t>(frame.heigth()) + height_mod;
  if (dest_height <= 0 || dest_height > INT32_MAX) {
    throw std::runtime_error("Height modifier must leave a valid positive frame height.");
  }
  SDL_Rect dest{left_offset, 0, width - left_offset - right_offset,
                static_cast<int>(dest_height)};
  if (display_stats && !stats_started) {
    const char *double_buffer = SDL_GetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER);
    std::fprintf(stderr, "\nKMS geometry: source=%dx%d, draw=%dx%d+%d+%d, screen=%dx%d; double-buffer hint=%s\n",
        frame.width(), frame.heigth(), dest.w, dest.h, dest.x, dest.y,
        width, heigth, double_buffer ? double_buffer : "unset");
  }
  checkSDL(SDL_RenderClear(renderer), "SDL_RenderClear");
  checkSDL(SDL_RenderCopy(renderer, texture.get(), nullptr, &dest), "SDL_RenderCopy");
  std::this_thread::sleep_until(next_frame);
  SDL_RenderPresent(renderer);
  next_frame += frame_period;
  const auto now = std::chrono::steady_clock::now();
  if (display_stats) {
    reportPresent(now);
  }
  // Do not burst frames when decoding falls behind the output clock.
  if (next_frame < now) {
    next_frame = now;
  }
}

void KMSDisplayConsumer::reportPresent(std::chrono::steady_clock::time_point now) {
  if (!stats_started) {
    stats_start = previous_present = now;
    stats_started = true;
    return;
  }
  const double gap = std::chrono::duration<double, std::milli>(now - previous_present).count();
  const double target = std::chrono::duration<double, std::milli>(frame_period).count();
  previous_present = now;
  ++intervals;
  min_gap_ms = intervals == 1 ? gap : std::min(min_gap_ms, gap);
  max_gap_ms = intervals == 1 ? gap : std::max(max_gap_ms, gap);
  short_intervals += gap < target * 0.75;
  long_intervals += gap > target * 1.25;
  const double elapsed = std::chrono::duration<double>(now - stats_start).count();
  if (elapsed >= 5.0) {
    // These are SDL call-return times, not measured analogue field timestamps.
    std::fprintf(stderr, "\nKMS timing: %.3f present returns/s; gap min=%.3f max=%.3f ms; target=%.3f ms; short=%u long=%u of %u\n",
        intervals / elapsed, min_gap_ms, max_gap_ms, target,
        short_intervals, long_intervals, intervals);
    stats_start = now;
    intervals = short_intervals = long_intervals = 0;
  }
}
