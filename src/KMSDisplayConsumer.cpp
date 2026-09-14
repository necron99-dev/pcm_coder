#include "KMSDisplayConsumer.h"

#include <memory>
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
                                     int height_mod)
    : left_offset(left_offset), right_offset(right_offset), height_mod(height_mod) {}

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
  checkSDL(SDL_RenderClear(renderer), "SDL_RenderClear");
  checkSDL(SDL_RenderCopy(renderer, texture.get(), nullptr, &dest), "SDL_RenderCopy");
  std::this_thread::sleep_until(next_frame);
  SDL_RenderPresent(renderer);
  next_frame += frame_period;
  const auto now = std::chrono::steady_clock::now();
  // Do not burst frames when decoding falls behind the output clock.
  if (next_frame < now) {
    next_frame = now;
  }
}
