"""Check main's paired controller contract against the effective Valve overlays."""
from pathlib import Path
import unittest
import wine_shm_state_cache_test as base

ROOT = Path(__file__).resolve().parents[1]


class ControllerProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)

    def test_version_and_coordinates_are_paired(self):
        host = (ROOT / 'entry/src/main/cpp/input/controller/gamepad_ipc_protocol.h').read_text()
        wine = self.first_replay['dlls/winebus.sys/winehua_gamepad_protocol.h'].decode()
        begin, end = '/* >>> WHGP_PROTOCOL_BEGIN */', '/* <<< WHGP_PROTOCOL_END */'
        block = lambda s: s[s.index(begin):s.index(end) + len(end)]
        self.assertEqual(block(host), block(wine))
        self.assertIn('#define WHGP_VERSION 2', block(host))
        bus = base.function(self.first_replay['dlls/winebus.sys/bus_ohos.c'].decode(),
                            'static void apply_state(')
        self.assertIn('whgp_stick_y_to_hid(body->ly)', bus)
        self.assertIn('whgp_hat_y_to_hid(body->hat_y)', bus)
        self.assertNotIn('ly = -', bus)

    def test_paired_napi_and_physical_update(self):
        prefix = ROOT / 'entry/src/main/cpp/input/controller'
        napi = base.function((prefix / 'controller_napi.cpp').read_text(), 'napi_value ControllerSetStick(')
        self.assertEqual(napi.count('.SetStick('), 1)
        self.assertNotIn('.SetAxis(', napi)
        physical = base.function((prefix / 'physical_gamepad.cpp').read_text(), 'void PhysicalFeedAxis(')
        self.assertIn('NormalizeOhosThumb(x, y)', physical)
        self.assertNotIn('.SetAxis(', physical)

    def test_replay(self):
        self.assertEqual(self.first_replay, self.second_replay)


if __name__ == '__main__':
    unittest.main(argv=[__file__] + base.remaining)
