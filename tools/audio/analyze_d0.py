#!/usr/bin/env python3
"""Analyze DirectSound D0 pre-norm aggregate float32 dumps."""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fp:
        return json.load(fp)


def analyze_float(path: str, channels: int, rate: int) -> None:
    data = open(path, "rb").read()
    sample_bytes = 4 * max(1, channels)
    frames = len(data) // sample_bytes
    leftover = len(data) % sample_bytes
    n = frames * channels
    samples = struct.unpack("<" + "f" * n, data[: n * 4]) if n else ()
    print(f"file={path} rate={rate} ch={channels} frames={frames} leftover={leftover}")
    for c in range(channels):
        seq = samples[c::channels]
        if not seq:
            print(f"  ch{c}: empty")
            continue
        peak = max(abs(v) for v in seq)
        rms = (sum(v * v for v in seq) / len(seq)) ** 0.5
        max_delta = 0.0
        sign_flip = 0
        over = {1.0: 0, 1.25: 0, 1.5: 0, 2.0: 0}
        prev = None
        for v in seq:
            a = abs(v)
            for thr in over:
                if a >= thr:
                    over[thr] += 1
            if prev is not None:
                max_delta = max(max_delta, abs(v - prev))
                if (prev > 0.5 and v < -0.5) or (prev < -0.5 and v > 0.5):
                    sign_flip += 1
            prev = v
        print(
            f"  ch{c}: peak={peak:.4f} rms={rms:.4f} maxDelta={max_delta:.4f} "
            f"over1={over[1.0]} ({over[1.0] / len(seq):.4f}) "
            f"over1.25={over[1.25]} over1.5={over[1.5]} over2={over[2.0]} "
            f"signFlip={sign_flip}"
        )


def verdict(meta: dict, peaks: list[float], over1_rates: list[float]) -> str:
    has_norm = int(meta.get("normfunction") or 0)
    peak = max(peaks) if peaks else 0.0
    over = max(over1_rates) if over1_rates else 0.0
    if peak >= 1.0 or over > 0.001:
        return (
            "A: D0 aggregate exceeds ±1 before norm16. "
            "Wine dsound float mix / S16 primary headroom is the lead hypothesis. Next is D1."
        )
    if has_norm:
        return (
            "B/C: D0 peak stays under 1.0 on this dump. "
            "If W0 still has ±full-range jumps in the same window, inspect norm16/format; "
            "if D0 already looks discontinuous, continue to D1. "
            "Prefer 1Hz [DSOUND-DIAG] during combat over the first-2s dump."
        )
    return (
        "Float backend (normfunction=0): D0 is the WASAPI float mix. "
        "Compare peak/over1 with the S16 device."
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", required=True)
    args = parser.parse_args()
    meta = load_json(args.json)
    base = os.path.dirname(os.path.abspath(args.json))
    raw = meta.get("rawPath") or ""
    if not os.path.isfile(raw):
        alt = os.path.join(base, os.path.basename(raw)) if raw else ""
        raw = alt if os.path.isfile(alt) else os.path.join(
            base, os.path.basename(args.json).replace(".json", "_float32.raw")
        )
        if not os.path.isfile(raw):
            raw = os.path.join(base, os.path.basename(args.json).replace(".json", ".raw"))
    ch = int(meta.get("channels") or 2)
    rate = int(meta.get("sampleRate") or 48000)
    print(
        f"d0Id={meta.get('d0Id')} forcewave={meta.get('forcewave')} mixfloat={meta.get('mixfloat')} "
        f"norm={meta.get('normfunction')} priolevel={meta.get('priolevel')} "
        f"bits={meta.get('bitsPerSample')} tag={meta.get('formatTag')} "
        f"nbufs={meta.get('nrofbuffers')} audibleMax={meta.get('audibleMax')} "
        f"jsonPeakL={meta.get('peakL')} jsonPeakR={meta.get('peakR')} "
        f"jsonOver1L={meta.get('over1L')} jsonOver1R={meta.get('over1R')}"
    )
    peaks = []
    over_rates = []
    if os.path.isfile(raw):
        analyze_float(raw, ch, rate)
        data = open(raw, "rb").read()
        n = (len(data) // 4)
        samples = struct.unpack("<" + "f" * n, data[: n * 4]) if n else ()
        for c in range(ch):
            seq = samples[c::ch]
            if not seq:
                continue
            peaks.append(max(abs(v) for v in seq))
            over_rates.append(sum(1 for v in seq if abs(v) >= 1.0) / len(seq))
    else:
        print(f"missing raw {raw}")
        peaks = [float(meta.get("peakL") or 0), float(meta.get("peakR") or 0)]
        samples = max(1, int(meta.get("samples") or 1) // max(1, ch))
        over_rates = [
            int(meta.get("over1L") or 0) / samples,
            int(meta.get("over1R") or 0) / samples,
        ]
    print(verdict(meta, peaks, over_rates))
    return 0


if __name__ == "__main__":
    sys.exit(main())
