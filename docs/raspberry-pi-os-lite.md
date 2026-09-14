# Raspberry Pi 3B+ with Raspberry Pi OS Lite

Target: Raspberry Pi OS Lite based on Debian 13 (Trixie), with the distribution's
FFmpeg 7.1 and SDL2 packages. The Pi 3B+ supports both 32-bit and 64-bit images.
Use a fresh Lite image from Raspberry Pi Imager when moving from an older OS.

The `-R` display path uses SDL2's KMSDRM backend and runs without a desktop.
It replaces the old dependency on `/opt/vc`, `bcm_host`, and DispmanX.
**Live PCM output is experimental until verified on a Pi and hardware decoder.**
A successful build or visible video does not establish decoder lock: field
order, scan-line alignment, and sustained playback timing need hardware checks.

## Build

From the repository directory:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  ffmpeg libsdl2-dev portaudio19-dev \
  libavcodec-dev libavformat-dev libavdevice-dev libavfilter-dev \
  libavutil-dev libswresample-dev libswscale-dev
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/src/pcm_coder --help
```

Two parallel compiler jobs keep memory use lower on the 1 GB Pi 3B+.
The executable is `build/src/pcm_coder`. Optional installation:

```sh
sudo cmake --install build
```

File-encoding smoke checks (requires `python3` and `ffmpeg`):

```sh
python3 tests/smoke.py ./build/src/pcm_coder
```

To build only the file encoder, configure with `-DENABLE_PLAYER=OFF`.
SDL2 and PortAudio development packages are then unnecessary. File encoding
works over SSH and needs no display or composite configuration:

```sh
./build/src/pcm_coder --pal input.wav output.avi
```

The default codec is uncompressed video; lossy video compression can damage
the PCM signal.

## Enable composite on the Pi 3B+

Connect a Raspberry Pi-compatible 3.5 mm AV cable and disconnect HDMI.
In `/boot/firmware/config.txt`, change the existing KMS overlay line to:

```ini
dtoverlay=vc4-kms-v3d,composite
```

Keep the full KMS driver. Do not switch to `vc4-fkms-v3d` or follow old
`tvservice` instructions. For PAL, append this parameter to the existing
**single line** in `/boot/firmware/cmdline.txt`, separated by a space:

```text
vc4.tv_norm=PAL
```

For NTSC use `vc4.tv_norm=NTSC` instead. Preserve the other boot parameters,
including `root=...`. Reboot, then verify that the console appears on the
composite display. The player expects 720×576 (PAL) or 720×480 (NTSC).

These settings follow Raspberry Pi's [composite video documentation](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/video.adoc).

## Play from the local console

Log in on the Pi's local text console, then run:

```sh
SDL_VIDEODRIVER=kmsdrm ./build/src/pcm_coder -R --crop-top 0 input.wav
```

`-R` selects KMSDRM automatically unless an SDL driver is explicitly set in
the environment. It detects PAL/NTSC from display dimensions, so do not combine
it with `--pal` or `--ntsc`. Ctrl+C stops playback. PortAudio is not opened in
this mode: audio is carried in the composite PCM image.

Start with `--crop-top 0` and adjust for your decoder. The KMS path preserves
source scan lines vertically, clipping at the display edges; cropping moves
the image upward and does not stretch the remaining lines. `--left_offset`
and `--right_offset` set horizontal margins. Leave `--heigth_mod` at zero
unless deliberately experimenting with vertical scaling.

The renderer requests vsync and paces complete PCM images at 25 fps (PAL) or
30000/1001 fps (NTSC). SDL does not expose field parity here, so this path
does not guarantee the legacy DispmanX callback's field phase. If the decoder
cannot lock after alignment adjustments, capture the output timing before
assuming a crop value will fix it.

If SDL cannot initialize KMSDRM, check that you are on an active local console,
no desktop compositor owns the display, and `/dev/dri/` exists. Inspect device
access with `ls -l /dev/dri` and `id`. If your account lacks access, add it to
the `video` and `render` groups and log out and back in:

```sh
sudo usermod -aG video,render "$USER"
```

An SSH session may lack active-seat/DRM-master access even with these groups;
try the local console first. SDL requires [DRM master access](https://wiki.libsdl.org/SDL2/SDL_HINT_KMSDRM_REQUIRE_DRM_MASTER)
to render through KMSDRM. Disabling that requirement does not enable rendering.

## Legacy systems

`-DENABLE_LEGACY_RPI=ON` explicitly selects the original DispmanX backend
and builds `rpi-fb-shifter` on systems with `/opt/vc`. It is not the option
for Trixie. `--vsync_delay` is available only with this legacy backend.
Do not run `rpi-fb-shifter` with KMS: it writes hardware registers directly
while the kernel owns the display.

OS selection and upgrades: [Raspberry Pi OS downloads](https://www.raspberrypi.com/software/operating-systems/)
and [OS documentation](https://www.raspberrypi.com/documentation/computers/os.html).
