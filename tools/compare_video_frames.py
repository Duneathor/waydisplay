#!/usr/bin/env python3
"""Compare aligned 8-bit RGB PPM frames; no codec or network dependencies.

Measures captured images, not codec-only distortion unless captures are taken
at the same unscaled source resolution and represent the same content/frame.
"""

import argparse
import json
import math
from pathlib import Path


def read_ppm(path):
    with Path(path).open("rb") as file:
        if file.readline().strip() != b"P6":
            raise ValueError(f"{path}: expected binary P6 PPM (RGB24)")
        tokens = []
        while len(tokens) < 3:
            line = file.readline()
            if not line:
                raise ValueError(f"{path}: incomplete header")
            tokens.extend(line.split(b"#", 1)[0].split())
        if len(tokens) != 3:
            raise ValueError(f"{path}: unsupported PPM header")
        width, height, maximum = map(int, tokens)
        if width <= 0 or height <= 0 or maximum != 255:
            raise ValueError(f"{path}: expected positive dimensions and maxval 255")
        rgb = file.read()
        if len(rgb) != width * height * 3:
            raise ValueError(f"{path}: unexpected pixel byte count")
        return width, height, rgb


def ycbcr(r, g, b):
    # Consistent full-range comparison coordinates, not the stream's YUV matrix.
    return (0.299 * r + 0.587 * g + 0.114 * b,
            -0.168736 * r - 0.331264 * g + 0.5 * b,
            0.5 * r - 0.418688 * g - 0.081312 * b)


def compare(reference, candidate):
    rw, rh, source = reference
    cw, ch, target = candidate
    if (rw, rh) != (cw, ch):
        raise ValueError(f"image dimensions differ: source {rw}x{rh}, candidate {cw}x{ch}; "
                         "resize/scale comparisons are not codec-fidelity measurements")
    totals = [0.0] * 3
    maxima = [0.0] * 3
    mismatches = 0
    for pos in range(0, len(source), 3):
        s = ycbcr(*source[pos:pos + 3])
        t = ycbcr(*target[pos:pos + 3])
        mismatches += source[pos:pos + 3] != target[pos:pos + 3]
        for channel in range(3):
            delta = abs(s[channel] - t[channel])
            totals[channel] += delta * delta
            maxima[channel] = max(maxima[channel], delta)
    pixels = rw * rh
    mse = [total / pixels for total in totals]
    return {
        "width": rw,
        "height": rh,
        "pixels_changed_pct": round(100.0 * mismatches / pixels, 3),
        "y_psnr_db": None if mse[0] == 0 else round(10.0 * math.log10(255.0 ** 2 / mse[0]), 3),
        "cb_psnr_db": None if mse[1] == 0 else round(10.0 * math.log10(255.0 ** 2 / mse[1]), 3),
        "cr_psnr_db": None if mse[2] == 0 else round(10.0 * math.log10(255.0 ** 2 / mse[2]), 3),
        "y_max_error": round(maxima[0], 3),
        "cb_max_error": round(maxima[1], 3),
        "cr_max_error": round(maxima[2], 3),
    }


def self_test():
    source = (2, 2, bytes([255, 0, 0] * 4))
    same = compare(source, source)
    assert same["pixels_changed_pct"] == 0 and same["y_psnr_db"] is None
    different = compare(source, (2, 2, bytes([0, 0, 255] * 4)))
    assert different["pixels_changed_pct"] == 100 and different["y_psnr_db"] < 20
    try:
        compare(source, (1, 4, source[2]))
    except ValueError as error:
        assert "dimensions differ" in str(error)
    else:
        raise AssertionError("mismatched dimensions must fail")
    print("video frame comparator self-test: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, help="aligned, unscaled source RGB PPM")
    parser.add_argument("--tiles", type=Path, help="optional aligned tiles screenshot RGB PPM")
    parser.add_argument("--video", type=Path, help="aligned video screenshot RGB PPM")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.reference or not args.video:
        parser.error("--reference and --video are required")
    reference = read_ppm(args.reference)
    result = {"video": compare(reference, read_ppm(args.video))}
    if args.tiles:
        result["tiles"] = compare(reference, read_ppm(args.tiles))
    print(json.dumps(result, indent=2, sort_keys=True))
    print("PSNR null means identical in this comparison channel; align source/time/size before interpreting results.")


if __name__ == "__main__":
    main()
