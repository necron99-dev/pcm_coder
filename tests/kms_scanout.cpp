// Integration test: real consumer and mmap buffers, simulated SDL/DRM device.
// No display required. Driver events deliberately include delays and wraparound.
#include "KMSDisplayConsumer.h"
#include "KMSTiming.h"
#include <SDL2/SDL_syswm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
struct Allocation { uint64_t offset, size; uint32_t pitch; };
std::map<uint32_t, Allocation> allocations;
uint32_t connector_id = 31, crtc_id = 42, sequence, next_handle, front_fb;
uint64_t timestamp, next_offset;
unsigned event_ticks, counter_stride, delayed_flip, create_failure, scanout_parity;
bool pending, page_flip, restored, cancel_playback;
void *event_data;
FILE *backing;
std::vector<uint32_t> flips;
std::vector<std::vector<uint32_t>> captured_fields;
drmModeModeInfo mode;

void reset(unsigned ticks, bool pal = false) {
  allocations.clear(); flips.clear(); captured_fields.clear(); scanout_parity = 0;
  sequence = UINT32_MAX - 12; timestamp = 1000000;
  next_handle = 1; next_offset = 0; front_fb = 99;
  event_ticks = ticks; counter_stride = 1; delayed_flip = create_failure = 0;
  pending = restored = cancel_playback = false;
  backing = tmpfile(); assert(backing);
  mode = {};
  mode.hdisplay = 720; mode.vdisplay = pal ? 576 : 480;
  mode.htotal = pal ? 864 : 858; mode.vtotal = pal ? 625 : 525;
  mode.clock = 13500; mode.flags = DRM_MODE_FLAG_INTERLACE;
  SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
  assert(SDL_Init(SDL_INIT_VIDEO) == 0);
}
void advance(unsigned delta) {
  sequence += delta;
  if (event_ticks >= 2) scanout_parity ^= (delta / (event_ticks / 2)) & 1u;
  const double frame_us = 1000.0 * mode.htotal * mode.vtotal / mode.clock;
  timestamp += static_cast<uint64_t>(delta * frame_us / event_ticks);
}
void checkClean() {
  assert(allocations.empty()); assert(!pending);
  assert(fcntl(fileno(backing), F_GETFD) >= 0); // SDL's fd was not closed.
  fclose(backing);
}
struct TestDisplay : KMSDisplayConsumer {
  using KMSDisplayConsumer::KMSDisplayConsumer;
  using KMSDisplayConsumer::renderFrame;
};
struct Pattern : IFrame {
  int seed;
  Pattern(int height = 525, int seed = 0) : IFrame(139, height), seed(seed) {}
  bool Eof() const override { return false; }
  PixelContainer render(uint8_t, uint8_t) const override {
    PixelContainer p(width(), heigth());
    for (int y = 0; y < heigth(); ++y)
      for (int x = 0; x < width(); ++x) p.pixels[y * width() + x] = (x + 3 * y + seed) % 256;
    return p;
  }
};
}

// Only device-specific SDL calls are replaced; palette/event teardown is real SDL.
extern "C" {
const char *SDL_GetCurrentVideoDriver() { return "KMSDRM"; }
SDL_Window *SDL_CreateWindow(const char *, int, int, int, int, Uint32) {
  return reinterpret_cast<SDL_Window *>(1);
}
void SDL_DestroyWindow(SDL_Window *) {}
SDL_bool SDL_GetWindowWMInfo(SDL_Window *, SDL_SysWMinfo *info) {
  info->subsystem = SDL_SYSWM_KMSDRM;
  info->info.kmsdrm.drm_fd = fileno(backing);
  return SDL_TRUE;
}
drmModeResPtr drmModeGetResources(int) {
  auto *r = new drmModeRes{};
  r->count_connectors = r->count_crtcs = 1;
  r->connectors = &connector_id; r->crtcs = &crtc_id; return r;
}
void drmModeFreeResources(drmModeResPtr r) { delete r; }
drmModeConnectorPtr drmModeGetConnector(int, uint32_t) {
  auto *c = new drmModeConnector{};
  c->connector_id = connector_id; c->connector_type = DRM_MODE_CONNECTOR_Composite;
  c->connection = DRM_MODE_CONNECTED; c->encoder_id = 7; return c;
}
void drmModeFreeConnector(drmModeConnectorPtr c) { delete c; }
drmModeEncoderPtr drmModeGetEncoder(int, uint32_t) {
  auto *e = new drmModeEncoder{}; e->crtc_id = crtc_id; return e;
}
void drmModeFreeEncoder(drmModeEncoderPtr e) { delete e; }
drmModeCrtcPtr drmModeGetCrtc(int, uint32_t) {
  auto *c = new drmModeCrtc{};
  c->crtc_id = crtc_id; c->mode_valid = 1; c->mode = mode; c->buffer_id = 99;
  return c;
}
void drmModeFreeCrtc(drmModeCrtcPtr c) { delete c; }
int drmIoctl(int fd, unsigned long request, void *arg) {
  if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
    auto *c = static_cast<drm_mode_create_dumb *>(arg);
    if (create_failure && next_handle == create_failure) { errno = ENOMEM; return -1; }
    c->handle = next_handle++; c->pitch = c->width * 4 + 64;
    c->size = (uint64_t(c->pitch) * c->height + 4095) & ~uint64_t(4095);
    allocations[c->handle] = {next_offset, c->size, c->pitch};
    next_offset += c->size;
    assert(ftruncate(fd, next_offset) == 0);
  } else if (request == DRM_IOCTL_MODE_MAP_DUMB) {
    auto *m = static_cast<drm_mode_map_dumb *>(arg);
    m->offset = allocations.at(m->handle).offset;
  } else if (request == DRM_IOCTL_MODE_DESTROY_DUMB) {
    allocations.erase(static_cast<drm_mode_destroy_dumb *>(arg)->handle);
  } else assert(false);
  return 0;
}
int drmModeAddFB2(int, uint32_t, uint32_t, uint32_t format,
                 const uint32_t handles[4], const uint32_t[4], const uint32_t[4],
                 uint32_t *id, uint32_t) {
  assert(format == DRM_FORMAT_XRGB8888); *id = handles[0]; return 0;
}
int drmModeRmFB(int, uint32_t) { return 0; }
int drmModeSetCrtc(int, uint32_t crtc, uint32_t fb, uint32_t, uint32_t,
                   uint32_t *, int, drmModeModeInfoPtr timing) {
  assert(crtc == crtc_id); assert(std::memcmp(timing, &mode, sizeof(mode)) == 0);
  front_fb = fb; if (fb == 99) restored = true;
  return 0;
}
int drmWaitVBlank(int, drmVBlankPtr v) {
  if (v->request.type & DRM_VBLANK_EVENT) {
    assert(!pending);
    const int32_t delta = kms::distance(v->request.sequence, sequence);
    if (delta > 0) advance((delta + counter_stride - 1) / counter_stride * counter_stride);
    pending = true; page_flip = false;
    event_data = reinterpret_cast<void *>(v->request.signal);
  } else {
    assert(v->request.sequence == 0);
    v->reply.sequence = sequence;
  }
  return 0;
}
int drmModePageFlip(int, uint32_t, uint32_t fb, uint32_t flags, void *data) {
  assert(!pending); assert(fb != front_fb);
  assert(flags == DRM_MODE_PAGE_FLIP_EVENT);
  advance(counter_stride + delayed_flip); delayed_flip = 0;
  front_fb = fb; flips.push_back(sequence);
  // Observe only the physical field selected by the simulated scanout engine.
  // Its parity is independent of DRM sequence numbering and initial phase.
  const auto &a = allocations.at(fb);
  std::vector<uint32_t> captured;
  std::vector<uint32_t> row(mode.hdisplay);
  for (unsigned y = scanout_parity; y < mode.vdisplay; y += 2) {
    assert(pread(fileno(backing), row.data(), row.size() * sizeof(uint32_t),
                 a.offset + y * a.pitch) == ssize_t(row.size() * sizeof(uint32_t)));
    captured.insert(captured.end(), row.begin(), row.end());
  }
  captured_fields.push_back(std::move(captured));
  pending = page_flip = true; event_data = data; return 0;
}
int drmHandleEvent(int fd, drmEventContextPtr context) {
  assert(pending); pending = false;
  auto handler = page_flip ? context->page_flip_handler : context->vblank_handler;
  handler(fd, sequence, timestamp / 1000000, timestamp % 1000000, event_data);
  return 0;
}
}

void checkCapturedField(size_t capture, unsigned logical_field, int seed) {
  const auto &pixels = captured_fields.at(capture);
  for (unsigned line = 0; line < mode.vdisplay / 2; ++line) {
    const unsigned source_row = 2 * line + logical_field;
    for (unsigned x = 0; x < 720; ++x) {
      const unsigned sx = x < 9 ? 0 : unsigned(((x - 9) + .5) / 711.0 * 139);
      const unsigned value = x < 9 ? 0 : (sx + 3 * source_row + seed) % 256;
      assert(pixels.at(line * 720 + x) == value * 0x010101u);
    }
  }
}

int main() {
  // Both initial physical parities must emit the same chronological PCM fields.
  // Exercise standard counters and the Pi's doubled software counters, PAL/NTSC.
  for (bool pal : {false, true}) for (unsigned ticks : {2u, 4u})
  for (unsigned initial_parity : {0u, 1u}) {
    reset(ticks, pal);
    counter_stride = ticks / 2;
    scanout_parity = initial_parity;
    {
      TestDisplay display(9, 0, 0);
      display.InitRenderer(720, mode.vdisplay);
      const int height = pal ? 625 : 525;
      for (int i = 0; i < 5; ++i) display.renderFrame(Pattern(height, i * 17));
      assert(flips.size() == 11); // Black anchor followed by 10 PCM fields.
      for (size_t i = 1; i < flips.size(); ++i) {
        assert(kms::distance(flips[i], flips[i - 1]) == int(counter_stride));
        checkCapturedField(i, (i - 1) % 2, ((i - 1) / 2) * 17);
      }
      // A late producer can repeat a field, but cannot reverse subsequent fields.
      advance(3 * ticks);
      display.renderFrame(Pattern(height, 111));
      assert(kms::distance(flips[11], flips[10]) == int(3 * ticks + counter_stride));
      assert(kms::distance(flips[12], flips[11]) == int(counter_stride));
      checkCapturedField(11, 0, 111);
      checkCapturedField(12, 1, 111);
    }
    assert(restored); checkClean();
  }
  // Drivers that expose only full-frame events retain a normal interlaced image.
  for (bool pal : {false, true}) for (unsigned ticks : {1u, 2u, 4u}) {
    reset(ticks, pal); counter_stride = ticks;
    {
      TestDisplay display(9, 0, 0); display.InitRenderer(720, mode.vdisplay);
      for (int i = 0; i < 5; ++i) display.renderFrame(Pattern(pal ? 625 : 525));
      assert(flips.size() == 6);
      for (size_t i = 1; i < flips.size(); ++i)
        assert(kms::distance(flips[i], flips[i - 1]) == int(ticks));
      const auto &a = allocations.at(front_fb);
      std::vector<uint32_t> row(a.pitch / 4);
      for (int y : {0, 1, 18, 200, int(mode.vdisplay) - 1}) {
        assert(pread(fileno(backing), row.data(), a.pitch, a.offset + y * a.pitch) == a.pitch);
        for (int x = 0; x < 720; ++x) {
          const unsigned sx = x < 9 ? 0 : unsigned(((x - 9) + .5) / 711.0 * 139);
          const unsigned value = x < 9 ? 0 : (sx + 3 * y) % 256;
          assert(row[x] == value * 0x010101u);
        }
      }
    }
    assert(restored); checkClean();
  }
  // A flip delayed by one field must not create a persistent field-order error.
  for (unsigned ticks : {2u, 4u}) {
    reset(ticks); counter_stride = ticks / 2;
    {
      TestDisplay display(9, 0, 0); display.InitRenderer(720, 480);
      delayed_flip = counter_stride;
      display.renderFrame(Pattern());
      display.renderFrame(Pattern(525, 17));
      assert(kms::distance(flips[1], flips[0]) == int(2 * counter_stride));
      for (size_t i = 1; i < flips.size(); ++i)
        checkCapturedField(i, (i - 1) % 2, ((i - 1) / 2) * 17);
    }
    assert(restored); checkClean();
  }
  // Partial allocation failure releases the first buffer without a modeset.
  reset(2); create_failure = 2;
  {
    TestDisplay display(9, 0, 0);
    bool failed = false;
    try { display.InitRenderer(720, 480); } catch (const std::runtime_error &) { failed = true; }
    assert(failed);
  }
  assert(!restored); checkClean();
  // Cancellation during calibration still drains the pending event at teardown.
  reset(2); cancel_playback = true;
  {
    TestDisplay display(9, 0, 0, false, [] { return cancel_playback; });
    display.InitRenderer(720, 480); display.renderFrame(Pattern());
    assert(flips.empty());
  }
  assert(restored); checkClean();
  // Reject a non-interlaced mode before allocating or changing scanout.
  reset(1); mode.flags = 0;
  {
    TestDisplay display(9, 0, 0);
    bool failed = false;
    try { display.InitRenderer(720, 480); } catch (const std::runtime_error &) { failed = true; }
    assert(failed);
  }
  assert(!restored); checkClean();
  bool rejected = false;
  try { kms::ticksPerFrame({1000, 1000, 1000, 1000}, 33366.67); }
  catch (const std::runtime_error &error) {
    const std::string message = error.what();
    assert(message.find("1000 us/counter tick") != std::string::npos);
    assert(message.find("mode=33366.7") != std::string::npos);
    rejected = true;
  }
  assert(rejected);
  rejected = false;
  try { kms::hasFieldEvents(4, 1); }
  catch (const std::runtime_error &) { rejected = true; }
  assert(rejected); // Four events/image is not the doubled software counter case.
  rejected = false;
  try { kms::completedIntervals({10, 1000000}, {12, 1100000}, 2, 33366.67); }
  catch (const std::runtime_error &) { rejected = true; }
  assert(rejected);
  std::puts("PASS: identical field order for both startup parities, DRM cadence, wraparound, late fields, raster, cleanup and cancellation");
}
