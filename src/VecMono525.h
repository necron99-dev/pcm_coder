#pragma once

#include <memory>
#include <string>

// Native equivalent of TweakVec --preset MONO525 --sync-adj 7.
// Construct after KMS has enabled composite; restore before its CRTC teardown.
class VecMono525 {
public:
  explicit VecMono525(
      const std::string &device_tree = "/sys/firmware/devicetree/base",
      const std::string &memory_device = "/dev/mem");
  ~VecMono525();
  VecMono525(const VecMono525 &) = delete;
  VecMono525 &operator=(const VecMono525 &) = delete;
  void apply();
  void restore() noexcept;

private:
  struct Context;
  std::unique_ptr<Context> ctx;
};
