# PCM Encoder

Convert audio files into PAL or NTSC video carrying PCM audio data. Save the
result as a video file, preview it with sound on a desktop, or send it through
a Raspberry Pi's composite output to a hardware PCM decoder.

The Raspberry Pi setup targets **Pi 3B+ with Raspberry Pi OS Lite (Trixie)**.
See the [Pi setup guide](docs/raspberry-pi-os-lite.md) for composite wiring,
boot configuration, playback, and troubleshooting.

**Live composite playback through the new KMS backend is experimental.**
Builds and file encoding have been checked in a Debian Trixie container;
interlaced timing, scan-line alignment, and decoder lock still need testing
on a physical Pi and PCM decoder.

## Features

- Read audio formats supported by FFmpeg, including audio tracks in video files.
- Generate PAL or NTSC PCM video with 14-bit or 16-bit audio encoding.
- Configure dithering, parity, Q generation, and the copy-protection bit.
- Save video using FFmpeg, with a selectable codec and bitrate.
- Preview video and sound using SDL2 and PortAudio.
- Play composite video from a Pi's local text console using SDL2/KMSDRM.
- Crop scan lines from the top and bottom of the image.

## Build on Raspberry Pi OS Lite or Debian Linux

You need Git, CMake 3.16 or newer, a C++17 compiler, and FFmpeg development
libraries. Playback also requires SDL2 and PortAudio. The compatibility checks
used GCC 14, FFmpeg 7.1, and SDL2 2.32 on Debian Trixie.

Install dependencies:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  ffmpeg libsdl2-dev portaudio19-dev \
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
`libsdl2-dev` and `portaudio19-dev` from the dependency installation and build
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
to enable composite output and select PAL or NTSC. Then, from the Pi's local
text console:

```sh
SDL_VIDEODRIVER=kmsdrm ./build/src/pcm_coder -R --crop-top 0 input.wav
```

`-R` detects PAL or NTSC from the display dimensions. Use it without an output
filename or `--pal`/`--ntsc`. This mode carries audio in the composite PCM
image and does not open a separate audio playback device. Press Ctrl+C to stop.

Start with `--crop-top 0` and adjust scan-line alignment for your decoder.
The KMS backend preserves vertical scan lines when cropping and clips the
image at the display edges. The setup guide describes the remaining timing
limitations and display-access requirements.

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
```

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
