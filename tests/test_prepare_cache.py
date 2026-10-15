"""Content verification, invalidation and failure recovery for launch preparation."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import prepare_cache as cache

WORKER = r'''
import importlib.util, sys, time
from pathlib import Path
spec = importlib.util.spec_from_file_location('cache', sys.argv[1])
cache = importlib.util.module_from_spec(spec); spec.loader.exec_module(cache)
root, mode = Path(sys.argv[2]), sys.argv[3]
(root / (mode + '-started')).write_text('started')
def runner(name, *args):
    with (root / 'calls').open('a') as log: log.write(name + '\n')
    if name == 'prepare.py' and mode == 'holding':
        (root / 'holding-lock').write_text('locked')
        deadline = time.monotonic() + 10
        while not (root / 'release-lock').exists():
            if time.monotonic() > deadline: raise TimeoutError('test release absent')
            time.sleep(.01)
    if name == 'link_modules.py':
        stage = Path(args[args.index('--out') + 1])
        for name in cache.OUTPUTS: (stage / name).write_bytes(name.encode())
hit = cache.prepare_game(root / 'game', root / 'out', runner, root=root, env={})
print('HIT' if hit else 'REBUILT', flush=True)
'''


class PrepareCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bb preparation ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game, self.out = self.root / 'game', self.root / 'out'
        self.out.mkdir()
        for name in cache.GAME_INPUTS:
            path = self.game / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'original')
        for name in cache.SCRIPT_INPUTS:
            path = self.root / 'scripts' / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(name, encoding='utf-8')
        self.calls = []
        (self.root / 'patches').mkdir()
        (self.root / 'patches/redzone.json').write_text('{}')
        self.env = {}

    def runner(self, name, *args):
        self.calls.append((name, args))
        if name == 'link_modules.py':
            for output in cache.OUTPUTS:
                (Path(args[args.index('--out')+1]) / output).write_bytes(output.encode())

    def prepare(self, runner=None, game=None):
        return cache.prepare_game(game or self.game, self.out, runner or self.runner,
                                  root=self.root, env=self.env)

    def worker(self, mode):
        child = subprocess.Popen([sys.executable, '-c', WORKER, cache.__file__, str(self.root), mode],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def cleanup():
            if child.poll() is None:
                child.terminate()
            child.communicate(timeout=10)
        self.addCleanup(cleanup)
        return child

    def wait_for(self, name):
        deadline = time.monotonic() + 10
        while not (self.root / name).is_file():
            self.assertLess(time.monotonic(), deadline, 'worker did not reach ' + name)
            time.sleep(.01)

    def test_verified_hit_avoids_both_preparation_processes(self):
        self.assertFalse(self.prepare())
        self.assertEqual([name for name, _ in self.calls], ['prepare.py', 'link_modules.py'])
        self.assertIn('--no-resource-inventory', self.calls[0][1])
        self.calls.clear()
        self.assertTrue(self.prepare())
        self.assertEqual(self.calls, [])

    def test_each_game_input_invalidates_even_with_same_size_and_mtime(self):
        for name in cache.GAME_INPUTS:
            with self.subTest(name=name):
                self.prepare()
                path = self.game / name
                stamp = path.stat()
                path.write_bytes(b'modified')
                os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                self.assertFalse(self.prepare())
                self.assertTrue(self.prepare())

    def test_each_preparation_script_invalidates(self):
        for name in cache.SCRIPT_INPUTS:
            self.prepare()
            with (self.root / 'scripts' / name).open('ab') as file:
                file.write(b'\n# changed')
            self.assertFalse(self.prepare(), name)

    def test_each_output_is_verified_and_missing_or_corrupt_files_rebuilt(self):
        self.prepare()
        for name in cache.OUTPUTS:
            with self.subTest(name=name):
                path = self.out / name
                path.write_bytes(b'corrupt')
                self.assertFalse(self.prepare())
                path.unlink()
                self.assertFalse(self.prepare())

    def test_check_bypass_is_part_of_key(self):
        self.prepare()
        self.env['BB_SKIP_GAME_CHECK'] = '1'
        self.assertFalse(self.prepare())
        self.env.pop('BB_SKIP_GAME_CHECK')
        self.assertFalse(self.prepare())

    def test_redzone_inputs_invalidate_linking(self):
        self.prepare()
        self.env["BB_RED_ZONE"]="0"
        self.assertFalse(self.prepare())
        self.assertTrue(self.prepare())
        (self.root/"patches/redzone.json").write_text("changed")
        self.assertFalse(self.prepare())

    def test_disable_forces_rebuild_and_removes_manifest(self):
        self.prepare()
        self.env['BB_PREPARE_CACHE'] = '0'
        self.assertFalse(self.prepare())
        self.assertFalse((self.out / 'prepare-cache.json').exists())
        self.assertFalse(self.prepare())

    def test_failed_rebuild_cannot_keep_previous_success_record(self):
        self.prepare()
        (self.game / 'eboot.bin').write_bytes(b'new dump')

        def fail(name, *args):
            (self.out / 'boot.bin').write_bytes(b'partial output')
            raise RuntimeError('link failed')

        with self.assertRaisesRegex(RuntimeError, 'link failed'):
            self.prepare(runner=fail)
        self.assertFalse((self.out / 'prepare-cache.json').exists())
        self.assertFalse(self.prepare())

    def test_inputs_changed_during_build_are_not_cached(self):
        def change(name, *args):
            self.runner(name, *args)
            if name == 'link_modules.py':
                (self.game / 'eboot.bin').write_bytes(b'changed during build')
        with self.assertRaisesRegex(ValueError, 'changed during preparation'):
            self.prepare(runner=change)
        self.assertFalse((self.out / 'prepare-cache.json').exists())

    def test_identical_mod_views_reuse_image_but_patched_executable_does_not(self):
        self.prepare()
        view = self.root / 'mod view'
        shutil.copytree(self.game, view)
        (view / 'dvdroot_ps4').mkdir()
        (view / 'dvdroot_ps4' / 'asset.bin').write_bytes(b'asset mod')
        self.assertTrue(self.prepare(game=view))
        (view / 'eboot.bin').write_bytes(b'executable mod')
        self.assertFalse(self.prepare(game=view))

    def test_diagnostic_intermediates_are_not_read_on_hit(self):
        self.prepare()
        (self.out / 'boot.bin').write_bytes(b'unused intermediate')
        (self.out / 'analysis.json').write_text('diagnostic inventory', encoding='utf-8')
        self.assertTrue(self.prepare())
        self.assertTrue((self.out / 'eboot.elf').exists())

    def test_invalid_manifest_rebuilds(self):
        for contents in ('{', '[]', '{"schema": 0}', '{"schema": 1, "outputs": []}'):
            (self.out / 'prepare-cache.json').write_text(contents, encoding='utf-8')
            self.assertFalse(self.prepare())

    def test_missing_input_still_runs_normal_validation(self):
        (self.game / 'eboot.bin').unlink()
        with self.assertRaises(FileNotFoundError):
            self.prepare(runner=lambda *args: (self.game / 'eboot.bin').read_bytes())
        self.assertFalse((self.out / 'prepare-cache.json').exists())

    def test_concurrent_processes_publish_only_one_complete_image(self):
        first = self.worker('holding')
        self.wait_for('holding-lock')
        second = self.worker('normal')
        self.wait_for('normal-started')
        # The first process still owns the lock; no second preparation may run.
        time.sleep(.15)
        self.assertIsNone(second.poll())
        self.assertEqual((self.root / 'calls').read_text().splitlines(), ['prepare.py'])
        (self.root / 'release-lock').write_text('release')
        outputs = [child.communicate(timeout=15) for child in (first, second)]
        self.assertEqual([child.returncode for child in (first, second)], [0, 0], outputs)
        self.assertIn('REBUILT', outputs[0][0])
        self.assertIn('HIT', outputs[1][0])
        self.assertEqual((self.root / 'calls').read_text().splitlines(), ['prepare.py', 'link_modules.py'])
        self.assertTrue(self.prepare())

    def test_interrupted_producer_releases_lock_without_publishing_success(self):
        first = self.worker('holding')
        self.wait_for('holding-lock')
        first.terminate()
        first.communicate(timeout=10)
        self.assertFalse((self.out / 'prepare-cache.json').exists())
        second = self.worker('normal')
        output, error = second.communicate(timeout=15)
        self.assertEqual(second.returncode, 0, error)
        self.assertIn('REBUILT', output)
        self.assertTrue(self.prepare())
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main()
