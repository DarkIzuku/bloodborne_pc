from paths import ROOT
import os
import tempfile
import unittest
from pathlib import Path
import prepare_stamp


class PrepareStampTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.game, self.out = root / 'game', root / 'out'
        (self.game / 'sce_sys').mkdir(parents=True)
        (self.game / 'sce_module').mkdir()
        (self.game / 'eboot.bin').write_bytes(b'eboot')
        (self.game / 'sce_sys/param.sfo').write_bytes(b'sfo')
        (self.game / 'sce_module/libc.prx').write_bytes(b'libc')
        self.out.mkdir()
        for name in prepare_stamp.OUTPUTS:
            (self.out / name).write_bytes(b'')
        os.environ.pop('BB_FORCE_PREPARE', None)

    def tearDown(self):
        self.tmp.cleanup()

    def test_unchanged_only_after_write_with_same_inputs(self):
        self.assertFalse(prepare_stamp.unchanged(self.game, self.out, 'full'))
        prepare_stamp.write(self.game, self.out, 'full')
        self.assertTrue(prepare_stamp.unchanged(self.game, self.out, 'full'))
        self.assertFalse(prepare_stamp.unchanged(self.game, self.out, 'trial'))

    def test_changed_input_missing_output_or_force_reruns(self):
        prepare_stamp.write(self.game, self.out, 'full')
        eboot = self.game / 'eboot.bin'
        os.utime(eboot, ns=(0, eboot.stat().st_mtime_ns + 1_000_000_000))
        self.assertFalse(prepare_stamp.unchanged(self.game, self.out, 'full'))
        prepare_stamp.write(self.game, self.out, 'full')
        (self.out / 'boot-linked.bin').unlink()
        self.assertFalse(prepare_stamp.unchanged(self.game, self.out, 'full'))
        (self.out / 'boot-linked.bin').write_bytes(b'')
        self.assertTrue(prepare_stamp.unchanged(self.game, self.out, 'full'))
        os.environ['BB_FORCE_PREPARE'] = '1'
        try:
            self.assertFalse(prepare_stamp.unchanged(self.game, self.out, 'full'))
        finally:
            del os.environ['BB_FORCE_PREPARE']


if __name__ == '__main__':
    unittest.main()
