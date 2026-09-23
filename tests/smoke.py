#!/usr/bin/env python3
"""File-encoding checks without a display: python3 tests/smoke.py <pcm_coder>."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile


def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        result.check_returncode()
    return result.stdout


binary = str(pathlib.Path(sys.argv[1]).resolve())
help_text = run(binary, "--help")
with tempfile.TemporaryDirectory(prefix="pcm-smoke-") as directory:
    root = pathlib.Path(directory)
    for name, rate, channels, mode, expected_rate, expected_height in (
        ("stereo-pal", 44100, 2, "--pal", "25/1", 625),
        ("mono-ntsc", 48000, 1, "--ntsc", "30000/1001", 525),
    ):
        source = root / (name + ".wav")
        dest = root / (name + ".avi")
        run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
            f"sine=frequency=1000:sample_rate={rate}:duration=1",
            "-ac", str(channels), str(source))
        run(binary, mode, str(source), str(dest))
        info = json.loads(run("ffprobe", "-v", "error", "-count_frames",
            "-show_streams", "-of", "json", str(dest)))["streams"][0]
        assert info["codec_name"] == "rawvideo", info
        assert info["height"] == expected_height, info
        assert info["r_frame_rate"] == expected_rate, info
        assert int(info["nb_read_frames"]) >= 20, info
        run("ffmpeg", "-v", "error", "-i", str(dest), "-f", "null", "-")
        print(f"PASS: {name}, {info['nb_read_frames']} frames")

    invalid_crop = subprocess.run(
        [binary, "--crop-top", "4294967295", str(source), str(dest)],
        capture_output=True, text=True)
    assert invalid_crop.returncode == 1, invalid_crop
    assert "Cropping must leave" in invalid_crop.stderr, invalid_crop.stderr
    print("PASS: invalid cropping rejected")

    if "-R,--rpi-mode" in help_text:
        conflict = subprocess.run([binary, "-R", str(source), str(dest)],
                                  capture_output=True, text=True)
        assert conflict.returncode != 0, conflict
        print("PASS: -R rejects an output filename")
        # Force SDL initialization to fail without requiring display hardware.
        unavailable = subprocess.run([binary, "-R", str(source)],
            env={**os.environ, "SDL_VIDEODRIVER": "pcm-test-unavailable"},
            capture_output=True, text=True)
        assert unavailable.returncode == 1, unavailable
        assert "SDL video initialization failed" in unavailable.stderr, unavailable.stderr
        print("PASS: SDL initialization failure reported cleanly")
    if "--wait-for-enter" in help_text:
        missing_rpi = subprocess.run([binary, "--wait-for-enter", str(source)],
            capture_output=True, text=True)
        assert missing_rpi.returncode != 0, missing_rpi
        assert "--wait-for-enter" in missing_rpi.stderr, missing_rpi.stderr
        compatible = subprocess.run([binary, "-R", "--wait-for-enter",
            "--kms-pcm-levels", "--left_offset", "2", "--right_offset", "8",
            "--crop-top", "18", "--crop-bot", "43", str(source)],
            env={**os.environ, "SDL_VIDEODRIVER": "pcm-test-unavailable"},
            capture_output=True, text=True, timeout=10)
        assert compatible.returncode == 1, compatible
        assert "SDL video initialization failed" in compatible.stderr, compatible.stderr
        print("PASS: wait-for-enter requires -R and accepts the current test geometry")
    if "--kms-pcm-levels" in help_text:
        missing_rpi = subprocess.run([binary, "--kms-pcm-levels", str(source)],
            capture_output=True, text=True)
        assert missing_rpi.returncode != 0, missing_rpi
        assert "--kms-pcm-levels" in missing_rpi.stderr, missing_rpi.stderr
        compatible = subprocess.run([binary, "-R", "--kms-pcm-levels",
            "--left_offset", "10", "--crop-top", "8", "--crop-bot", "45", str(source)],
            env={**os.environ, "SDL_VIDEODRIVER": "pcm-test-unavailable"},
            capture_output=True, text=True)
        assert compatible.returncode == 1, compatible
        assert "SDL video initialization failed" in compatible.stderr, compatible.stderr
        print("PASS: PCM levels require -R and accept the working cropped geometry")
    if "--kms-full-frame" in help_text:
        for args in (["--kms-full-frame"],
                     ["-R", "--kms-full-frame", "--crop-top", "0"],
                     ["-R", "--kms-full-frame", "--crop-bot", "1"],
                     ["-R", "--kms-full-frame", "--heigth_mod", "-12"]):
            conflict = subprocess.run([binary, *args, str(source)],
                env={**os.environ, "SDL_VIDEODRIVER": "pcm-test-unavailable"},
                capture_output=True, text=True)
            assert conflict.returncode != 0, conflict
            assert "--kms-full-frame" in conflict.stderr, conflict.stderr
            assert "SDL video initialization failed" not in conflict.stderr, conflict.stderr
        print("PASS: full-frame mode rejects incompatible CLI options before opening a display")
