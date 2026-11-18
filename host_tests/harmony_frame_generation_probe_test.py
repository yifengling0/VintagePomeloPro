#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise optional system loading, public query ABI and safe unsupported paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SYMBOLS = [
    'CreateContext', 'DestroyContext', 'SetAlgorithmMode', 'SetResolution',
    'SetImageFormat', 'SetIntegrationMode', 'Activate', 'Deactivate',
    'CreateImage', 'DestroyImage', 'Dispatch',
]


class CapabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vpp-harmony-fg-')
        cls.directory = Path(cls.temp.name)
        harness = cls.directory/'probe.cpp'
        harness.write_text('''
#include "gamescope/harmony_frame_generation.h"
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto c = winehua::gamescope::ProbeHarmonyFrameGeneration(argv[1]);
    printf("%d %d %d %s %s\\n", c.libraryLoaded, c.supportQueryAvailable,
           c.vulkanEntryPointsAvailable, c.deviceSupport.c_str(), c.stage.c_str());
}
''')
        cls.probe = cls.directory/'probe'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'entry/src/main/cpp'), str(harness),
                        '-ldl', '-o', str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def library(self, name, supported=None, omit=None):
        source = self.directory/(name+'.c')
        text = '#include <stdbool.h>\n#include <stdlib.h>\n'
        if supported is not None:
            text += ('bool HMS_FG_IsFrameGenerationSupported(int feature) {'
                     ' if (feature != 4) abort(); return '+str(supported).lower()+'; }\n')
        # Capability checks must never create/activate/dispatch a context.
        text += ''.join('void HMS_FG_'+s+'_VK(void) { abort(); }\n'
                        for s in SYMBOLS if s != omit)
        source.write_text(text)
        output = self.directory/(name+'.so')
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(output)], check=True)
        return output

    def query(self, path):
        return subprocess.check_output([self.probe, path], text=True, timeout=5).strip()

    def test_unavailable_library_is_unknown(self):
        self.assertEqual(self.query(self.directory/'missing.so'),
                         '0 0 0 unknown library-unavailable')

    def test_old_system_without_query_is_unknown(self):
        self.assertEqual(self.query(self.library('old')),
                         '1 0 0 unknown support-query-unavailable')

    def test_device_negative_is_distinct_from_loader_failure(self):
        self.assertEqual(self.query(self.library('unsupported', False)),
                         '1 1 0 unsupported device-unsupported')

    def test_positive_query_does_not_hide_missing_dispatch(self):
        self.assertEqual(self.query(self.library('incomplete', True, 'Dispatch')),
                         '1 1 0 supported vulkan-entry-point-unavailable')

    def test_complete_library_is_only_a_candidate(self):
        self.assertEqual(self.query(self.library('candidate', True)),
                         '1 1 1 supported candidate-available')


if __name__ == '__main__':
    unittest.main()
