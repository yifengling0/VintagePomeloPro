"""Exercise Wine's real PE address-space branches and explicit overrides."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/wine/0016-ntdll-honor-pe-large-address-aware-default.patch'
parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, default=ROOT / 'thirdparty/wine-valve')
args, unittest_args = parser.parse_known_args()

PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x20
#define IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA 0x20
#define IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE 0x40
static struct { unsigned int ImageCharacteristics, DllCharacteristics; } main_image_info;
static int is_win64, wow64, free_calls;
static char *limit_2g = (char *)0x80000000ULL;
static char *limit_4g = (char *)0x100000000ULL;
static char *address_space_start, *address_space_limit;
static char *user_space_limit, *working_set_limit, *user_space_wow_limit;
static int is_wow64(void) { return wow64; }
static void free_reserved_memory(char *start, char *end)
{ (void)start; (void)end; ++free_calls; }
'''
MAIN = r'''
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    wow64 = !strcmp(argv[1], "wow64");
    is_win64 = wow64 || !strcmp(argv[1], "native64");
    main_image_info.ImageCharacteristics = atoi(argv[2]);
    address_space_limit = is_win64 ? (char *)0x7fffffff0000ULL : limit_4g;
    user_space_limit = working_set_limit = limit_2g;
    user_space_wow_limit = limit_2g - 1;
    virtual_set_large_address_space();
    printf("{\"wow_limit\":%llu,\"user_limit\":%llu,\"working_limit\":%llu,"
           "\"start\":%llu,\"free_calls\":%d}\n",
           (unsigned long long)(uintptr_t)user_space_wow_limit,
           (unsigned long long)(uintptr_t)user_space_limit,
           (unsigned long long)(uintptr_t)working_set_limit,
           (unsigned long long)(uintptr_t)address_space_start, free_calls);
    return 0;
}
'''


def extract_policy(source):
    start = source.index('static int need_override_large_address_aware(void)')
    body = source.index('void virtual_set_large_address_space(void)', start)
    end = source.index('\n}\n', body) + 3
    return source[start:end]


class WineLargeAddressAwareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        relative = Path('dlls/ntdll/unix/virtual.c')
        fixture = cls.directory / relative
        fixture.parent.mkdir(parents=True)
        fixture.write_text((args.wine_src / relative).read_text())
        patch_args = ['patch', '-d', str(cls.directory), '-p1', '--batch', '--force']
        patch_text = PATCH.read_text()
        reverse = subprocess.run(patch_args + ['--dry-run', '-R'], input=patch_text,
                                 capture_output=True, text=True)
        if reverse.returncode == 0:
            subprocess.run(patch_args + ['-R'], input=patch_text, capture_output=True,
                           text=True, check=True)
        baseline = fixture.read_text()
        subprocess.run(patch_args, input=patch_text, capture_output=True, text=True, check=True)
        candidate = fixture.read_text()
        cls.binaries = {}
        for name, source in [('baseline', baseline), ('candidate', candidate)]:
            c_file = cls.directory / (name + '.c')
            c_file.write_text(PRELUDE + extract_policy(source) + MAIN)
            binary = cls.directory / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            str(c_file), '-o', str(binary)], check=True)
            cls.binaries[name] = binary

    def run_policy(self, mode, characteristics, override, name='candidate'):
        environment = os.environ.copy()
        environment.pop('WINE_LARGE_ADDRESS_AWARE', None)
        if override is not None:
            environment['WINE_LARGE_ADDRESS_AWARE'] = override
        result = subprocess.run([str(self.binaries[name]), mode, str(characteristics)],
                                env=environment, capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def test_wow64_image_contract_and_overrides(self):
        expectations = [
            (0, None, 0x7fffffff), (0, '0', 0x7fffffff), (0, '1', 0xffffffff),
            (0x20, None, 0xffffffff), (0x20, '0', 0xffffffff), (0x20, '1', 0xffffffff),
        ]
        for characteristics, override, limit in expectations:
            with self.subTest(characteristics=characteristics, override=override):
                result = self.run_policy('wow64', characteristics, override)
                self.assertEqual(result['wow_limit'], limit)

    def test_native32_reservation_release(self):
        expectations = [
            (0, None, 0), (0, '0', 0), (0, '1', 1),
            (0x20, None, 1), (0x20, '0', 1), (0x20, '1', 1),
        ]
        for characteristics, override, calls in expectations:
            with self.subTest(characteristics=characteristics, override=override):
                result = self.run_policy('native32', characteristics, override)
                self.assertEqual(result['free_calls'], calls)
                self.assertEqual(result['user_limit'], 0x100000000 if calls else 0x80000000)
                self.assertEqual(result['working_limit'], result['user_limit'])

    def test_native64_behavior_matches_baseline(self):
        for characteristics in (0, 0x20):
            for override in (None, '0', '1'):
                with self.subTest(characteristics=characteristics, override=override):
                    result = self.run_policy('native64', characteristics, override)
                    self.assertEqual(result, self.run_policy('native64', characteristics,
                                                             override, 'baseline'))
                    self.assertEqual(result['start'], 0x10000)

    def test_old_default_is_reproduced(self):
        self.assertEqual(self.run_policy('wow64', 0, None, 'baseline')['wow_limit'], 0xffffffff)
        self.assertEqual(self.run_policy('wow64', 0, None)['wow_limit'], 0x7fffffff)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + unittest_args)
