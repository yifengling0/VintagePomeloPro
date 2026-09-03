#!/usr/bin/env python3
"""Compare Wine W0 (client) vs W1 (converted mix) captures from one ReleaseBuffer window."""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze_s16le import analyze_bytes, print_metrics


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fp:
        return json.load(fp)


def analyze_client(path: str, meta: dict) -> None:
    data = open(path, "rb").read()
    channels = max(1, int(meta.get("channels") or 2))
    rate = max(1, int(meta.get("sampleRate") or 44100))
    bits = int(meta.get("bitsPerSample") or 16)
    is_float = int(meta.get("isFloat") or 0)
    print(f"W0 file={path} rate={rate} ch={channels} bits={bits} float={is_float} bytes={len(data)}")
    if is_float or bits != 16:
        if is_float and bits == 32:
            frames = len(data) // (4 * channels)
            samples = struct.unpack("<" + "f" * (frames * channels), data[: frames * 4 * channels])
            for c in range(channels):
                seq = samples[c::channels]
                peak = max(abs(v) for v in seq) if seq else 0.0
                nans = sum(1 for v in seq if v != v)
                max_delta = 0.0
                for i in range(1, frames):
                    max_delta = max(max_delta, abs(seq[i] - seq[i - 1]))
                print(f"  ch{c}: peak={peak:.5f} nan={nans} maxDelta={max_delta:.5f} frames={frames}")
            print("  W0 is float; 16-bit wrap fingerprint applies to W1 / decoded S16, not this dump")
            return
        print("  W0 is not PCM S16; inspect formatTag/validBits before comparing wrap")
        return
    leftover = len(data) % (2 * channels)
    print_metrics(path, rate, channels, leftover, analyze_bytes(data, rate, channels))


def verdict(w0: list[dict], w1: list[dict]) -> str:
    def wrap_rate(metrics: list[dict]) -> float:
        if not metrics:
            return 0.0
        return max(m.get("wrapCandidateRate", 0.0) for m in metrics)

    def jump_rate(metrics: list[dict]) -> float:
        if not metrics:
            return 0.0
        return max(m.get("largeJumpRate", 0.0) for m in metrics)

    w0_wrap, w1_wrap = wrap_rate(w0), wrap_rate(w1)
    w0_jump, w1_jump = jump_rate(w0), jump_rate(w1)
    if w0_wrap > 0.001 or w0_jump > 0.001:
        return (
            "A: W0 already has full-range / wrap jumps. "
            "Wine conversion is not the first source; next is BASS/Box64 (then Windows A/B)."
        )
    if w1_wrap > 0.001 or w1_jump > 0.001:
        return (
            "B: W0 looks continuous, W1 first shows the jump. "
            "Investigate wineohos decode/volume/SRC/S16 conversion."
        )
    return (
        "C: neither dump shows wrap in this 2 s window. "
        "If pops were heard later, recapture during combat or compare Host H0."
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", required=True, help="m5_stream_*_c*.json from wineohos W0/W1 dump")
    args = parser.parse_args()
    if not os.path.isfile(args.json):
        print(f"missing {args.json}", file=sys.stderr)
        return 1
    meta = load_json(args.json)
    base = os.path.dirname(os.path.abspath(args.json))
    client = meta.get("clientPath") or os.path.join(
        base, os.path.basename(args.json).replace(".json", "_client.raw")
    )
    mix = meta.get("mixPath") or os.path.join(
        base, os.path.basename(args.json).replace(".json", "_mix.s16le")
    )
    if not os.path.isfile(client):
        alt = os.path.join(base, os.path.basename(client))
        if os.path.isfile(alt):
            client = alt
    if not os.path.isfile(mix):
        alt = os.path.join(base, os.path.basename(mix))
        if os.path.isfile(alt):
            mix = alt

    print(
        f"stream={meta.get('streamId')} capture={meta.get('captureId')} "
        f"formatTag={meta.get('formatTag')} rate={meta.get('sampleRate')} "
        f"ch={meta.get('channels')} bits={meta.get('bitsPerSample')} "
        f"float={meta.get('isFloat')} mask={meta.get('channelMask')} "
        f"clientFrames={meta.get('clientFrames')} mixFrames={meta.get('mixFrames')}"
    )
    w0 = []
    w1 = []
    if os.path.isfile(client):
        analyze_client(client, meta)
        if not int(meta.get("isFloat") or 0) and int(meta.get("bitsPerSample") or 16) == 16:
            data = open(client, "rb").read()
            w0 = analyze_bytes(data, int(meta.get("sampleRate") or 44100), int(meta.get("channels") or 2))
    else:
        print(f"missing W0 {client}")
    print("W1 converted mix (48k stereo S16)")
    if os.path.isfile(mix):
        data = open(mix, "rb").read()
        leftover = len(data) % 4
        w1 = analyze_bytes(data, 48000, 2)
        print_metrics(mix, 48000, 2, leftover, w1)
    else:
        print(f"missing W1 {mix}")
    print(verdict(w0, w1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
