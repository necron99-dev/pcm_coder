#include "VecMono525.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

Bytes readProperty(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("VEC device-tree property unavailable: " + path.string());
  return Bytes(std::istreambuf_iterator<char>(input), {});
}

uint64_t cells(const Bytes &data, size_t offset, unsigned count) {
  if (count < 1 || count > 2 || offset > data.size() || count * 4 > data.size() - offset)
    throw std::runtime_error("Unsupported or truncated VEC device-tree cells.");
  uint64_t value = 0;
  for (unsigned i = 0; i < count * 4; ++i) value = (value << 8) | data[offset + i];
  return value;
}

unsigned cellCount(const fs::path &path) {
  const auto data = readProperty(path);
  if (data.size() != 4) throw std::runtime_error("Invalid VEC device-tree cell count.");
  const auto count = cells(data, 0, 1);
  if (count != 1 && count != 2) throw std::runtime_error("Unsupported VEC device-tree cell count.");
  return static_cast<unsigned>(count);
}

bool compatible(const fs::path &node, const std::string &wanted) {
  const auto data = readProperty(node / "compatible");
  const std::string list(data.begin(), data.end());
  for (size_t i = 0; i < list.size();) {
    const auto end = list.find('\0', i);
    if (end == std::string::npos) return false;
    if (list.substr(i, end - i) == wanted) return true;
    i = end + 1;
  }
  return false;
}

fs::path symbol(const fs::path &root, const char *name) {
  const auto data = readProperty(root / "__symbols__" / name);
  if (data.size() < 2 || data.front() != '/' || data.back() != 0)
    throw std::runtime_error("Invalid VEC device-tree symbol.");
  const fs::path relative(std::string(data.begin() + 1, data.end() - 1));
  for (const auto &part : relative)
    if (part == "..") throw std::runtime_error("Invalid VEC device-tree symbol path.");
  const auto node = root / relative;
  if (node.parent_path() != root / "soc")
    throw std::runtime_error("Unsupported VEC device-tree bus; expected /soc.");
  return node;
}

uint64_t physicalAddress(const fs::path &root, const fs::path &node) {
  const auto bus = root / "soc";
  const unsigned child = cellCount(bus / "#address-cells");
  const unsigned parent = cellCount(root / "#address-cells");
  const unsigned size = cellCount(bus / "#size-cells");
  const auto reg = readProperty(node / "reg");
  const uint64_t address = cells(reg, 0, child);
  if (cells(reg, child * 4, size) < 0x100)
    throw std::runtime_error("VEC device-tree register range is too small.");
  const auto ranges = readProperty(bus / "ranges");
  const size_t stride = (child + parent + size) * 4;
  if (ranges.empty() || ranges.size() % stride)
    throw std::runtime_error("Invalid VEC device-tree bus ranges.");
  for (size_t i = 0; i < ranges.size(); i += stride) {
    const auto base = cells(ranges, i, child);
    const auto host = cells(ranges, i + child * 4, parent);
    const auto span = cells(ranges, i + (child + parent) * 4, size);
    if (address >= base && address - base < span && span - (address - base) >= 0x1000) {
      const auto delta = address - base;
      if (host > UINT64_MAX - delta) break;
      return host + delta;
    }
  }
  throw std::runtime_error("Cannot translate VEC device-tree register address.");
}

struct Mapping {
  void *base = MAP_FAILED;
  size_t length = 0, delta = 0;
  ~Mapping() { if (base != MAP_FAILED) munmap(base, length); }
  void open(int fd, uint64_t address) {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || address % 4) throw std::runtime_error("Invalid VEC mapping alignment.");
    delta = address % static_cast<uint64_t>(page);
    const auto aligned = address - delta;
    if (aligned > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
      throw std::runtime_error("VEC address exceeds mmap offset range.");
    length = delta + 0x1000;
    base = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, aligned);
    if (base == MAP_FAILED)
      throw std::runtime_error(std::string("Cannot map VEC registers: ") + std::strerror(errno));
  }
  volatile uint32_t &at(unsigned offset) const {
    return *reinterpret_cast<volatile uint32_t *>(
        static_cast<uint8_t *>(base) + delta + offset);
  }
  void write(unsigned offset, uint32_t value) {
    at(offset) = value;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (at(offset) != value)
      throw std::runtime_error("VEC register readback failed at offset " + std::to_string(offset));
  }
};

// Register semantics and preset values checked against kFYatek/TweakVec
// (public domain / Unlicense): https://github.com/kFYatek/tweakvec
// These include the preset's default-frequency and picture-mode writes;
// PixelValve timing registers and the VEC half-pixel delay are untouched.
struct Setting { unsigned offset; uint32_t mask, value; };
constexpr std::array<Setting, 8> settings{{
    {0x104, 0x1f2363cb, 0x000001c0}, // NTSC, pedestal, no chroma/burst, sync on
    {0x180, UINT32_MAX, 0x000029c7}, // Default inactive custom-frequency words
    {0x184, UINT32_MAX, 0x00001c72},
    {0x188, 0x00031c45, 0x00001c00}, // Normal full-range RGB/luma, CVBS outputs
    {0x18c, 0x00007000, 0x00007000}, // sync_adj=7; preserve interlace/digital flags
    {0x198, UINT32_MAX, 0x284bda13}, // Default inactive SECAM Db frequency
    {0x1a0, 0x00000003, 0x00000002}, // Preset's standard horizontal mask
    {0x208, UINT32_MAX, 0x0000000a}, // VEC enabled, normal picture
}};
}

struct VecMono525::Context {
  Mapping vec, pv;
  std::array<uint32_t, settings.size()> saved{};
  bool applied = false;
};

VecMono525::VecMono525(const std::string &device_tree, const std::string &memory_device)
    : ctx(std::make_unique<Context>()) {
  const fs::path root(device_tree);
  const auto vec = symbol(root, "vec");
  const bool pi4 = compatible(vec, "brcm,bcm2711-vec");
  if (!pi4 && !compatible(vec, "brcm,bcm2835-vec"))
    throw std::runtime_error("--vec-mono525 requires a supported Raspberry Pi 0-4 VEC.");
  const auto pv = symbol(root, pi4 ? "pixelvalve3" : "pixelvalve2");
  if (!compatible(pv, pi4 ? "brcm,bcm2711-pixelvalve3" : "brcm,bcm2835-pixelvalve2"))
    throw std::runtime_error("Unsupported VEC PixelValve device.");
  const auto vec_address = physicalAddress(root, vec);
  const auto pv_address = physicalAddress(root, pv);
  const auto vec_reg = readProperty(vec / "reg");
  const unsigned ac = cellCount(root / "soc" / "#address-cells");
  const unsigned sc = cellCount(root / "soc" / "#size-cells");
  if (cells(vec_reg, ac * 4, sc) < 0x21c)
    throw std::runtime_error("VEC register range does not contain configuration registers.");
  const int fd = ::open(memory_device.c_str(), O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("--vec-mono525 needs read/write access to " + memory_device +
                             " (normally sudo): " + std::strerror(errno));
  try {
    ctx->vec.open(fd, vec_address);
    ctx->pv.open(fd, pv_address);
  } catch (...) { close(fd); throw; }
  close(fd);
}

void VecMono525::apply() {
  if (ctx->applied) return;
  const auto standard = ctx->vec.at(0x104) & 0x00200003;
  if ((ctx->pv.at(0) & 0xc) != 8 || !(ctx->vec.at(0x208) & 8))
    throw std::runtime_error("VEC is not active; --vec-mono525 must follow the KMS modeset.");
  if ((standard != 0 && standard != 2) || (ctx->vec.at(0x18c) & 0x8000))
    throw std::runtime_error("--vec-mono525 requires an active interlaced 525-line VEC mode.");
  for (size_t i = 0; i < settings.size(); ++i)
    ctx->saved[i] = ctx->vec.at(settings[i].offset);
  ctx->applied = true; // Also roll back partial application on readback failure.
  try {
    for (size_t i = 0; i < settings.size(); ++i) {
      const auto &s = settings[i];
      ctx->vec.write(s.offset, (ctx->saved[i] & ~s.mask) | s.value);
    }
  } catch (...) { restore(); throw; }
}

void VecMono525::restore() noexcept {
  if (!ctx->applied) return;
  for (size_t i = 0; i < settings.size(); ++i) {
    try { ctx->vec.write(settings[i].offset, ctx->saved[i]); }
    catch (const std::exception &e) { std::fprintf(stderr, "VEC cleanup: %s\n", e.what()); }
  }
  ctx->applied = false;
}

VecMono525::~VecMono525() { restore(); }
