#!/usr/bin/env python3
"""Offline analysis of stereo/mono PCM with 16-bit wrap fingerprints."""
from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import Iterable, Sequence


WRAP_NEAR = 24000
LARGE_JUMP = 32768


def modulo16_delta(previous: int, current: int) -> int:
    raw = current - previous
    return ((raw + 32768) & 0xFFFF) - 32768


def channel_metrics(seq: Sequence[int], rate: int) -> dict:
    frames = len(seq)
    if frames == 0:
        return {}
    peak = max(abs(v) for v in seq)
    mean = sum(seq) / frames
    rms = (sum(v * v for v in seq) / frames) ** 0.5
    full = sum(1 for v in seq if v in (32767, -32768))
    near = sum(1 for v in seq if abs(v) >= 32000)
    run = cur = 0
    for v in seq:
        if v in (32767, -32768):
            cur += 1
            run = max(run, cur)
        else:
            cur = 0

    max_delta = 0
    max_at = 0
    max_mod = 0
    max_mod_at = 0
    large_jump = 0
    sign_flip = 0
    wrap_candidate = 0
    wrap_small_mod = 0
    click16 = 0
    click32 = 0
    for i in range(1, frames):
        prev = seq[i - 1]
        cur = seq[i]
        raw = abs(cur - prev)
        wrapped = abs(modulo16_delta(prev, cur))
        if raw > max_delta:
            max_delta = raw
            max_at = i
        if wrapped > max_mod:
            max_mod = wrapped
            max_mod_at = i
        if raw >= 16384:
            click16 += 1
        if raw >= LARGE_JUMP:
            click32 += 1
            large_jump += 1
        if (prev > WRAP_NEAR and cur < -WRAP_NEAR) or (prev < -WRAP_NEAR and cur > WRAP_NEAR):
            sign_flip += 1
            wrap_candidate += 1
            if wrapped <= 256:
                wrap_small_mod += 1

    return {
        "peak": peak,
        "rms": rms,
        "crest": (peak / rms) if rms else 0.0,
        "dc": mean,
        "full": full,
        "near": near,
        "fullScaleRun": run,
        "maxDelta": max_delta,
        "maxDeltaAt": max_at,
        "maxModulo16Delta": max_mod,
        "maxModulo16At": max_mod_at,
        "largeJumpCount": large_jump,
        "largeJumpRate": large_jump / max(1, frames - 1),
        "signFlipLargeJumpCount": sign_flip,
        "wrapCandidateCount": wrap_candidate,
        "wrapCandidateRate": wrap_candidate / max(1, frames - 1),
        "wrapSmallModuloCount": wrap_small_mod,
        "click16": click16,
        "click32": click32,
        "frames": frames,
        "duration_s": frames / rate if rate else 0.0,
    }


def analyze_bytes(data: bytes, rate: int, channels: int) -> list[dict]:
    sample_bytes = 2 * channels
    if len(data) < sample_bytes:
        return []
    frames = len(data) // sample_bytes
    samples = struct.unpack("<" + "h" * (frames * channels), data[: frames * sample_bytes])
    return [channel_metrics(samples[c::channels], rate) for c in range(channels)]


def print_metrics(path: str, rate: int, channels: int, leftover: int, metrics: Iterable[dict]) -> None:
    print(f"file={path}")
    printed = False
    for c, m in enumerate(metrics):
        printed = True
        print(
            f"  ch{c}: peak={m['peak']} rms={m['rms']:.1f} crest={m['crest']:.2f} dc={m['dc']:.1f} "
            f"full={m['full']} near={m['near']} run={m['fullScaleRun']} "
            f"maxDelta={m['maxDelta']} at={m['maxDeltaAt']} ({m['maxDeltaAt'] / rate:.4f}s) "
            f"maxMod16={m['maxModulo16Delta']} at={m['maxModulo16At']} "
            f"click16={m['click16']} click32={m['click32']} "
            f"largeJump={m['largeJumpCount']} ({m['largeJumpRate']:.4f}) "
            f"signFlip={m['signFlipLargeJumpCount']} wrapCand={m['wrapCandidateCount']} "
            f"({m['wrapCandidateRate']:.4f}) wrapSmallMod={m['wrapSmallModuloCount']}"
        )
        if m["wrapCandidateCount"] and m["wrapSmallModuloCount"] >= max(1, m["wrapCandidateCount"] // 2):
            print("    wrap fingerprint: many ±full-range jumps collapse under modulo-16bit")
    if not printed:
        print(f"{path}: empty")
        return
    print(f"  bytes={os.path.getsize(path) if os.path.isfile(path) else 0} leftover={leftover}")


def analyze(path: str, rate: int, channels: int) -> None:
    data = open(path, "rb").read()
    sample_bytes = 2 * channels
    leftover = len(data) % sample_bytes if sample_bytes else 0
    metrics = analyze_bytes(data, rate, channels)
    print_metrics(path, rate, channels, leftover, metrics)


def self_test() -> int:
    # +32760 -> -32760 is a 16-bit wrap of +16, not a natural transient.
    seq = [32760, -32760]
    m = channel_metrics(seq, 48000)
    assert m["maxDelta"] == 65520, m
    assert modulo16_delta(32760, -32760) == 16
    assert m["wrapCandidateCount"] == 1
    assert m["wrapSmallModuloCount"] == 1
    # Natural loud step that is not a wrap.
    seq2 = [20000, -20000]
    m2 = channel_metrics(seq2, 48000)
    assert m2["wrapCandidateCount"] == 0, m2
    print("self-test ok")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input")
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.input or not os.path.isfile(args.input):
        print(f"missing {args.input}", file=sys.stderr)
        return 1
    analyze(args.input, args.rate, args.channels)
    return 0


if __name__ == "__main__":
    sys.exit(main())
