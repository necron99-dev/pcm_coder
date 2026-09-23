# PCM Encoder

Convert audio files into PAL or NTSC video carrying PCM audio data. Save the
result as a video file, preview it with sound on a desktop, or send it through
a Raspberry Pi's composite output to a hardware PCM decoder.

The Raspberry Pi setup targets **Pi 3B+ with Raspberry Pi OS Lite (Trixie)**.
See the [Pi setup guide](docs/raspberry-pi-os-lite.md) for composite wiring,
boot configuration, playback, and troubleshooting.

**KMS composite playback has been reported working on a Pi 3B+ with an NTSC
Sony PCM-501ES**, using default 14-bit encoding and `--left_offset 9`.
Direct 14-bit monitoring has been reported clean after console/CPU isolation
changes. Earlier recordings through a Sony SLV-R1000 dropped out; direct Pi-to-VCR
recordings initially failed to lock reliably, including with monochrome output. Native
PCM-501ES recordings play cleanly on that deck. On the same setup, 16-bit playback
produces music with substantial noise and remains unresolved.
A later live comparison reported that lock stayed with `--left_offset 10
--right_offset 0 --crop-top 8 --crop-bot 45` (710-pixel draw width), after a
703-pixel-wide comparison lost lock when the audio changed. Direct Pi-to-VHS
recording with this newer geometry was then reported substantially improved
and close to clean, but still imperfect. The remaining glitches are unresolved.
An opt-in [full-frame scanout experiment](docs/raspberry-pi-os-lite.md#experimental-full-frame-ntsc-scanout)
removes the known PCM row clipping, but the Pi/PCM-501ES test reported no lock
and worse results after trying horizontal offsets. Leave `--kms-full-frame`
disabled on this setup; the recording problem remains unresolved.
An independent, opt-in [PCM video-level test](docs/raspberry-pi-os-lite.md#experimental-pcm-video-levels)
raises data-zero above blanking while preserving the selected geometry.
Its analogue levels and effect on OVC have not yet been verified on the Pi.
Builds and file checks run in a Debian Trixie
container; passing file tests does not establish hardware decoding quality.

## Features

- Read audio formats supported by FFmpeg, including audio tracks in video files.
- Generate PAL or NTSC PCM video with 14-bit or 16-bit audio encoding.
- Configure dithering, parity, Q generation, and the copy-protection bit.
- Save video using FFmpeg, with a selectable codec and bitrate.
- Preview video and sound using SDL2 and PortAudio.
- Play composite video from a Pi's local text console using SDL2/KMSDRM.
- Start KMS with PCM silence and press Enter to begin the audio file using
  [`--wait-for-enter`](docs/raspberry-pi-os-lite.md#start-with-pcm-silence-before-playing-the-file).
- Crop scan lines from the top and bottom of the image.

## Build on Raspberry Pi OS Lite or Debian Linux

You need Git, CMake 3.16 or newer, a C++17 compiler, and FFmpeg development
libraries. Playback also requires SDL2 (2.0.15 or newer), libdrm on Linux,
and PortAudio. The compatibility checks
used GCC 14, FFmpeg 7.1, and SDL2 2.32 on Debian Trixie.

Install dependencies:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  ffmpeg libsdl2-dev libdrm-dev portaudio19-dev \
  libavcodec-dev libavformat-dev libavdevice-dev libavfilter-dev \
  libavutil-dev libswresample-dev libswscale-dev
```

Clone the repository and its pinned dependencies:

```sh
git clone --recurse-submodules https://github.com/necron99-dev/pcm_coder.git
cd pcm_coder
```

For an existing checkout, initialize the dependencies with:

```sh
git submodule update --init --recursive
```

Configure and build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/src/pcm_coder --help
```

Using two compiler jobs keeps memory use lower on the Pi 3B+. The executable
is `build/src/pcm_coder`. All examples below run it from the repository root.

Optional system-wide installation:

```sh
sudo cmake --install build
```

After installation, you can use `pcm_coder` directly.

### Build only the file encoder

File encoding works over SSH without a display. To disable playback, omit
`libsdl2-dev`, `libdrm-dev`, and `portaudio19-dev` from the dependency installation and build
with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_PLAYER=OFF
cmake --build build -j2
```

An encoding-only build requires both input and output filenames.

## Usage

### Encode a video file

PAL with the default uncompressed video codec:

```sh
./build/src/pcm_coder --pal input.wav output.avi
```

NTSC with 16-bit audio encoding:

```sh
./build/src/pcm_coder --ntsc --16 input.flac output.avi
```

The defaults are PAL, 14-bit audio, and `rawvideo`. Dithering, parity, and Q
generation are enabled by default; Q generation applies to 14-bit mode.

To select an FFmpeg video encoder and bitrate, use `-c` and `-b` with this
syntax, replacing `CODEC` and `BITRATE` with your values:

```text
./build/src/pcm_coder --pal -c CODEC -b BITRATE input.wav output.avi
```

The bitrate is specified in bits per second. Lossy video compression can
damage the encoded PCM data; use the default uncompressed output when
preserving the signal is the priority.

### Play through Raspberry Pi composite output

First follow the [Pi 3B+ / Raspberry Pi OS Lite guide](docs/raspberry-pi-os-lite.md)
to enable composite output and select PAL or NTSC. This command has been
reported working with the **NTSC Sony PCM-501ES**:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --crop-top 0 --left_offset 9 input.wav
```

`-R` detects PAL or NTSC from the display dimensions. Use it without an output
filename or `--pal`/`--ntsc`. This mode carries audio in the composite PCM
image and does not open a separate audio playback device. Press Ctrl+C to stop.

This uses 14-bit encoding, P/Q correction, a zero right offset, and no field
swap. On a 720-pixel display, the left offset both shifts the image and narrows
it to 711 pixels. The KMS backend preserves vertical scan lines when cropping
and clips at the display edges. See the setup guide for test limitations,
diagnostics, and console/SSH display access. Presentation waits for DRM
page-flip completion and follows measured vblank events instead of a software
timer. Each completed flip is checked against the established relative frame
phase; DRM does not identify physical odd/even fields.

### Preview on a desktop

With playback support built, omit the output filename:

```sh
./build/src/pcm_coder input.wav
```

This opens a video window and plays sound. It requires a graphical session
and a working audio output device. Use `-R` for composite playback from a Lite
console.

### Common options

| Option | Purpose |
| --- | --- |
| `--pal` / `--ntsc` | Select the video standard for encoding or desktop preview. |
| `--14` / `--16` | Select the PCM audio bit width. |
| `--swap-fields` | Exchange the PCM image fields before cropping, for field-order diagnostics. |
| `--display-stats` | Report geometry, DRM flip timestamps, and repeated frames in KMS Pi mode. |
| `--kms-full-frame` | Experimental NTSC 720×492i scanout with all PCM rows; requires `-R`, excludes cropping and height scaling. |
| `--kms-pcm-levels` | Experimental PCM data-zero/high RGB codes of 36/146, with blanking 0 and white reference 255; requires `-R`. |
| `--no-dither` | Disable dithering when converting to 14-bit audio. |
| `--no-parity` | Disable parity generation. |
| `--no-q` | Disable Q generation in 14-bit mode. |
| `--copy-protection` | Set the copy-protection bit. |
| `-c CODEC` | Select an FFmpeg video encoder; requires an output filename. |
| `-b BITRATE` | Set video bitrate; requires `-c` and an output filename. |
| `--crop-top N` / `--crop-bot N` | Remove scan lines from the top or bottom. |
| `-R` | Enable Raspberry Pi composite playback. |
| `--left_offset N` / `--right_offset N` | Adjust horizontal margins in Pi mode. |
| `--heigth_mod N` | Adjust image height in Pi mode; keep at zero to preserve vertical scan lines. |
| `--help` | Show all options available in the current build. |

`--heigth_mod` retains its original spelling for command-line compatibility.

## Verification

The smoke checks generate test audio, encode PAL and NTSC video, inspect and
decode the output with FFmpeg, and check argument/error handling. They require
Python 3 and the `ffmpeg` command-line tools:

```sh
sudo apt install -y python3
python3 tests/smoke.py ./build/src/pcm_coder
python3 tests/pcm_roundtrip.py ./build/src/pcm_coder
sh tests/kms_scanout.sh
```

The DRM test compiles a simulated device around the actual display consumer.
It checks PAL/NTSC cadence, field/frame event rates, sequence wraparound, late
frames, phase-loss rejection, raster geometry, cancellation, and resource cleanup.
It requires the SDL2 and libdrm development packages.

These checks run without display hardware and do not verify composite output
or hardware PCM decoding.

## Other build configurations

### Windows

The repository retains its MSVC build configuration, but it has not been
validated as part of the Trixie compatibility work. It requires a C++17-capable
Visual Studio toolchain, CMake, Git, and matching FFmpeg headers and import
libraries. The old FFmpeg 4.2 download instructions no longer match the audio
reader's channel-layout API.

For an encoding-only build, the starting configuration is:

```powershell
cmake -S . -B build -A x64 -DENABLE_PLAYER=OFF -DCMAKE_PREFIX_PATH="C:/ffmpeg"
cmake --build build --config Release
```

The FFmpeg prefix should contain `include`, `lib`, and `bin` directories.
If discovery fails, set the `LIBAVCODEC`, `LIBAVDEVICE`, `LIBAVFORMAT`,
`LIBAVUTIL`, `LIBSWRESAMPLE`, and `LIBSWSCALE` `_INCLUDE_DIR` and `_LIBRARIES`
cache entries in CMake GUI to the corresponding headers and import libraries.

The executable is `build/src/Release/pcm_coder.exe`. Make the DLLs from the
same FFmpeg build available on `PATH` or beside the executable. Windows
playback uses older SDL2 and PortAudio dependency scripts and needs separate
validation.

### Legacy Raspberry Pi graphics

For older systems that still provide DispmanX and `/opt/vc`, configure with
`-DENABLE_LEGACY_RPI=ON`. This selects the original display backend and builds
the `rpi-fb-shifter` utility. The legacy `--vsync_delay` option is available
only in that configuration.

Use the default KMS backend on Trixie. Do not run `rpi-fb-shifter` with KMS:
it writes display registers directly while the kernel owns the display.
