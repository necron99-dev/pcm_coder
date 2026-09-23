// Exercise the real MMIO implementation on a file-backed device and fake DT.
#include "VecMono525.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using Words = std::vector<uint32_t>;

struct Fixture {
  fs::path root, dt, mem, vec, pv;
  explicit Fixture(bool pi4, uint32_t seed) {
    char path[] = "/tmp/pcm-vec-test-XXXXXX";
    const auto dir = mkdtemp(path); assert(dir);
    root = dir; dt = root / "dt"; mem = root / "mem";
    vec = dt / "soc/vec@7e801000"; pv = dt / "soc/pixelvalve@7e802000";
    fs::create_directories(vec); fs::create_directories(pv);
    fs::create_directory(dt / "__symbols__");
    text(dt / "__symbols__/vec", "/soc/vec@7e801000");
    text(dt / "__symbols__" / (pi4 ? "pixelvalve3" : "pixelvalve2"),
         "/soc/pixelvalve@7e802000");
    text(vec / "compatible", pi4 ? "brcm,bcm2711-vec" : "brcm,bcm2835-vec");
    text(pv / "compatible", pi4 ? "brcm,bcm2711-pixelvalve3" : "brcm,bcm2835-pixelvalve2");
    property(dt / "#address-cells", {pi4 ? 2u : 1u});
    property(dt / "soc/#address-cells", {1});
    property(dt / "soc/#size-cells", {1});
    // Translate bus addresses to offsets in our small backing file.
    property(dt / "soc/ranges", pi4 ? Words{0x7e800000, 0, 0, 0x4000} :
                                             Words{0x7e800000, 0, 0x4000});
    property(vec / "reg", {0x7e801000, 0x1000});
    property(pv / "reg", {0x7e802000, 0x100});
    Words contents(0x4000 / 4, seed);
    contents[(0x1000 + 0x104) / 4] &= ~0x00200003u;
    contents[(0x1000 + 0x18c) / 4] &= ~0x8000u;
    contents[(0x1000 + 0x208) / 4] = 8;
    contents[0x2000 / 4] = 8;
    std::ofstream out(mem, std::ios::binary);
    out.write(reinterpret_cast<const char *>(contents.data()), contents.size() * 4);
    assert(out.good());
  }
  ~Fixture() { fs::remove_all(root); }
  static void text(const fs::path &path, const std::string &value) {
    std::ofstream out(path, std::ios::binary); out.write(value.c_str(), value.size() + 1);
    assert(out.good());
  }
  static void property(const fs::path &path, const Words &values) {
    std::ofstream out(path, std::ios::binary);
    for (auto v : values) for (int shift = 24; shift >= 0; shift -= 8) out.put(v >> shift);
    assert(out.good());
  }
  Words read() const {
    Words data(0x4000 / 4);
    std::ifstream in(mem, std::ios::binary);
    in.read(reinterpret_cast<char *>(data.data()), data.size() * 4);
    assert(in.good()); return data;
  }
  void word(unsigned offset, uint32_t value) {
    std::fstream out(mem, std::ios::in | std::ios::out | std::ios::binary);
    out.seekp(offset); out.write(reinterpret_cast<char *>(&value), 4); assert(out.good());
  }
};

int main() {
  constexpr std::array<unsigned, 8> offsets{0x104, 0x180, 0x184, 0x188, 0x18c, 0x198, 0x1a0, 0x208};
  // Golden values from TweakVec's MONO525 + sync_adj=7 applied to both seeds.
  // Includes preserved bits, inactive frequency defaults and normal-picture CFG.
  constexpr std::array<uint32_t, 8> zero{0x1c0, 0x29c7, 0x1c72, 0x1c00, 0x7000, 0x284bda13, 2, 10};
  constexpr std::array<uint32_t, 8> seeded{0xa08485e4, 0x29c7, 0x1c72, 0xa5a4bda0,
                                       0xa5a575a5, 0x284bda13, 0xa5a5a5a6, 10};
  for (bool pi4 : {false, true}) for (uint32_t seed : {0u, 0xa5a5a5a5u}) {
    Fixture f(pi4, seed);
    const auto before = f.read();
    auto expected = before;
    for (size_t i = 0; i < offsets.size(); ++i)
      expected[(0x1000 + offsets[i]) / 4] = (seed ? seeded : zero)[i];
    {
      VecMono525 profile(f.dt, f.mem);
      assert(f.read() == before); // Construction never changes hardware.
      profile.apply(); assert(f.read() == expected); // Also checks all PV words.
      profile.apply(); assert(f.read() == expected);
      profile.restore(); assert(f.read() == before);
      profile.restore(); assert(f.read() == before);
      profile.apply(); assert(f.read() == expected);
    }
    assert(f.read() == before); // Normal teardown restores the original snapshot.
    try {
      VecMono525 profile(f.dt, f.mem); profile.apply();
      throw std::runtime_error("simulate later KMS startup failure");
    } catch (const std::runtime_error &) {}
    assert(f.read() == before);
  }
  // Invalid hardware/mode/DT must fail without making any register changes.
  for (int invalid = 0; invalid < 10; ++invalid) {
    Fixture f(false, 0);
    switch (invalid) {
    case 0: f.word(0x1104, 1); break; // PAL
    case 1: f.word(0x118c, 0x8000); break; // Progressive
    case 2: f.word(0x2000, 0); break; // PixelValve not clocked from VEC
    case 3: f.word(0x1208, 0); break; // VEC disabled
    case 4: Fixture::text(f.vec / "compatible", "unsupported"); break;
    case 5: Fixture::property(f.vec / "reg", {0x7e801000, 0x100}); break;
    case 6: Fixture::property(f.dt / "soc/ranges", {0x7f000000, 0, 0x4000}); break;
    case 7: fs::remove(f.dt / "__symbols__/vec"); break;
    case 8: Fixture::property(f.dt / "soc/ranges", {0x7e800000}); break;
    case 9: Fixture::text(f.pv / "compatible", "unsupported"); break;
    }
    const auto before = f.read();
    bool failed = false;
    try { VecMono525 profile(f.dt, f.mem); profile.apply(); }
    catch (const std::runtime_error &) { failed = true; }
    assert(failed && f.read() == before);
  }
  std::puts("PASS: native MONO525 golden registers, DT translation, untouched PixelValve, restore and rejection");
}
