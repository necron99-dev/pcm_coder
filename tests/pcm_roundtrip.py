#!/usr/bin/env python3
"""Check PCM bits/CRC and recover audio from uncompressed video files.

Usage: python3 tests/pcm_roundtrip.py build/src/pcm_coder
This exercises the encoder, not the physical composite output or Sony's decoder.
"""
import binascii
import itertools
import pathlib
import functools
import operator
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


def q_parity(words):
    # STC-007: T^6 L0 + T^5 R0 + ... + T R2 over GF(2).
    # T is a left shift with feedback at bits 8 and 0 from bit 13.
    # Independent reference: Fagear/SDVPCMdecoder's TP1_MATRIX and calcQcode:
    # https://github.com/Fagear/SDVPCMdecoder/blob/main/stc007deinterleaver.cpp
    result = 0
    for value in words:
        combined = result ^ value
        result = ((combined << 1) & 0x3fff) ^ (0x101 if combined & 0x2000 else 0)
    return result


def unpack_word(row, column, depth):
    result = row[column]
    if depth == 16:
        result = (result << 2) | ((row[7] >> ((6 - column) * 2)) & 3)
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
            visible = []
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
                            # Model the current KMS renderer at crop=0,
                            # height_mod=0: rows below the screen are clipped.
                            visible.append(y < (480 if standard == "ntsc" else 576))

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

            parity_groups = min(len(rows) - 16 * 7, len(samples) // 6)
            repaired_audio_words = 0
            for group in range(parity_groups):
                block = [unpack_word(rows[group + 16 * col], col, depth)
                         for col in range(7)]
                assert functools.reduce(operator.xor, block) == 0, (
                    standard, depth, swapped, group, "P mismatch")
                if depth == 14:
                    assert q_parity(block[:6]) == rows[group + 16 * 7][7], (
                        standard, swapped, group, "Q mismatch")

                # Treat clipped scan lines as known erasures. This does not
                # model the Sony's line synchronization or analogue bit errors.
                missing = [col for col in range(7) if not visible[group + 16 * col]]
                assert len(missing) <= 1, (standard, group, "P cannot repair this pattern")
                if missing:
                    bad = missing[0]
                    recovered = functools.reduce(operator.xor,
                        (value for col, value in enumerate(block) if col != bad))
                    assert recovered == block[bad], (standard, depth, group, bad)
                    repaired_audio_words += int(bad < 6)
            assert repaired_audio_words > 0
            print(f"PASS: {parity_groups} P{'/Q' if depth == 14 else ''} blocks; "
                  f"{repaired_audio_words} clipped audio words recoverable with P "
                  "(known erasures, ideal signal)")
