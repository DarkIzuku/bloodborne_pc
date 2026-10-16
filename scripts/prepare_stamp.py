"""Skip prepare.py, link_libc.py, link_modules.py and content_profile.py when nothing they read
changed: their outputs are functions of the game files, the SKU and these scripts only. The
stamp holds (size, mtime_ns) of every input and the scripts' SHA-256. BB_FORCE_PREPARE=1
runs the chain regardless.

    prepare_stamp.py check GAME --out OUT --sku SKU   exit 0: outputs are current
    prepare_stamp.py write GAME --out OUT --sku SKU   after a successful chain
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

SCRIPTS = ('prepare.py', 'game_check.py', 'link_libc.py', 'link_modules.py', 'content_profile.py')
OUTPUTS = ('boot.bin', 'eboot.elf', 'entry.bin', 'analysis.json', 'boot-libc.bin', 'libc-link.json',
           'boot-linked.bin', 'link.json', 'content.bin', 'content-profile.json')


def key(game, sku):
    game = Path(game)
    inputs = [game / 'eboot.bin', game / 'sce_sys/param.sfo', *sorted((game / 'sce_module').iterdir())]
    here = Path(__file__).resolve().parent
    return dict(game=str(game.resolve()), sku=sku, skip_check=os.environ.get('BB_SKIP_GAME_CHECK', '0'),
                inputs={str(p): [p.stat().st_size, p.stat().st_mtime_ns] for p in inputs},
                scripts={s: hashlib.sha256((here / s).read_bytes()).hexdigest() for s in SCRIPTS})


def unchanged(game, out, sku):
    out = Path(out)
    if os.environ.get('BB_FORCE_PREPARE') == '1' or not all((out / o).is_file() for o in OUTPUTS):
        return False
    try:
        return json.loads((out / 'prepare-stamp.json').read_text(encoding='utf-8')) == key(game, sku)
    except (OSError, ValueError):
        return False


def write(game, out, sku):
    (Path(out) / 'prepare-stamp.json').write_text(json.dumps(key(game, sku), indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['check', 'write'])
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--sku', default='full')
    a = p.parse_args()
    if a.command == 'write':
        write(a.game, a.out, a.sku)
    else:
        sys.exit(0 if unchanged(a.game, a.out, a.sku) else 1)
