#include "KMSDisplayConsumer.h"
#include "KMSTiming.h"

#include <SDL2/SDL_syswm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace {
void checkDRM(int result, const char *operation) {
  if (result < 0)
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

// IEC 60841 Figure 3: data low/high are 0.1/0.4 V above blanking,
// while the white reference is 0.7 V above blanking. These RGB values
// assume a linear full-range DAC transfer; actual Pi voltages are unmeasured.
constexpr uint8_t pcm_low = 22;   // Experimental reduction from nominal RGB 36.
constexpr uint8_t pcm_high = 146; // round(255 * 0.4 / 0.7)
constexpr uint8_t pcm_white = 255;

void raisePCMDataZero(IFrame::PixelContainer &pixels) {
  constexpr int pcm_width = IFrame::WHITE_LINE + IFrame::WHITE_WIDTH + 1;
  if (pixels.width() != pcm_width)
    throw std::runtime_error("--kms-pcm-levels requires a 139-cell PCM source.");
  for (int y = 0; y < pixels.heigth(); ++y) {
    auto *row = pixels.pixels.data() + size_t(y) * pcm_width;
    if (std::all_of(row, row + pcm_width, [](uint8_t v) { return v == 0; }))
      continue; // Preserve vertical blank padding.
    if (row[0] != 0 || row[pcm_width - 1] != 0 ||
        row[IFrame::SYNC_LINE_1] != pcm_high ||
        row[IFrame::SYNC_LINE_2] != pcm_high ||
        !std::all_of(row + IFrame::WHITE_LINE,
                     row + IFrame::WHITE_LINE + IFrame::WHITE_WIDTH,
                     [](uint8_t v) { return v == pcm_white; }))
      throw std::runtime_error("--kms-pcm-levels requires intact PCM line markers.");
    // Include the low bits in data sync and the one-cell gap before white.
    // Keep the outer blank cells and white reference unchanged.
    for (int x = IFrame::SYNC_LINE_1; x < IFrame::WHITE_LINE; ++x) {
      if (row[x] == 0) row[x] = pcm_low;
      else if (row[x] != pcm_high)
        throw std::runtime_error("Unexpected PCM data level.");
    }
  }
}
}

struct KMSDisplayConsumer::Scanout {
  struct Buffer {
    uint32_t handle = 0, fb = 0, pitch = 0;
    uint64_t size = 0;
    void *map = MAP_FAILED;
  };
  int fd = -1; // Borrowed from SDL; never close it here.
  drmModeCrtc *saved = nullptr;
  drmModeModeInfo mode{};
  uint32_t connector = 0, pipe = 0;
  std::array<Buffer, 2> buffers{};
  int front = 0;
  bool changed = false, pending = false, have_pcm = false;
  bool field_events = false;
  kms::Stamp event{}, last_flip{};
  unsigned ticks = 0;
  double frame_us = 0;
  std::function<bool()> stopping;
  uint64_t stats_start = 0;
  unsigned intervals = 0, repeats = 0;
  double min_ms = std::numeric_limits<double>::max(), max_ms = 0;

  static void onEvent(int, unsigned sequence, unsigned sec, unsigned usec,
                      void *data) {
    auto &self = *static_cast<Scanout *>(data);
    self.event = {sequence, uint64_t(sec) * 1000000 + usec};
    self.pending = false;
  }

  bool wait(bool cancellable = true) {
    drmEventContext context{};
    context.version = 2;
    context.vblank_handler = onEvent;
    context.page_flip_handler = onEvent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pending) {
      if (cancellable && stopping && stopping()) return false;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Timed out waiting for DRM scanout event.");
      pollfd pfd{fd, POLLIN, 0};
      const int ret = poll(&pfd, 1, 50);
      if (ret < 0 && errno == EINTR) continue;
      checkDRM(ret, "poll DRM");
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw std::runtime_error("DRM device became unavailable.");
      if (pfd.revents & POLLIN)
        checkDRM(drmHandleEvent(fd, &context), "drmHandleEvent");
    }
    return true;
  }

  drmVBlank request(uint32_t sequence, bool relative, bool notify) {
    drmVBlank vblank{};
    vblank.request.type = static_cast<drmVBlankSeqType>(
        (relative ? DRM_VBLANK_RELATIVE : DRM_VBLANK_ABSOLUTE) |
        (notify ? DRM_VBLANK_EVENT : 0) |
        ((pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK));
    vblank.request.sequence = sequence;
    vblank.request.signal = notify ? reinterpret_cast<unsigned long>(this) : 0;
    checkDRM(drmWaitVBlank(fd, &vblank), "drmWaitVBlank");
    return vblank;
  }

  uint32_t currentTick() { return request(0, true, false).reply.sequence; }

  bool waitTick(uint32_t target, bool cancellable = true) {
    request(target, false, true);
    pending = true;
    return wait(cancellable);
  }

  bool flip(int next) {
    checkDRM(drmModePageFlip(fd, saved->crtc_id, buffers[next].fb,
                           DRM_MODE_PAGE_FLIP_EVENT, this), "drmModePageFlip");
    pending = true;
    if (!wait()) return false;
    front = next; // The previous front buffer is now safe to write.
    return true;
  }

  void allocate(Buffer &buffer, int width, int height) {
    drm_mode_create_dumb create{};
    create.width = width;
    create.height = height;
    create.bpp = 32;
    checkDRM(drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create), "CREATE_DUMB");
    buffer.handle = create.handle;
    buffer.pitch = create.pitch;
    buffer.size = create.size;
    uint32_t handles[4]{buffer.handle}, pitches[4]{buffer.pitch}, offsets[4]{};
    checkDRM(drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888,
                         handles, pitches, offsets, &buffer.fb, 0), "drmModeAddFB2");
    drm_mode_map_dumb map{};
    map.handle = buffer.handle;
    checkDRM(drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map), "MAP_DUMB");
    buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (buffer.map == MAP_FAILED) checkDRM(-1, "mmap scanout buffer");
    std::memset(buffer.map, 0, buffer.size);
  }

  void initialize(int width, int height, bool full_frame) {
    std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources(
        drmModeGetResources(fd), drmModeFreeResources);
    if (!resources) checkDRM(-1, "drmModeGetResources");
    for (int i = 0; i < resources->count_connectors; ++i) {
      std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> conn(
          drmModeGetConnector(fd, resources->connectors[i]), drmModeFreeConnector);
      if (!conn || conn->connector_type != DRM_MODE_CONNECTOR_Composite ||
          conn->connection != DRM_MODE_CONNECTED || !conn->encoder_id) continue;
      std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)> encoder(
          drmModeGetEncoder(fd, conn->encoder_id), drmModeFreeEncoder);
      if (!encoder || !encoder->crtc_id) continue;
      saved = drmModeGetCrtc(fd, encoder->crtc_id);
      connector = conn->connector_id;
      break;
    }
    if (!saved || !saved->mode_valid)
      throw std::runtime_error("DRM requires an active Composite connector and CRTC.");
    mode = saved->mode;
    if (!(mode.flags & DRM_MODE_FLAG_INTERLACE) || mode.hdisplay != width ||
        mode.vdisplay != height || width != 720 || (height != 480 && height != 576))
      throw std::runtime_error("DRM composite must use the boot-selected 720x480i or 720x576i mode.");
    if (!mode.clock || !mode.htotal || !mode.vtotal || mode.vscan > 1 ||
        (mode.flags & DRM_MODE_FLAG_DBLSCAN))
      throw std::runtime_error("Unsupported DRM composite timing.");
    frame_us = 1000.0 * mode.htotal * mode.vtotal / mode.clock;
    const double expected = height == 576 ? 40000.0 : 100100.0 / 3.0;
    if (std::abs(frame_us - expected) > expected * .01)
      throw std::runtime_error("Composite refresh does not match PAL/NTSC PCM timing.");
    if (full_frame) {
      if (height != 480 || mode.clock != 13500 || mode.htotal != 858 ||
          mode.vtotal != 525)
        throw std::runtime_error("--kms-full-frame requires NTSC 720x480i, 13.5 MHz, 858x525 total timing.");
      // 246 rows/field: one control row and all 245 data rows. Retain
      // pixel clock, horizontal timing and full-frame total. The adjusted
      // VC4 timings are active=246, front=9, sync=3, back=4 per field;
      // four is the driver's minimum back porch for 525-line modes.
      // This removes clipping but does not prove analogue line/field phase.
      mode.vdisplay = 492;
      mode.vsync_start = 510;
      mode.vsync_end = 516;
      mode.type = DRM_MODE_TYPE_USERDEF;
      std::snprintf(mode.name, sizeof(mode.name), "720x492i-PCM");
      height = mode.vdisplay;
      std::fprintf(stderr,
          "\nKMS experimental full frame: 720x492i, vertical timings 492 510 516 525; "
          "246 rows/field, no PCM padding. Analogue line/field phase is unverified.\n");
    }
    bool found = false;
    for (int i = 0; i < resources->count_crtcs; ++i) {
      if (resources->crtcs[i] == saved->crtc_id) { pipe = i; found = true; break; }
    }
    if (!found) throw std::runtime_error("Cannot find composite CRTC index.");
    for (auto &buffer : buffers) allocate(buffer, width, height);
    checkDRM(drmModeSetCrtc(fd, saved->crtc_id, buffers[0].fb, 0, 0,
                          &connector, 1, &mode), "drmModeSetCrtc");
    changed = true;

    // Measure actual kernel event spacing while displaying black. A nominal
    // 60 Hz mode can expose field or frame events; do not guess from its name.
    std::vector<double> samples;
    unsigned smallest_step = UINT_MAX;
    std::fprintf(stderr, "\nKMS calibration: mode=%.3f us/image, CRTC=%u (index=%u)\n",
        frame_us, saved->crtc_id, pipe);
    if (!waitTick(currentTick() + 1)) return;
    auto previous = event;
    for (unsigned i = 0; i < 8; ++i) {
      if (!waitTick(currentTick() + 1)) return;
      const int32_t delta = kms::distance(event.sequence, previous.sequence);
      std::fprintf(stderr,
          "KMS vblank sample %u: sequence=%u -> %u, timestamp=%llu -> %llu us\n",
          i + 1, previous.sequence, event.sequence,
          static_cast<unsigned long long>(previous.us),
          static_cast<unsigned long long>(event.us));
      if (delta <= 0 || event.us <= previous.us)
        throw std::runtime_error("Invalid DRM vblank calibration timestamps.");
      samples.push_back(double(event.us - previous.us) / delta);
      smallest_step = std::min(smallest_step, static_cast<unsigned>(delta));
      previous = event;
    }
    ticks = kms::ticksPerFrame(samples, frame_us);
    // Some drivers count fields but deliver events only once per full frame.
    // Waiting for an intermediate field on those drivers would halve playback.
    field_events = kms::hasFieldEvents(ticks, smallest_step);
    // An actual completed flip is the phase anchor, not a wall-clock epoch.
    if (!flip(1)) return;
    last_flip = event;
    std::fprintf(stderr,
        "\nKMS sync: %ux%u interlaced, %.3f ms/image, %u counter ticks/image; "
        "events=%s (step=%u); flip anchor=%u. Physical odd/even field is not reported by DRM.\n",
        mode.hdisplay, mode.vdisplay, frame_us / 1000, ticks,
        field_events ? "field" : "frame", smallest_step, last_flip.sequence);
  }

  void report(kms::Stamp previous, unsigned frames, bool enabled) {
    if (frames > 1)
      std::fprintf(stderr, "\nKMS underrun: repeated the previous image for %u extra frame(s).\n", frames - 1);
    if (!enabled) return;
    if (!stats_start) stats_start = previous.us;
    const double gap = (event.us - previous.us) / 1000.0;
    min_ms = std::min(min_ms, gap);
    max_ms = std::max(max_ms, gap);
    ++intervals;
    repeats += frames - 1;
    const double elapsed = (event.us - stats_start) / 1000000.0;
    if (elapsed >= 5) {
      std::fprintf(stderr,
          "\nKMS timing: %.3f completed flips/s; gap min=%.3f max=%.3f ms; "
          "repeated=%u; sequence=%u timestamp=%llu us\n",
          intervals / elapsed, min_ms, max_ms, repeats, event.sequence,
          static_cast<unsigned long long>(event.us));
      stats_start = event.us;
      intervals = repeats = 0;
      min_ms = std::numeric_limits<double>::max(); max_ms = 0;
    }
  }

  ~Scanout() {
    if (fd < 0) return;
    try {
      if (pending) wait(false);
      // Let the last PCM image finish scanning before restoring the console.
      if (have_pcm && ticks && !(stopping && stopping()) &&
          kms::distance(currentTick(), last_flip.sequence + ticks) < 0)
        waitTick(last_flip.sequence + ticks, false);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "\nDRM cleanup: %s\n", error.what());
    }
    if (changed && drmModeSetCrtc(fd, saved->crtc_id, saved->buffer_id,
                                 saved->x, saved->y, &connector, 1, &saved->mode) < 0)
      std::fprintf(stderr, "\nCould not restore DRM CRTC: %s\n", std::strerror(errno));
    for (auto &buffer : buffers) {
      if (buffer.fb) drmModeRmFB(fd, buffer.fb);
      if (buffer.map != MAP_FAILED) munmap(buffer.map, buffer.size);
      if (buffer.handle) {
        drm_mode_destroy_dumb destroy{};
        destroy.handle = buffer.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      }
    }
    if (saved) drmModeFreeCrtc(saved);
  }
};

KMSDisplayConsumer::KMSDisplayConsumer(int left_offset, int right_offset,
                                     int height_mod, bool display_stats,
                                     std::function<bool()> stopping, bool full_frame,
                                     bool pcm_levels)
    : left_offset(left_offset), right_offset(right_offset), height_mod(height_mod),
      display_stats(display_stats), full_frame(full_frame), pcm_levels(pcm_levels),
      stopping(std::move(stopping)) {}

KMSDisplayConsumer::~KMSDisplayConsumer() = default;

void KMSDisplayConsumer::InitRenderer(int width, int height) {
  const char *driver = SDL_GetCurrentVideoDriver();
  if (!driver || std::string(driver) != "KMSDRM")
    throw std::runtime_error("Raspberry Pi playback requires SDL_VIDEODRIVER=kmsdrm.");
  this->width = width;
  this->heigth = height;
  if (full_frame && height_mod != 0)
    throw std::runtime_error("--kms-full-frame cannot vertically rescale PCM rows.");
  if (left_offset < 0 || right_offset < 0 ||
      static_cast<int64_t>(left_offset) + right_offset >= width)
    throw std::runtime_error("Left and right offsets must leave a positive display width.");
  window = SDL_CreateWindow("PCM", SDL_WINDOWPOS_UNDEFINED_DISPLAY(0),
                            SDL_WINDOWPOS_UNDEFINED_DISPLAY(0), width, height,
                            SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window)
    throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
  SDL_ShowCursor(SDL_DISABLE);
  SDL_SysWMinfo info{};
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_KMSDRM)
    throw std::runtime_error(std::string("SDL KMS device access: ") + SDL_GetError());
  scanout = std::make_unique<Scanout>();
  scanout->fd = info.info.kmsdrm.drm_fd;
  scanout->stopping = stopping;
  if (scanout->fd < 0) throw std::runtime_error("SDL did not supply a DRM file descriptor.");
  scanout->initialize(width, height, full_frame);
  this->heigth = scanout->mode.vdisplay;
}

void KMSDisplayConsumer::renderFrame(const IFrame &frame) {
  if (stopping && stopping()) return;
  auto &out = *scanout;
  if (full_frame && (frame.width() != 139 || frame.heigth() != heigth))
    throw std::runtime_error("Full-frame scanout requires an unpadded 139x492 PCM frame.");
  auto pixels = pcm_levels ? frame.render(pcm_high, pcm_white) : frame.render();
  if (pcm_levels) raisePCMDataZero(pixels);
  const int64_t dest_height = static_cast<int64_t>(frame.heigth()) + height_mod;
  if (frame.width() <= 0 || frame.heigth() <= 0 || dest_height <= 0 || dest_height > INT32_MAX)
    throw std::runtime_error("Height modifier must leave a valid positive frame height.");
  const int dest_width = width - left_offset - right_offset;
  const int next = 1 - out.front;
  auto &buffer = out.buffers[next];
  std::memset(buffer.map, 0, buffer.size);
  // Nearest-neighbour pixel-centre sampling matches the former SDL texture.
  // No vertical rescaling unless explicitly requested with --heigth_mod.
  std::array<int, 720> source_x{};
  for (int x = 0; x < dest_width; ++x)
    source_x[x] = (int64_t(2 * x + 1) * frame.width()) / (2 * dest_width);
  for (int y = 0; y < std::min<int64_t>(heigth, dest_height); ++y) {
    const int sy = (int64_t(2 * y + 1) * frame.heigth()) / (2 * dest_height);
    auto *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(buffer.map) + y * buffer.pitch);
    for (int x = 0; x < dest_width; ++x) {
      const uint32_t value = pixels.pixels[size_t(sy) * frame.width() + source_x[x]];
      row[left_offset + x] = value * 0x010101u;
    }
  }
  if (pcm_levels && !out.have_pcm)
    std::fprintf(stderr,
        "\nKMS experimental PCM levels: blank=0, data-zero=36, data-one=146, "
        "white=255 (RGB codes; analogue voltages unverified).\n");
  if (display_stats && !out.have_pcm)
    std::fprintf(stderr, "\nKMS geometry: source=%dx%d, draw=%dx%lld+%d+0, screen=%dx%d\n",
        frame.width(), frame.heigth(), dest_width, static_cast<long long>(dest_height),
        left_offset, width, heigth);

  const uint32_t now = out.currentTick();
  const uint32_t submit = out.field_events
      ? kms::submissionTick(out.last_flip.sequence, now, out.ticks) : now;
  if (kms::distance(submit, now) > 0 && !out.waitTick(submit)) return;
  if (!out.flip(next)) return;
  const unsigned frames = kms::completedFrames(out.last_flip, out.event, out.ticks, out.frame_us);
  if (out.have_pcm) out.report(out.last_flip, frames, display_stats);
  out.last_flip = out.event;
  out.have_pcm = true;
}
