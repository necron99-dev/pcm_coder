#include "PCMPreroll.h"
#include "pcmframe.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

bool playSilenceUntilEnter(IConsumer<SamplesPack> &encoder, bool pal,
                          const std::function<bool()> &stopping, int input_fd) {
  const int rows = (pal ? PCMFrame::PAL_HEIGTH : PCMFrame::NTSC_HEIGTH) -
                   PCMFrame::HEADER_SIZE_LINES;
  // One image's worth of stereo samples, bypassing quantization/dither so
  // the lead-in is digital silence even when the WAV will use dithering.
  const SamplesPack silence{
      std::vector<SampleGenerator::o_samples_format>(rows * PCMLine::TotalDataLRSamples)};
  bool prompted = false;
  while (!stopping()) {
    encoder.Ressive(silence);
    if (stopping()) return false;
    if (!prompted) {
      std::cerr << "\nPCM silence is running. Apply TweakVec if needed, then press Enter "
                   "to start the audio file (Ctrl+C to quit).\n";
      prompted = true;
    }

    pollfd input{input_fd, POLLIN, 0};
    const int ready = poll(&input, 1, 0);
    if (ready < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("Waiting for Enter: ") + std::strerror(errno));
    }
    if (!ready) continue;
    if (input.revents & (POLLERR | POLLNVAL))
      throw std::runtime_error("--wait-for-enter cannot read standard input.");
    if (input.revents & (POLLIN | POLLHUP)) {
      char keys[256];
      const auto count = read(input_fd, keys, sizeof(keys));
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        throw std::runtime_error(std::string("Reading Enter: ") + std::strerror(errno));
      }
      if (!count)
        throw std::runtime_error("Standard input closed before Enter; audio file not started.");
      for (ssize_t i = 0; i < count; ++i)
        if (keys[i] == '\n') return !stopping();
    }
  }
  return false;
}
