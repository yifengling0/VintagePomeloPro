#!/usr/bin/env python3
"""Exercise the actual Wine clipboard-window predicate retained in patch 0085."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/wine-upstream/0085-wayland-clipboard-window-class-termination.patch'


def predicate(text):
    match = re.search(r'static BOOL is_winewayland_clipboard_hwnd\(HWND hwnd\)\n\{.*?\n\}', text, re.S)
    if not match:
        raise ValueError('Missing production clipboard-window predicate')
    return match.group()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    # A complete function is retained as patch context, so a fresh checkout can
    # run this test without downloading the full Proton source tree.
    patched = '\n'.join(line[1:] for line in PATCH.read_text().splitlines()
                        if line.startswith((' ', '+')) and not line.startswith('+++'))
    code = predicate(patched)
    if args.source and predicate(args.source.read_text()) != code:
        raise ValueError('Selected Wine source differs from the tested patch')
    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uchar.h>
typedef uint16_t WCHAR;
typedef unsigned HWND;
typedef int BOOL;
typedef struct { uint16_t Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
#define FALSE 0
static const char16_t *class_name;
static unsigned NtUserGetClassName(HWND hwnd, BOOL real, UNICODE_STRING *name)
{
    (void)real;
    if (!hwnd) return 0;
    size_t length = 0;
    while (class_name[length]) ++length;
    assert((length + 1) * sizeof(WCHAR) <= name->MaximumLength);
    memcpy(name->Buffer, class_name, (length + 1) * sizeof(WCHAR));
    name->Length = length * sizeof(WCHAR);
    return length;
}
static int compare_utf16(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (int)*a - (int)*b;
}
#define wcscmp compare_utf16
'''
    harness += code + r'''
int main(void)
{
    class_name = u"__winewayland_clipboard_manager";
    assert(is_winewayland_clipboard_hwnd(1));
    assert(!is_winewayland_clipboard_hwnd(0));
    class_name = u"__winewayland_clipboard_manager_other";
    assert(!is_winewayland_clipboard_hwnd(1));
    class_name = u"__winewayland_clipboard_manage";
    assert(!is_winewayland_clipboard_hwnd(1));
    class_name = u"__wine_clipboard_manager";
    assert(!is_winewayland_clipboard_hwnd(1));
    class_name = u"Notepad";
    assert(!is_winewayland_clipboard_hwnd(1));
    puts("PASS Wine core-data-device clipboard window: exact class, other classes, invalid HWND; ASan/UBSan");
}
'''
    directory = ROOT / 'build/host_tests/wine-wayland-clipboard'
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / 'predicate.c'
    output = directory / 'predicate'
    source.write_text(harness)
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
