#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verified executable cache, adapted from AwesomeObserver/windows.

This caches only base linking. Every launch still validates/prepares the current
mod content, engine overlays, content profile and selected XML patches.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 3
GAME_INPUTS = ('eboot.bin', 'sce_sys/param.sfo', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx')
SCRIPT_INPUTS = ('prepare.py', 'game_check.py', 'engine_assets.py', 'link_libc.py', 'link_modules.py', 'prepare_cache.py', 'redzone.py')
# Unlike the reference, our live native profile and engine asset tools consume
# the complete ELF on every launch. Preserve and verify it, not only its headers.
OUTPUTS = ('boot-linked.bin', 'eboot.elf')

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def fingerprint(game, root, env):
    return {'game': {n: digest(game / n) for n in GAME_INPUTS},
            'scripts': {n: digest(root / 'scripts' / n) for n in SCRIPT_INPUTS},
            'redzone_table': digest(root / 'patches/redzone.json'),
            'redzone_enabled': env.get('BB_RED_ZONE', '1' if os.name == 'nt' else '0'),
            'skip_game_check': env.get('BB_SKIP_GAME_CHECK') == '1'}

@contextmanager
def preparation_lock(out):
    """OS lock is released even after interruption; never trust a PID lock file."""
    with (out / 'prepare-cache.lock').open('a+b', buffering=0) as stream:
        if os.name == 'nt':
            import msvcrt
            if not stream.seek(0, os.SEEK_END):
                stream.write(b'0')
            deadline = time.monotonic() + 120
            while True:
                stream.seek(0)
                try:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Another executable preparation still owns the cache')
                    time.sleep(.1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream, fcntl.LOCK_UN)

def prepare_game(game, out, runner, *, root=ROOT, env=None):
    game, out, root = Path(game), Path(out), Path(root)
    out.mkdir(parents=True, exist_ok=True)
    with preparation_lock(out):
        return _prepare_game(game, out, runner, root, os.environ if env is None else env)

def _prepare_game(game, out, runner, root, env):
    manifest = out / 'prepare-cache.json'
    enabled = env.get('BB_PREPARE_CACHE') != '0'
    started = time.perf_counter()
    inputs = None
    if enabled:
        try:
            inputs = fingerprint(game, root, env)
            if manifest.stat().st_size > 65536:
                raise ValueError('Oversized preparation manifest')
            saved = json.loads(manifest.read_text(encoding='utf-8'))
            if (isinstance(saved, dict) and saved.get('schema') == SCHEMA and
                    saved.get('inputs') == inputs and
                    saved.get('output_sizes') == {n: (out / n).stat().st_size for n in OUTPUTS} and
                    saved.get('outputs') == {n: digest(out / n) for n in OUTPUTS}):
                print(f'Prepared image: verified cache hit ({time.perf_counter()-started:.3f}s)', flush=True)
                return True
        except (OSError, ValueError):
            pass
    else:
        try:
            inputs = fingerprint(game, root, env)
        except OSError:
            pass # Normal validation below reports the original missing-file error.
    # Old output may remain for diagnosis, but never an old success manifest.
    manifest.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='prepare-stage-', dir=out) as directory:
        stage = Path(directory)
        runner('prepare.py', game, '--out', stage, '--no-resource-inventory')
        # link_modules already links libc; boot-libc.bin is not used by Windows.
        runner('link_modules.py', game, '--out', stage)
        output_hashes = {n: digest(stage / n) for n in OUTPUTS}
        if inputs is not None and fingerprint(game, root, env) != inputs:
            raise ValueError('Executable inputs changed during preparation; launch refused')
        for name in OUTPUTS:
            os.replace(stage / name, out / name)
        for name in ('boot.bin', 'analysis.json', 'entry.bin'):
            if (stage / name).is_file():
                os.replace(stage / name, out / name)
    if enabled and inputs is not None:
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', prefix='prepare-manifest-',
                                             suffix='.tmp', dir=out, delete=False) as stream:
                temporary = Path(stream.name)
                json.dump({'schema': SCHEMA, 'inputs': inputs, 'outputs': output_hashes,
                           'output_sizes': {n: (out / n).stat().st_size for n in OUTPUTS}}, stream)
                stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, manifest)
        except OSError as error:
            print(f'Prepared image: cache manifest unavailable ({type(error).__name__}); next launch will rebuild', flush=True)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    print(f'Prepared image: rebuilt ({time.perf_counter()-started:.3f}s)', flush=True)
    return False
