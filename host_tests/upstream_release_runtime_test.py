#!/usr/bin/env python3
"""Release boundaries: reject stale bytes and source labels that lie about them."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from upstream_release_runtime import RAW, verify


class ReleaseRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.lock = dict(wine=dict(adaptedCommit='new-wine', lockSha256='wine-lock'),
                         fex=dict(adaptedCommit='new-fex', lockSha256='fex-lock'))
        self.manifest = dict(componentOverrides=copy.deepcopy(self.lock))
        self.data = {RAW + 'wine-data.zip': b'new complete Wine and FEX payload'}
        self.refresh()

    def refresh(self):
        self.data[RAW + 'wine-runtime-manifest.json'] = json.dumps(self.manifest).encode()
        self.lock['members'] = {n: dict(sha256=hashlib.sha256(v).hexdigest()) for n, v in self.data.items()}

    def check(self):
        return verify(lambda n: io.BytesIO(self.data[n]), self.data, self.lock)

    def test_selected_pair_passes(self):
        self.assertEqual(self.check()['wine'], 'new-wine')

    def test_old_payload_with_new_labels_fails(self):
        self.data[RAW + 'wine-data.zip'] = b'old runtime with copied new labels'
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            self.check()

    def test_old_wine_selection_fails(self):
        self.manifest['componentOverrides']['wine']['adaptedCommit'] = 'old-wine'
        self.refresh()
        with self.assertRaisesRegex(ValueError, 'Old or mixed'):
            self.check()

    def test_mixed_fex_source_lock_fails(self):
        self.manifest['componentOverrides']['fex']['lockSha256'] = 'old-fex-lock'
        self.refresh()
        with self.assertRaisesRegex(ValueError, 'source lock mismatch'):
            self.check()

    def test_incomplete_package_fails(self):
        self.data.pop(RAW + 'wine-data.zip')
        with self.assertRaisesRegex(ValueError, 'member missing'):
            self.check()

    def test_controller_metadata_passes(self):
        metadata = dict(schemaVersion=1, slots=4, patchSha256='paired-controller')
        self.lock['controller'] = dict(runtimeMetadata=metadata)
        self.manifest['controllerRuntime'] = copy.deepcopy(metadata)
        self.refresh()
        self.assertEqual(self.check()['wine'], 'new-wine')

    def test_old_controller_runtime_fails(self):
        self.lock['controller'] = dict(runtimeMetadata=dict(slots=4))
        self.refresh()
        with self.assertRaisesRegex(ValueError, 'Unpaired controller'):
            self.check()


if __name__ == '__main__':
    unittest.main()
