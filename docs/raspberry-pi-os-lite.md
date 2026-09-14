# Raspberry Pi 3B+ with Raspberry Pi OS Lite

Target: Raspberry Pi OS Lite based on Debian 13 (Trixie), with the distribution's
FFmpeg 7.1 and SDL2 packages. The Pi 3B+ supports both 32-bit and 64-bit images.
Use a fresh Lite image from Raspberry Pi Imager when moving from an older OS.

The `-R` display path uses SDL2's KMSDRM backend and runs without a desktop.
It replaces the old dependency on `/opt/vc`, `bcm_host`, and DispmanX.
**Live PCM output is experimental until verified on a Pi and hardware decoder.**
A successful build or visible video does not establish decoder lock: field
order, scan-line alignment, and sustained playback timing need hardware checks.
Testing with an NTSC Sony PCM-501ES has produced noise despite visible composite
output and crop/offset adjustments. This configuration is not yet confirmed
to deliver usable audio.

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
python3 tests/pcm_roundtrip.py ./build/src/pcm_coder
```

The round-trip check extracts bits from the generated video, checks every row's
CRC and the P/Q parity words, and recovers known audio samples in PAL/NTSC and
14/16-bit modes. It also simulates P recovery of the rows clipped by the default
KMS geometry, assuming their positions are known and all other bits are correct.
It does not test the physical signal, the Sony's synchronization and correction
behavior, or complete EOF flushing.

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
video=Composite-1:720x576ie,tv_mode=PAL
```

For an NTSC decoder, including an NTSC Sony PCM-501ES, use this instead:

```text
video=Composite-1:720x480ie,tv_mode=NTSC
```

Replace any existing `video=Composite-1:...` parameter rather than adding a
second one. Preserve the other boot parameters, including `root=...`.
The `i` selects interlaced output and `e` forces the connector on. Selecting
the TV standard alone with `vc4.tv_norm` does not force connector detection.
Reboot, then verify that the console appears on the composite display. The
player expects 720×576 (PAL) or 720×480 (NTSC).

The Pi's composite driver can report `unknown` connection status, and SDL2
requires a connector reported as `connected` with available modes. Forcing
the connector on addresses this initialization problem; it does not verify
the PCM decoder's field timing or audio recovery.

These settings follow Raspberry Pi's [composite video documentation](https://github.com/raspberrypi/documentation/blob/master/documentation/asciidoc/computers/config_txt/video.adoc)
and the kernel's [video mode parameter documentation](https://docs.kernel.org/fb/modedb.html).

## Play from the local console

Log in on the Pi's local text console, then run:

```sh
SDL_VIDEODRIVER=kmsdrm ./build/src/pcm_coder -R --crop-top 0 input.wav
```

`-R` selects KMSDRM automatically unless an SDL driver is explicitly set in
the environment. It detects PAL/NTSC from display dimensions, so do not combine
it with `--pal` or `--ntsc`. Ctrl+C stops playback. PortAudio is not opened in
this mode: audio is carried in the composite PCM image.

The command above is a diagnostic starting point, not a verified alignment for
the Sony decoder. The KMS path preserves
source scan lines vertically, clipping at the display edges; cropping moves
the image upward and does not stretch the remaining lines. `--left_offset`
and `--right_offset` set horizontal margins. Leave `--heigth_mod` at zero
unless deliberately experimenting with vertical scaling.

The renderer requests vsync and paces complete PCM images at 25 fps (PAL) or
30000/1001 fps (NTSC). SDL does not expose field parity here, so this path
does not guarantee the legacy DispmanX callback's field phase.

If playback produces noise, leave the player running and use a second SSH
session to read the active display state:

```sh
sudo cat /sys/kernel/debug/dri/0/state
```

This path assumes the composite device is `card0`, as shown in
`/sys/class/drm/card0-Composite-1/status`. Adjust the index for another card.
Record the exact playback command with the output. The state helps check the
active mode and framebuffer source/destination rectangles. It cannot measure
the analogue waveform or establish field order at the decoder.

The encoder's NTSC image contains 492 rows before padding, while the selected
display mode has 480 active rows. The default 525-row padded image therefore
cannot fit without clipping; crop/offset changes alone do not establish correct
PCM timing. Diagnose the mode, scan-line mapping, and frame presentation before
continuing alignment experiments.

There is also a horizontal timing mismatch with zero offsets in the reported
NTSC mode (`13500 720 736 800 858 480 486 492 525`). The
[original EIAJ format description, Figure 4](https://vidachestvo.ru/%D0%A1%D0%BB%D1%83%D0%B6%D0%B5%D0%B1%D0%BD%D0%B0%D1%8F%3ARedirect/file/Mitsubishi._A_PCM_Digital_Audio_Processor_for_Home_Use_VTR%27s.pdf)
specifies 168 bit periods per horizontal line and data sync starting 26 bit
periods after horizontal sync begins. With 858 pixel periods per line, the
encoder's 139-cell image should span `139 * 858 / 168 = 709.893` pixels.
Stretching it to 720 pixels makes it about 1.4% too wide.

The mode places the start of active video 122 pixel periods after horizontal
sync begins. The encoder's first sync pulse is one cell into its image, so
the image origin should be `25 * 858 / 168 - 122 = 5.679` pixels. Rounded
to whole pixels, `--left_offset 6 --right_offset 4` provides a 710-pixel
image with that origin. This is a calculated horizontal alignment for this
specific NTSC mode; it has not yet been confirmed to fix Sony playback.
It does not correct the vertical alignment or frame presentation timing.

In the software erasure model, P parity can recover the data lost to this
default clipping pattern. Clipping alone therefore does not establish the
cause of noise on the Sony: misplaced lines, additional bit errors, and frame
timing still need investigation.

To collect presentation timing measurements, run:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 SDL_VIDEO_DOUBLE_BUFFER=1 \
  ./build/src/pcm_coder -R --14 --crop-top 0 --display-stats input.wav
```

Keep it running for at least 20 seconds and record the `KMS geometry` and
`KMS timing` lines. The report shows SDL presentation-return frequency and
the minimum/maximum gaps over each five-second window. `short` and `long`
count intervals below 75% or above 125% of the target frame period, respectively.
NTSC's target is about 33.367 ms per complete image; PAL's is 40 ms.
This can reveal irregular pacing, but it does not measure the analogue field
phase or prove that every submitted image was displayed correctly.

`SDL_VIDEO_DOUBLE_BUFFER=1` makes the SDL KMS backend wait for completion of
the submitted page flip before returning (after initial setup), so these
measurements are more useful than with a pending flip. See the
[SDL implementation](https://github.com/libsdl-org/SDL/blob/release-2.32.4/src/video/kmsdrm/SDL_kmsdrmopengles.c).

For a controlled field-order comparison, repeat the same playback command with
`--swap-fields` added:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --14 --crop-top 0 --swap-fields input.wav
```

Keep the bit depth, crop, offsets, and SDL environment identical between the
two runs. This option exchanges the two PCM image fields before cropping,
without resizing the image or changing the output mode. It is available for
file encoding as well, and the round-trip test checks both field arrangements.
An odd top crop also changes which raster parity carries each field.

The Pi driver's [NTSC field timing](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/gpu/drm/vc4/vc4_crtc.c)
differs from PAL. That is a reason to test field order, not proof that swapping
is required. The switch does not synchronize page flips to a particular field,
restore clipped rows, or establish correct analogue timing. Remove it to
return to the original field arrangement.

If SDL reports `kmsdrm not available`, first inspect the connector statuses:

```sh
grep -H . /sys/class/drm/card*-*/status
```

If the composite connector reports `unknown`, check that the forced
`video=Composite-1:...ie,tv_mode=...` parameter above is present in
`/proc/cmdline` after rebooting. SDL's [connector probe](https://github.com/libsdl-org/SDL/blob/release-2.32.4/src/video/kmsdrm/SDL_kmsdrmvideo.c)
does not accept an unknown status, even when KMSDRM is compiled in and the
account can access the DRM device.

If SDL still cannot initialize KMSDRM, check that you are on an active local console,
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
