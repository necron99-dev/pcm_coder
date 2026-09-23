// Real PCM encoder with a pipe standing in for terminal input; no DRM needed.
#include "PCMPreroll.h"
#include "PCMFrmageStage.h"
#include "LineGeneratorStage.h"
#include "pcmframe.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

struct Pipe {
  int fd[2];
  Pipe() { assert(pipe(fd) == 0); }
  ~Pipe() { close(fd[0]); if (fd[1] >= 0) close(fd[1]); }
};

struct Frames : IConsumer<IFrame> {
  std::vector<PCMLine> rows;
  std::function<void(unsigned)> onFrame;
  unsigned frames = 0;
  void Ressive(const IFrame &image) override {
    assert(!image.Eof()); // Waiting must never flush/end the encoder.
    const auto &frame = dynamic_cast<const PCMFrame &>(image);
    for (int y = 0; y < frame.dataHeigth(); ++y) {
      const auto &line = *frame.getLine(y);
      assert(*line.pCRC() == line.generateCRC());
      rows.push_back(line);
    }
    // Silence still carries sync and white reference, rather than a black image.
    auto pixels = image.render();
    for (int y = 0; y < pixels.heigth(); ++y) {
      const auto *row = pixels.pixels.data() + y * pixels.width();
      assert(row[1] == 150 && row[3] == 150);
      assert(row[134] == 255 && row[137] == 255);
    }
    ++frames;
    if (onFrame) onFrame(frames);
  }
};

void checkTransition(bool pal, bool bits14) {
  Pipe input;
  LineGeneratorStage lines(bits14, true, bits14);
  auto *frames = new Frames;
  lines.NextStage(new PCMFrameStage(bits14, true, bits14, false, pal))
      .NextConsumer(frames);
  frames->onFrame = [&](unsigned n) {
    // Non-newline input must not start playback.
    if (n == 1) assert(write(input.fd[1], "x", 1) == 1);
    if (n == 3) assert(write(input.fd[1], "\n", 1) == 1);
    assert(n <= 3);
  };
  assert(playSilenceUntilEnter(lines, pal, [] { return false; }, input.fd[0]));
  assert(frames->frames == 3);
  for (const auto &row : frames->rows)
    for (unsigned c = 0; c < 8; ++c) assert(row.getData()[c] == 0);
  frames->onFrame = {};

  const size_t frameRows = pal ? 588 : 490;
  const size_t silentSamples = 3 * frameRows * 3;
  std::vector<SampleGenerator::o_samples_format> music(frameRows * 3);
  for (size_t i = 0; i < music.size(); ++i) {
    music[i].L = static_cast<int16_t>((i * 29 + 1) % 8000);
    music[i].R = static_cast<int16_t>(-1 - static_cast<int>((i * 17) % 8000));
  }
  const auto expected = music;
  lines.Ressive(SamplesPack{std::move(music)});
  // Continue the same stream to emit the interleaver's trailing music samples.
  lines.Ressive(SamplesPack{std::vector<SampleGenerator::o_samples_format>(frameRows * 3)});
  const unsigned mask = bits14 ? 0x3fff : 0xffff;
  for (size_t sample = 0; sample < silentSamples + expected.size(); ++sample) {
    for (size_t channel = 0; channel < 2; ++channel) {
      const size_t column = (sample % 3) * 2 + channel;
      const auto &row = frames->rows.at(sample / 3 + 16 * column);
      unsigned actual = row.getData()[column] & 0x3fff;
      if (!bits14) actual = (actual << 2) | ((row.getData()[7] >> ((6 - column) * 2)) & 3);
      const unsigned want = sample < silentSamples ? 0 :
          static_cast<uint16_t>(expected[sample - silentSamples].all[channel]) & mask;
      assert(actual == want); // No lost, duplicated or reordered start samples.
    }
  }
}

struct SilentPackets : IConsumer<SamplesPack> {
  std::function<void()> onPacket;
  unsigned packets = 0;
  void Ressive(const SamplesPack &packet) override {
    assert(!packet.eof());
    for (const auto &sample : packet.data) assert(sample.L == 0 && sample.R == 0);
    ++packets;
    if (onPacket) onPacket();
  }
};

int main() {
  for (bool pal : {false, true})
    for (bool bits14 : {false, true}) checkTransition(pal, bits14);
  {
    Pipe input;
    SilentPackets packets;
    bool stopped = false;
    packets.onPacket = [&] { stopped = true; };
    assert(!playSilenceUntilEnter(packets, false, [&] { return stopped; }, input.fd[0]));
    assert(packets.packets == 1);
    assert(!playSilenceUntilEnter(packets, false, [] { return true; }, input.fd[0]));
    assert(packets.packets == 1);
  }
  {
    Pipe input;
    close(input.fd[1]); input.fd[1] = -1;
    SilentPackets packets;
    bool rejected = false;
    try { playSilenceUntilEnter(packets, false, [] { return false; }, input.fd[0]); }
    catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
  }
  std::cout << "PASS: PCM silence, Enter gating, cancellation, EOF, CRC and exact audio transition (PAL/NTSC, 14/16 bit)\n";
}
