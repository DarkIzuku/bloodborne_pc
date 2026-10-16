"""Compile the real Windows CPU loader for early crash regressions, without Vulkan.

This does not replace the final production executable's crash tests. Neither the
GPU stand-in nor this synthetic executable is installed in the player artifact.
"""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out/crash-preflight'
if os.name != 'nt':
    raise SystemExit('This preflight exercises Windows exception/minidump APIs')
OUT.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC', 'clang')
includes = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'sdl3'], text=True))
libraries = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'sdl3'], text=True))
objects = []
for source in sorted((ROOT / 'third_party/LibAtrac9/C/src').glob('*.c')):
    obj = OUT / (source.stem + '.obj')
    subprocess.run([cc, '-std=c99', '-O2', '-w', '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
sources = [ROOT / 'src/probe.c', *sorted((ROOT / 'src').glob('runtime*.c')),
           *sorted((ROOT / 'src').glob('win32_*.c')), ROOT / 'tests/test_probe_crash_stubs.c']
subprocess.run([cc, '-std=gnu11', '-D_FILE_OFFSET_BITS=64', '-D_WIN32_WINNT=0x0A00',
                '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT), '-I'+str(ROOT/'src'),
                *includes, *map(str, sources), *objects, *libraries, '-lwinmm', '-lws2_32', '-lpsapi',
                '-Wl,--disable-dynamicbase,--disable-high-entropy-va',
                '-o', str(OUT/'probe-crash-only.exe')], check=True, cwd=ROOT)
print('Built CPU-only crash preflight from production loader/runtime sources')
