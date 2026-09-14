#!/usr/bin/env python3
"""Check PCM bits/CRC and recover audio from uncompressed video files.

Usage: python3 tests/pcm_roundtrip.py build/src/pcm_coder
This exercises the encoder, not the physical composite output or Sony's decoder.
"""
import binascii
import itertools
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import wave


def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result.stdout


def word(bits):
    result = 0
    for bit in bits:
        result = (result << 1) | bit
    return result


binary = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="pcm-roundtrip-") as directory:
    root = pathlib.Path(directory)
    source = root / "pattern.wav"
    # Different deterministic values on both channels, including both signs.
    rng = random.Random(501)
    samples = [rng.randrange(-32768, 32768) for _ in range(44100 * 2)]
    samples[:8] = [-32768, 32767, -1, 0, 1, -2, 2, -32767]
    with wave.open(str(source), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(44100)
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))

    for standard, height, top, rows_per_field in (
        ("ntsc", 525, 18, 245), ("pal", 625, 14, 294)
    ):
        for depth, swapped in itertools.product((14, 16), (False, True)):
            dest = root / f"{standard}-{depth}-{swapped}.avi"
            run(binary, f"--{standard}", f"--{depth}", "--no-dither",
                *(["--swap-fields"] if swapped else []), str(source), str(dest))
            pixels = run("ffmpeg", "-v", "error", "-i", str(dest),
                         "-f", "rawvideo", "-pix_fmt", "gray", "-")
            frame_size = 720 * height
            assert len(pixels) % frame_size == 0
            frames = len(pixels) // frame_size
            rows = []
            checked_crc = 0
            # Sample the middle of each bit cell after the video writer's
            # horizontal resize, independently of its interpolation filter.
            bit_x = [int((5 + bit + 0.5) * 720 / 139) for bit in range(128)]
            for frame in range(frames):
                # Read chronological fields from their selected raster parity.
                for field in ((1, 0) if swapped else (0, 1)):
                    # One control row followed by the data rows in each field.
                    for line in range(rows_per_field + 1):
                        y = top + 2 * line + field
                        offset = frame * frame_size + y * 720
                        bits = [int(pixels[offset + x] > 75) for x in bit_x]
                        payload = bytes(word(bits[i:i + 8]) for i in range(0, 112, 8))
                        actual_crc = word(bits[112:])
                        assert binascii.crc_hqx(payload, 0xffff) == actual_crc, (
                            standard, depth, frame, field, line, "CRC mismatch")
                        checked_crc += 1
                        if line:
                            rows.append([word(bits[i:i + 14]) for i in range(0, 112, 14)])

            # Undo the 16-line delay between successive audio words. The last
            # sample column is delayed by 80 rows. Only verify groups for which
            # all six words exist in the output (EOF flushing is not tested).
            groups = min(len(rows) - 16 * 5, len(samples) // 6)
            assert groups * 6 > len(samples) * 0.9
            for group in range(groups):
                for column in range(6):
                    row = rows[group + 16 * column]
                    recovered = row[column]
                    expected = samples[group * 6 + column]
                    if depth == 16:
                        # Low bits are packed into the same transmitted row,
                        # after interleaving, in place of its Q word.
                        recovered = (recovered << 2) | ((row[7] >> ((6 - column) * 2)) & 3)
                        expected &= 0xffff
                    else:
                        expected = (expected >> 2) & 0x3fff
                    assert recovered == expected, (
                        standard, depth, group, column, recovered, expected)
            print(f"PASS: {standard.upper()} {depth}-bit, swap={swapped}: {checked_crc} row CRCs; "
                  f"{groups * 3} stereo sample pairs recovered exactly "
                  "(EOF flushing not checked)")
