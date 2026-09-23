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
The DRM event-driven path at commit `a7ada62` has now also been reported to
play consistently in 14-bit mode. The provided log shows 29.971 completed
flips/s, roughly 33.367 ms between flips, and no repeated frames over about
30 seconds. The driver reports two counter increments per field, four per
image. On the same setup, 16-bit mode produces music with substantial noise.
Its cause remains unresolved; the reported timing log was from 14-bit playback.
Waveform accuracy, bit-error rate, and other decoder/TV-standard combinations
remain unverified.

Subsequent tests found tape-recording failures on a Sony SLV-R1000 at SP:
the Pi through the PCM-501ES COPY OUT produces playback dropouts, while direct
Pi-to-VCR recording produces severe noise and unreliable lock. EDIT playback
mode and confirmed Mono composite output did not resolve it. A native
PCM-501ES recording from a turntable plays cleanly on the same deck.
Clean live monitoring, including silent PCM, therefore does not establish
recording compatibility. Silence also cannot reveal repeated identical frames.

The latest live comparison used one WAV containing 20 seconds of `holy.wav`,
20 seconds of `Pink_Floyd_DSotM_RMR.wav`, then the same Holy excerpt again.
At `--left_offset 13 --right_offset 4 --crop-top 8 --crop-bot 45`, lock was
lost when Pink Floyd began at 20 seconds. With the same vertical settings and
`--left_offset 10 --right_offset 0`, the user reported that lock stayed.
The latter run confirmed `source=139x472, draw=710x472+10+0, screen=720x480`.
Its supplied timing excerpt showed 29.970 flips/s and gaps of about
33.356–33.376 ms. A subsequent direct Pi-to-VHS recording with these settings
was reported substantially improved and close to clean, but still imperfect.
The user subsequently reported a maximum tracking indication with red OVC
flashing during recording and more frequent flashing on playback. Adjusting
OVC did not clear the red indication. Long-duration reliability and the COPY
OUT route with this geometry remain unverified.
The known-good tape made with the Sony's native encoder reaches green OVC
without moving the knob. This provides a working playback reference; it does
not identify which property of the Pi-generated signal causes the errors.

The working border-matching settings still transmit only 230 of the 245 data
lines per field: ten black rows, two control rows, 460 data rows, and eight
black output rows. A comparison changed only `--crop-bot 45` to
`--crop-bot 0`. This fills those eight bottom rows with PCM data, giving 234
data lines per field with the same top position, horizontal geometry and
720x480i mode. It still omits eleven data lines per field. The user reported
harder initial lock and no improvement in errors, so the working baseline
remains `--crop-bot 45`. This test did not use the failed full-frame mode.

Current live-test command (keep the existing TV mode and CPU settings):

```sh
sudo chrt -f 50 taskset -c 3 \
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R \
  --left_offset 10 --right_offset 0 \
  --crop-top 8 --crop-bot 45 --display-stats \
  /tmp/pcm-lock-test.wav
```

This command expects the comparison WAV to have already been created. Keep the
improved direct recording setup unchanged while checking the remaining glitches.
Replay the same affected tape passage twice and note whether glitches recur at
the same positions and whether the PCM-501ES loses lock. Those observations can
guide the next controlled test without attributing the residual errors to a
specific part of the signal path prematurely.

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
intervening field to keep successive flips on the same relative phase, one
complete image apart. With one vblank per complete frame, it queues the next flip
directly, including drivers whose counter advances by two at each frame event.
Late production can repeat a complete image; it does not trigger
catch-up bursts. Repeats are reported because they can cause audible errors.
A flip that completes on an unexpected phase stops playback. The legacy flip
API cannot guarantee a target sequence if scheduling misses the boundary;
the completion check detects that failure after it happens.

Counter ticks are not necessarily physical fields. Calibration supports one,
two, or four counts per image and checks the observed event step separately.
The four-count case accommodates kernels that halve interlaced timing twice:
two counter increments per physical field, with four increments per image.
The [upstream 6.18.34 calculation](https://github.com/gregkh/linux/blob/v6.18.34/drivers/gpu/drm/drm_vblank.c)
halves the adjusted CRTC interval for interlace; the
[Pi branch calculation](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/gpu/drm/drm_vblank.c)
checks whether the vertical timing was already halved. Detection uses measured
timestamps and counts, not the kernel version string.

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

The earlier working `--left_offset 9` setting narrows the image to 711 pixels,
close to that calculated width, and moves it to the right. The latest live
comparison above uses 710 pixels (`--left_offset 10 --right_offset 0`). A
photo-based 703-pixel setting shortened the nominal bit periods by about 1%
and showed content-dependent lock loss. Matching TV borders alone is not a
reliable way to set PCM bit timing. The successful comparison changed both
horizontal position and width; it does not isolate which change restored lock
or establish correct analogue timing throughout the signal path.

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
and initial flip sequence. Raw startup samples show the sequence and timestamp
pairs, including when calibration fails. Timing reports use completed DRM flip timestamps,
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

## Native MONO525 VEC profile

Add `--vec-mono525` to direct KMS playback (`-R`) to apply the equivalent of:

```sh
sudo python3 ~/tweakvec/tweakvec.py --preset MONO525 --sync-adj 7
```

The implementation is native C++; Python and a TweakVec checkout are not
required. It follows the register settings in
[TweakVec](https://github.com/kFYatek/tweakvec/blob/master/tweakvec.py)
(public domain / Unlicense), including the preset defaults: NTSC, pedestal on,
luma and sync on, chroma and burst off, and sync adjustment 7.

The profile is applied **after the KMS modeset and before calibration or PCM
output**, because the kernel programs the VEC during a modeset. Startup prints
`VEC MONO525 applied and read back` after checking all written registers.
Failure to access or configure the VEC stops playback with an error.

This option requires read/write access to `/dev/mem` (normally run with `sudo`),
a supported Raspberry Pi 0–4 VEC device tree with `vec` and PixelValve symbols,
and an active 720x480 interlaced NTSC mode (13.5 MHz, 858x525 total timing).
It rejects PAL and progressive modes. Register addresses come from the running
device tree. PixelValve timings, PCM levels, crops and offsets are unchanged.
On normal exit, Ctrl+C or startup failure, the saved VEC settings are restored
before restoring the previous DRM display. Forced termination cannot perform
cleanup. Do not run TweakVec concurrently with this option.

The option defaults off. Software tests cover register equivalence, device-tree
translation, rejected modes, startup ordering and restoration; validation on the
Pi is still required. This integrates the existing experiment, and does not by
itself establish error-free VHS recording.

## Start with PCM silence before playing the file

Add `--wait-for-enter` to KMS playback to send continuous PCM-encoded silence
before starting the input file. With `--vec-mono525`, the VEC profile is already
active when the silence prompt appears. Allow the decoder to lock, and press Enter in the
`pcm_coder` terminal. Ctrl+C exits while waiting; closing standard input without
Enter exits with an error rather than starting the file.

For the current level-comparison experiment:

```sh
sudo chrt -f 50 taskset -c 3 \
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --vec-mono525 --kms-pcm-levels --wait-for-enter \
  --left_offset 2 --right_offset 8 \
  --crop-top 18 --crop-bot 43 \
  ~/music/holy.wav
```

No separate TweakVec command is needed with `--vec-mono525`.

The lead-in uses zero audio samples without dither, with the selected PCM
format, parity, levels and geometry. Enter starts the file from its beginning
through the same encoder and display; the interleaver and KMS mode are not
restarted. Normal pipeline latency still applies. The file's dither setting
is preserved, and its progress starts when file playback starts. Without the
option, playback starts immediately as before. This startup control does not
change the signal-level experiment or establish VHS recording reliability.

## Experimental PCM video levels

The normal renderer uses RGB codes 0/150 for PCM zero/one, with a 255 white
reference. Zero therefore shares the black padding level. In
[IEC 60841 Figure 3a/b](https://pcm4all.ru/wp-content/uploads/2021/08/IEC-60841-1988.pdf#page=15),
data zero is 0.1 V above blanking, data one is 0.4 V above blanking (a 0.3 V
data swing), and the white reference is 0.7 V above blanking.

`--kms-pcm-levels` currently tests RGB codes **22/146** for zero/one, with
blanking 0 and white reference 255. The original 36/146 experiment assumed a
linear mapping from RGB 0–255 to a 0–0.7 V span; the zero code was subsequently
adjusted to 22 in hardware comparisons. The actual DAC
transfer, TV-mode pedestal and terminated signal levels have not been measured.
These are software codes, not a verified voltage calibration or a proven fix.

The change applies to the data-sync bits, 128 payload bits and the one-bit gap
before the white reference. The outer blank cells, vertical padding, margins
and white reference remain unchanged. Bit values, field arrangement, crops,
draw width and display mode are unchanged. The option defaults off, affects
only direct KMS playback, and does not restore the missing PCM lines.

After the updated sources have been built on the Pi, compare this against the
working command, leaving the OVC knob, TV mode, wiring and WAV unchanged:

```sh
sudo chrt -f 50 taskset -c 3 \
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --kms-pcm-levels \
  --left_offset 10 --right_offset 0 \
  --crop-top 8 --crop-bot 45 --display-stats \
  /tmp/pcm-lock-test.wav
```

Expect the same `source=139x472, draw=710x472+10+0, screen=720x480` geometry
and a new `KMS experimental PCM levels` line reporting the four RGB codes.
Compare live OVC, lock acquisition and audible errors before making another
recording. Omit `--kms-pcm-levels` to restore the baseline. Do not add the
unsuccessful `--kms-full-frame` option to this comparison.

The mocked scanout test checks every output pixel for both level settings,
including all-zero/all-one payloads, sync, the gap, white reference, margins
and blank rows. Hardware OVC and VHS results for this option are pending.

## Experimental full-frame NTSC scanout

**Hardware result: unsuccessful on the reported Pi/PCM-501ES setup.** The user
reported no lock with `--kms-full-frame` and worse results after testing left
offsets. Omit this flag to return to the previous output path; no rebuild or
Git rollback is needed. Keep the other settings unchanged for that comparison.
The experiment remains available for investigation, but is not a recording fix.

The default raster has 18 black rows, two PCM control rows, 490 PCM data rows,
then 15 black rows. Displaying only its first 480 rows loses 30 data rows:
15 per field. The software erasure test shows recovery for ideal, correctly
located erasures; it does not show how much additional tape damage a real
decoder can tolerate. Stable page-flip timing does not detect these lost rows.

`--kms-full-frame` bypasses that padding and temporarily requests a 720x492i
DRM mode. Each field gets one control row and all 245 data rows. No vertical
scaling is used. It preserves the original horizontal timing, 13.5 MHz clock,
858 pixels/line and 525 lines/frame (29.970 images/s). The vertical modeline is
`492 510 516 525`, giving adjusted active/front/sync/back lengths of
`246/9/3/4` per field before the driver's half-line handling. This meets the
[Pi driver's checked limits](https://github.com/raspberrypi/linux/blob/rpi-6.18.y/drivers/gpu/drm/vc4/vc4_vec.c).

**This is a clipping-removal experiment, not verified EIAJ-compliant output.**
The [PCM standard, section 8.3 and Figure 4a](https://pcm4all.ru/wp-content/uploads/2021/08/IEC-60841-1988.pdf)
also specifies the physical position of the control/data lines. The experiment
does not establish those positions or the odd/even field identity on the wire.
Kernel acceptance and simulated tests cannot establish analogue waveform
quality or successful tape playback.

For further investigation only, the experimental command is:

```sh
sudo chrt -f 50 taskset -c 3 \
  env SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_DEVICE_INDEX=0 \
  ./build/src/pcm_coder -R --kms-full-frame --left_offset 9 \
  --display-stats ~/music/Pink_Floyd_DSotM_RMR.wav
```

Use the same TV mode, tape speed, horizontal offsets and wiring as the previous
comparison. Do not pass `--crop-top`, `--crop-bot`, or `--heigth_mod`, including
zero values; this path sends the unpadded PCM frame directly. The expected
geometry is `source=139x492, draw=711x492+9+0, screen=720x492`. Frame time should
remain approximately 33.367 ms. The existing flip-phase checks still apply.

Only boot-selected NTSC 720x480i with the timing above is supported. If the
kernel rejects the mode, playback stops and reports the error instead of
silently reverting to clipped output. The original display mode is restored
on normal exit, Ctrl+C and handled playback errors. Omit the flag to use the
previous path. No boot-file changes or raw register writes are involved.

The simulated scanout test verifies all 492 framebuffer rows, unchanged flip
cadence, original-mode restoration and cleanup after rejected modesets. It
does not emulate the analogue encoder or a VCR. Those software checks passed,
but the subsequent hardware test above failed to establish lock. The cause of
the lock failure and the original tape dropouts remains unresolved.

## Legacy systems

`-DENABLE_LEGACY_RPI=ON` explicitly selects the original DispmanX backend
and builds `rpi-fb-shifter` on systems with `/opt/vc`. It is not the option
for Trixie. `--vsync_delay` is available only with this legacy backend.
Do not run `rpi-fb-shifter` with KMS: it writes hardware registers directly
while the kernel owns the display.

OS selection and upgrades: [Raspberry Pi OS downloads](https://www.raspberrypi.com/software/operating-systems/)
and [OS documentation](https://www.raspberrypi.com/documentation/computers/os.html).
