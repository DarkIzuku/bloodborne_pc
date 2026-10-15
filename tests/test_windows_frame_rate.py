from paths import ROOT
import unittest
from run_windows import configure_frame_rate


class WindowsFrameRateTests(unittest.TestCase):
    def test_supported_presets_override_inherited_unbounded_values(self):
        for fps in ('30', '60', '90', '120'):
            with self.subTest(fps=fps):
                env = {'BB_FPS': fps, 'BB_FPS_LIMIT': '0', 'BB_VBLANK_HZ': '480'}
                self.assertEqual(configure_frame_rate(env), fps)
                self.assertEqual(env['BB_FPS_LIMIT'], fps)
                self.assertEqual(env['BB_VBLANK_HZ'], fps if fps in ('90','120') else '60')

    def test_uncapped_gameplay_is_guarded_by_the_runtime(self):
        env = {'BB_FPS': 'uncap', 'BB_FPS_LIMIT': '60', 'BB_VBLANK_HZ': '60'}
        self.assertEqual(configure_frame_rate(env), 'uncap')
        self.assertEqual(env['BB_FPS_LIMIT'], '0')
        self.assertEqual(env['BB_VBLANK_HZ'], '480')

    def test_default_and_malformed_presets_are_capped_at_60(self):
        for value in (None, 'Unlimited', '', '0', '480', 'bad'):
            with self.subTest(value=value):
                env = {'BB_VBLANK_HZ': '0', 'BB_FPS_LIMIT': '0'}
                if value is not None:
                    env['BB_FPS'] = value
                self.assertEqual(configure_frame_rate(env), '60')
                self.assertEqual(env['BB_FPS'], '60')
                self.assertEqual(env['BB_FPS_LIMIT'], '60')
                self.assertEqual(env['BB_VBLANK_HZ'], '60')


if __name__ == '__main__':
    unittest.main()
