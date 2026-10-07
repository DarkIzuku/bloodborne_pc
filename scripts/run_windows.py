#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepare the game image, compile the patches and start
out/bb-probe.exe. Same environment variables and bbport.ini settings as run.sh.

    run.bat [--game-dir DIR] [bb-probe options...]

The game folder: --game-dir, else BB_GAME_DIR, else the last one used (out/game_dir.txt), else
../CUSA03173 (as run.sh). Unless BB_PREBUILT=1
the port is (re)built first through MSYS2 (build.sh in the CLANG64 environment)."""
import atexit
from datetime import datetime
import os
from pathlib import Path
import shlex
import subprocess
import sys
from mods import remove_overlay

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / 'scripts'
PYTHON = sys.executable
LOG_STREAM = None


def init_session_log():
    """Route launcher/runtime diagnostics to BB_LOG_FILE when the GUI launcher supplies one."""
    global LOG_STREAM
    path = os.environ.get('BB_LOG_FILE')
    if not path:
        return
    log_path = Path(path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    LOG_STREAM = log_path.open('a', encoding='utf-8', errors='backslashreplace', buffering=1)
    atexit.register(LOG_STREAM.close)
    sys.stdout = LOG_STREAM
    sys.stderr = LOG_STREAM
    print('=' * 80)
    print(f'Bloodborne PC session log started: {datetime.now().isoformat(timespec="seconds")}')
    print(f'Launcher root: {ROOT}')
    print(f'Log file: {log_path.resolve()}')
    print('=' * 80, flush=True)


def msys_root():
    return Path(os.environ.get('BB_MSYS2', r'C:\msys64'))


def run(arguments, capture=False, check=True, env=None):
    result = subprocess.run(
        [str(a) for a in arguments],
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE if capture else LOG_STREAM,
        stderr=LOG_STREAM,
        text=True,
    )
    if capture and LOG_STREAM is not None and result.stdout:
        LOG_STREAM.write(result.stdout)
        if not result.stdout.endswith('\n'):
            LOG_STREAM.write('\n')
        LOG_STREAM.flush()
    if check and result.returncode:
        print(f'Command failed with exit code {result.returncode}: '
              f'{" ".join(shlex.quote(str(a)) for a in arguments)}', flush=True)
        sys.exit(result.returncode)
    return result.stdout.strip() if capture else result.returncode


def build():
    """build.sh in a CLANG64 login shell (its clang, cmake, ninja and pkg-config)."""
    bash = msys_root() / 'usr/bin/bash.exe'
    if not bash.is_file():
        sys.exit(f'MSYS2 not found at {msys_root()} (set BB_MSYS2, or BB_PREBUILT=1 with a built out/)')
    env = dict(os.environ, MSYSTEM='CLANG64', CHERE_INVOKING='1')
    run([bash, '-lc', 'bash build.sh'], env=env)


def settings_value(config, key):
    if not config.is_file():
        return None
    for line in config.read_text(errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def main():
    arguments = sys.argv[1:]
    game = os.environ.get('BB_GAME_DIR')
    if arguments[:1] == ['--game-dir'] and len(arguments) > 1:
        game, arguments = arguments[1], arguments[2:]
    data = Path(os.environ.get('BB_DATA_DIR', ROOT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = Path(os.environ['BB_CONFIG'])
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    # The last folder that worked is remembered, so run.bat alone starts the game afterwards.
    remembered = out / 'game_dir.txt'
    if not game and remembered.is_file():
        game = remembered.read_text(encoding='utf-8').strip()
    game = Path(game) if game else ROOT.parent / 'CUSA03173'
    if not (game / 'eboot.bin').is_file():
        sys.exit(f'No eboot.bin in {game} (pass --game-dir or set BB_GAME_DIR).')
    original = game.resolve()
    remembered.write_text(str(original), encoding='utf-8')
    os.environ['BB_GAME_DIR'] = str(original)

    # Keep the original classic movie structure and resource package format. Build six GFX
    # variants that only rename the existing external TGA resource, plus one extended TPF that
    # contains those six resources. This survives the game's resource caching and keeps the dump
    # read-only.
    os.environ.pop('BB_CUSTOM_LOADING_GFX', None)
    os.environ.pop('BB_CUSTOM_LOADING_GFX_DIR', None)
    os.environ.pop('BB_CUSTOM_LOADING_TPF_DIR', None)
    os.environ.pop('BB_CUSTOM_LOADING_TPF', None)
    classic_gfx = original / 'dvdroot_ps4' / 'menu' / 'nowloading.gfx'
    classic_tpf = original / 'dvdroot_ps4' / 'menu' / 'nowloading.tpf.dcx'
    loading_assets = ROOT / 'assets' / 'loading_screens'
    texconv = ROOT / 'tools' / 'directxtex' / 'texconv.exe'
    witchy = ROOT / 'tools' / 'witchybnd' / 'WitchyBND.exe'
    custom_dir = out / 'ui' / 'loading_screens'
    gfx_builder = SCRIPTS / 'build_loading_gfx.py'
    tpf_builder = SCRIPTS / 'build_loading_tpf.py'
    custom_gfx = [custom_dir / f'nowloading-custom-{index:02d}.gfx'
                  for index in range(1, 7)]
    custom_tpf = custom_dir / 'nowloading-custom.tpf.dcx'
    if (classic_gfx.is_file() and classic_tpf.is_file() and loading_assets.is_dir()
            and gfx_builder.is_file() and tpf_builder.is_file()
            and texconv.is_file() and witchy.is_file()):
        gfx_status = run([PYTHON, gfx_builder, classic_gfx,
                          '--images-dir', loading_assets, '--out-dir', custom_dir], check=False)
        tpf_status = run([PYTHON, tpf_builder, classic_tpf,
                          '--images-dir', loading_assets, '--texconv', texconv,
                          '--witchy', witchy, '--output', custom_tpf], check=False)
        if (gfx_status == 0 and tpf_status == 0 and
                all(path.is_file() for path in custom_gfx) and custom_tpf.is_file()):
            os.environ['BB_CUSTOM_LOADING_GFX_DIR'] = str(custom_dir.resolve())
            os.environ['BB_CUSTOM_LOADING_TPF'] = str(custom_tpf.resolve())
        else:
            print('Loading screens: custom GFX/TPF resources unavailable; using the original classic screen')
    else:
        print('Loading screens: classic GFX/TPF, packaged artwork, texconv or WitchyBND missing; '
              'using the original classic screen')

    # The in-game menu's "Apply and restart" runs this launcher again (probe.c runtime_restart).
    os.environ['BB_RESTART_COMMAND'] = subprocess.list2cmdline([PYTHON, str(Path(__file__).resolve()), *sys.argv[1:]])

    merged = Path(run([PYTHON, SCRIPTS / 'mods.py', original, '--out', out,
                       '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                       '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                       '--enabled', os.environ.get('BB_MODS_ENABLED', '1')], capture=True))
    overlay = merged if merged.resolve() != original else None
    try:
        for script, extra in (('prepare.py', []), ('link_libc.py', []), ('link_modules.py', []),
                              ('content_profile.py', ['--sku', os.environ.get('BB_CONTENT_SKU', 'full')])):
            run([PYTHON, SCRIPTS / script, merged, '--out', out, *extra])
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if os.environ.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                os.environ.pop(key, None)
        fps = os.environ.get('BB_FPS', 'uncap')
        scaled_render = scaled_output = None
        if not os.environ.get('BB_RENDER_RES'):
            sizes = run([PYTHON, SCRIPTS / 'patches.py', '--print-scaled', '--settings', config], capture=True, check=False)
            if sizes and len(sizes.split()) == 2:
                scaled_render, scaled_output = sizes.split()
        prebuilt = os.environ.get('BB_PREBUILT') == '1'
        if not prebuilt:
            build()
        live = '0'
        if scaled_output:
            live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = out / 'bb-gpu-capabilities.exe'
                live = run([caps, '--live-resolution'], capture=True, check=False) or '0'
            live = '1' if live == '1' else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        elif scaled_output:
            os.environ.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            os.environ.setdefault('BB_DMEM_MB', '9152')
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {os.environ["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        run([PYTHON, SCRIPTS / 'patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
             '--settings', config, '--game-dir', merged, '--render-res', os.environ.get('BB_RENDER_RES', ''),
             '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
             '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
             '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json')])
        os.environ.setdefault('BB_VBLANK_HZ', {'uncap': '0', '90': '90'}.get(fps, '60'))
        probe = ROOT / os.environ.get('BB_PROBE', out / 'bb-probe.exe')
        command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
                   '--patches', out / 'patches.bin', '--app0', merged,
                   '--user', os.environ.get('BB_USER_DIR', data / 'user'),
                   '--timeout', os.environ.get('BB_TIMEOUT', '0'), *arguments]
        # MSYS2's DLLs (libc++, SDL3, FFmpeg, ...). System32 is searched before PATH, so the
        # Vulkan loader stays the one installed with the GPU driver.
        os.environ['PATH'] = os.pathsep.join([str(msys_root() / 'clang64/bin'), os.environ.get('PATH', '')])
        print('Starting:', ' '.join(shlex.quote(str(c)) for c in command), flush=True)
        try:
            status = subprocess.call(
                [str(c) for c in command],
                cwd=ROOT,
                stdout=LOG_STREAM,
                stderr=LOG_STREAM,
            )
        except KeyboardInterrupt:
            status = 130
        print(f'bb-probe exited with code {status}', flush=True)
        return status
    finally:
        if overlay:
            remove_overlay(overlay)


if __name__ == '__main__':
    init_session_log()
    sys.exit(main())
