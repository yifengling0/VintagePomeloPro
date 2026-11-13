#!/usr/bin/env python3
"""Portable bit-oracle and overlay integration checks for WOW64 exact-store."""
import argparse
import random
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


def convert(sig, sign_exp, bits):
    exp = sign_exp & 0x7fff
    sign = sign_exp >> 15
    if bits == 32:
        low, emin, emax, fracbits = 40, 0x3f81, 0x407e, 23
    else:
        low, emin, emax, fracbits = 11, 0x3c01, 0x43fe, 52
    if not (sig >> 63) or sig & ((1 << low) - 1) or not emin <= exp <= emax:
        return None
    return (sign << (bits - 1)) | ((exp - emin + 1) << fracbits) | ((sig >> low) & ((1 << fracbits) - 1))


def oracle(sig, sign_exp, bits):
    exp = sign_exp & 0x7fff
    sign = sign_exp >> 15
    precision = 24 if bits == 32 else 53
    bias = 127 if bits == 32 else 1023
    unbiased = exp - 0x3fff
    target_exp = unbiased + bias
    discarded = 64 - precision
    if not (sig >> 63) or sig & ((1 << discarded) - 1) or not 1 <= target_exp < (1 << (bits - precision)) - 1:
        return None
    fraction = (sig >> discarded) & ((1 << (precision - 1)) - 1)
    return (sign << (bits - 1)) | (target_exp << (precision - 1)) | fraction


def test_vectors():
    negatives = [
        (0, 0), (0, 0x8000),                         # signed zero
        (1 << 62, 1),                                # denormal
        (1 << 63, 0),                                # pseudo-denormal
        (1 << 62, 0x3fff),                           # unnormal
        (1 << 63, 0x7fff),                           # infinity
        ((1 << 63) | (1 << 62), 0x7fff),             # qNaN
        ((1 << 63) | 1, 0x7fff),                     # sNaN
    ]
    for bits, low, lo, hi in ((32, 40, 0x3f81, 0x407e), (64, 11, 0x3c01, 0x43fe)):
        positives = []
        for sign in (0, 0x8000):
            for exp in (lo, lo + 1, 0x3fff, hi - 1, hi):
                for frac in (0, 1, (1 << (63 - low)) - 1, 0x15555 & ((1 << (63 - low)) - 1)):
                    positives.append(((1 << 63) | (frac << low), sign | exp))
        for sig, se in positives:
            got = convert(sig, se, bits)
            assert got == oracle(sig, se, bits) and got is not None
            packed = struct.pack('>I' if bits == 32 else '>Q', got)
            assert len(packed) == bits // 8
        rejected = negatives + [
            (1 << 63, lo - 1), (1 << 63, hi + 1),
            ((1 << 63) | 1, lo), ((1 << 63) | (1 << (low - 1)), 0x3fff),
            ((1 << 63) | ((1 << low) - 1), hi),
        ]
        for sig, se in rejected:
            assert convert(sig, se, bits) is None
        rng = random.Random(0xF80 + bits)
        for _ in range(20000):
            sig, se = rng.getrandbits(64), rng.getrandbits(16)
            assert convert(sig, se, bits) == oracle(sig, se, bits)


def test_integration(root, fex):
    patch = root / 'scripts/patches/fex-wow64-exact-store.patch'
    diagnostics = root / 'scripts/patches/fex-wow64-exact-store-diagnostics.patch'
    build = (root / 'scripts/build_fex.sh').read_text()
    game_hook = (root / 'entry/src/main/ets/game/GameHook.ets').read_text()
    text = patch.read_text()
    # `git diff --check` sees the mandatory one-space prefix on blank unified
    # diff context lines as whitespace in this patch-as-data file. Validate
    # the source lines introduced by each overlay directly instead of
    # rewriting valid context or weakening patch applicability.
    for overlay in (patch, diagnostics):
        for number, line in enumerate(overlay.read_text().splitlines(), 1):
            if line.startswith('+') and not line.startswith('+++'):
                assert line[1:] == line[1:].rstrip(' \t'), \
                    f'{overlay.name}:{number}: added source has trailing whitespace'
    assert 'FEX_WOW64_EXACT_STORE' in text and 'FEX_EXACTSTORE' in text
    assert 'std::strcmp(ExactStore, "1") == 0' in text
    assert 'exact_store_flags="-DFEX_WOW64_EXACT_STORE=1"' in build
    assert 'FEX_EXACTSTORE_DIAGNOSTICS:-0' in build
    assert "key === 'FEX_EXACTSTORE'" in game_hook
    assert "key === 'FEX_EXACTSTORE_STATS'" in game_hook
    ec = build[build.index('build_fex_ec()'):build.index('build_fex_pe()')]
    pe = build[build.index('build_fex_pe()'):]
    assert 'FEX_WOW64_EXACT_STORE=1' not in ec and 'FEX_WOW64_EXACT_STORE=1' in pe
    with tempfile.TemporaryDirectory(prefix='fex-exact-') as tmp:
        dst = Path(tmp) / 'fex'
        shutil.copytree(fex, dst, ignore=shutil.ignore_patterns('.git', 'build', 'Build'))
        subprocess.run(['patch', '-d', str(dst), '-p1', '--dry-run', '-s'], stdin=patch.open('rb'), check=True)
        subprocess.run(['patch', '-d', str(dst), '-p1', '-s'], stdin=patch.open('rb'), check=True)
        reverse = subprocess.run(['patch', '-d', str(dst), '-p1', '-R', '--dry-run', '-s'], stdin=patch.open('rb'))
        assert reverse.returncode == 0
        subprocess.run(['patch', '-d', str(dst), '-p1', '--dry-run', '-s'], stdin=diagnostics.open('rb'), check=True)
        subprocess.run(['patch', '-d', str(dst), '-p1', '-s'], stdin=diagnostics.open('rb'), check=True)
        # Diagnostics intentionally changes adjacent functional hunks, so the
        # build script uses explicit full-overlay markers on repeated runs.
        assert 'WindowsExactFloatStoreEnabled' in (dst / 'FEXCore/Source/Interface/Context/Context.h').read_text()
        assert 'std::getenv("FEX_EXACTSTORE")' in (dst / 'FEXCore/Source/Interface/Core/Core.cpp').read_text()
        assert 'CountExactStore' in (dst / 'FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp').read_text()
        subprocess.run(['patch', '-d', str(dst), '-p1', '-R', '--dry-run', '-s'], stdin=diagnostics.open('rb'), check=True)
        dispatcher = (dst / 'FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp').read_text()
        for case, fill in (('FABI_F32_I16_F80_PTR', 'FillF32Result'), ('FABI_F64_I16_F80_PTR', 'FillF64Result')):
            block = dispatcher[dispatcher.index('case ' + case):]
            block = block[:block.index('} break;')]
            assert block.index('WindowsExactFloatStoreEnabled') < block.index('SpillForABICall')
            assert block.index('Bind(&Fallback)') < block.index('SpillForABICall')
            assert block.count('SystemRegister::NZCV') == 3
            assert 'TMP4' not in '\n'.join(line for line in block.splitlines() if not line.lstrip().startswith('//'))
            assert block.index(fill) < block.index('Bind(&ExactDone)')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--fex-src', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    test_vectors()
    test_integration(root, args.fex_src.resolve())
    print('PASS: exact-store oracle, reject vectors, overlay replay, and isolation')


if __name__ == '__main__':
    main()
