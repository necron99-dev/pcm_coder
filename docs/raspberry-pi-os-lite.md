# Raspberry Pi 3B+ with Raspberry Pi OS Lite

Target: Raspberry Pi OS Lite based on Debian 13 (Trixie), with the distribution's
FFmpeg 7.1 and SDL2 packages. The Pi 3B+ supports both 32-bit and 64-bit images.
Use a fresh Lite image from Raspberry Pi Imager when moving from an older OS.

The `-R` display path uses SDL2's KMSDRM device/console setup and libdrm
page-flip events for presentation, without a desktop. It replaces the old
dependency on `/opt/vc`, `bcm_host`, and DispmanX.
**Audio playback has been reported working on a Pi 3B+ with an NTSC Sony
PCM-501ES**, using default 14-bit encoding, `--crop-top 0`, and
`--left_offset 9`. Zero horizontal offsets previously produced noise.
That listening result predates the new DRM event-driven presentation path,
which still needs hardware retesting with the same settings. Waveform accuracy,
bit-error rate, 16-bit playback, and other decoder/TV-standard combinations
remain unverified.

## Build

From the repository directory:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  ffmpeg libsdl2-dev libdrm-dev portaudio19-dev \
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
sh tests/kms_scanout.sh
```

The round-trip check extracts bits from the generated video, checks every row's
CRC and the P/Q parity words, and recovers known audio samples in PAL/NTSC and
14/16-bit modes. It also simulates P recovery of the rows clipped by the default
KMS geometry, assuming their positions are known and all other bits are correct.
It does not test the physical signal, the Sony's synchronization and correction
behavior, or complete EOF flushing.

To build only the file encoder, configure with `-DENABLE_PLAYER=OFF`.
SDL2, libdrm, and PortAudio development packages are then unnecessary. File encoding
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

## Play through composite

The following command was reported to play audio successfully through the
NTSC PCM-501ES. It was run over SSH with no desktop compositor owning DRM:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --crop-top 0 --left_offset 9 input.wav
```

The omitted options retain their defaults: 14-bit encoding, P and Q enabled,
right offset zero, height modifier zero, and no field swap. The inherited value
of `SDL_VIDEO_DOUBLE_BUFFER` was not recorded for that earlier successful test.
The current direct DRM presentation path does not use this SDL hint.

`-R` selects KMSDRM automatically unless an SDL driver is explicitly set in
the environment. It detects PAL/NTSC from display dimensions, so do not combine
it with `--pal` or `--ntsc`. Ctrl+C stops playback. PortAudio is not opened in
this mode: audio is carried in the composite PCM image.

On the 720-pixel display, `--left_offset 9` draws a 711-pixel-wide image starting
at horizontal pixel 9. It changes both image position and bit-cell width.
The KMS path preserves source scan lines vertically, clipping at the display
edges; cropping moves
the image upward and does not stretch the remaining lines. `--left_offset`
and `--right_offset` set horizontal margins. Leave `--heigth_mod` at zero
unless deliberately experimenting with vertical scaling.

The player allocates two DRM scanout buffers and preserves the active interlaced
composite mode. It first displays black, measures vblank spacing, and anchors
its relative frame phase to a completed page flip. It prepares the next image
in the free buffer, queues a synchronized flip, and waits for the kernel's
completion event before reusing the previous buffer. There is no sleep-based
frame clock and no SDL renderer on this path.

When the driver reports one vblank per field, the player submits during the
intervening field to keep successive flips on the same relative phase, two
ticks apart. With one vblank per complete frame, it queues the next flip
directly, including drivers whose counter advances by two at each frame event.
Late production can repeat a complete image; it does not trigger
catch-up bursts. Repeats are reported because they can cause audible errors.
A flip that completes on an unexpected phase stops playback. The legacy flip
API cannot guarantee a target sequence if scheduling misses the boundary;
the completion check detects that failure after it happens.

DRM event sequence/timestamps do **not** label the physical odd/even field.
This establishes a repeatable phase relative to the first flip within a run,
not a verified analogue field identity across runs. Keep the known working
field arrangement initially; `--swap-fields` remains a separate raster-order
diagnostic. Hardware measurement is still required to prove field identity.
See the [DRM event interface](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/include/uapi/drm/drm.h)
and [VC4 flip completion handling](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/gpu/drm/vc4/vc4_crtc.c).

## Diagnose noise or timing problems

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

The working `--left_offset 9` setting narrows the image to 711 pixels, close
to that calculated width, and moves it to the right. Use the reported working
setting for this setup; the nominal timing calculation alone does not capture
the full analogue signal path or decoder tolerance.

In the software erasure model, P parity can recover the data lost to this
default clipping pattern. Clipping alone therefore does not establish the
cause of noise on the Sony: misplaced lines, additional bit errors, and frame
timing still need investigation.

To collect presentation timing measurements, run:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --14 --crop-top 0 --left_offset 9 --display-stats input.wav
```

Keep it running for at least 20 seconds and record the `KMS sync`, `KMS geometry`,
and `KMS timing` lines. Startup reports the measured vblank ticks per image
and initial flip sequence. Timing reports use completed DRM flip timestamps,
with minimum/maximum gaps and repeated images over each five-second window.
NTSC should average about 29.970 flips/s with 33.367 ms gaps; PAL should be
25 flips/s with 40 ms gaps. These are kernel reports, not analogue measurements.

If startup rejects the cadence or playback reports lost frame phase, preserve
the error and startup output. Do not try to compensate by changing crop or
offsets: those settings change image geometry, not event timing. The player
uses bounded waits and restores the console on exit, including errors and
Ctrl+C. `SDL_VIDEO_DOUBLE_BUFFER` no longer affects `-R` presentation.

For a controlled field-order comparison, repeat the same playback command with
`--swap-fields` added:

```sh
SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --14 --crop-top 0 --left_offset 9 --swap-fields input.wav
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
