"""Reject nonpublic modules, user content and incomplete portable Windows packages."""
from pathlib import Path
import re
import sys

root=Path(sys.argv[1]).resolve()
required=('BloodborneLauncher.exe','out/bb-probe.exe','out/bb-engine-assets.exe',
          'out/bb-gpu-capabilities.exe','out/nvngx_dlss.dll','tools/python/python.exe',
          'scripts/prepare_cache.py','scripts/redzone.py','scripts/render_pools.py',
          'patches/redzone.json','licenses/LICENSE-DLSS-SDK.txt')
for name in required:
    assert (root/name).is_file() and (root/name).stat().st_size>0, 'Missing package input: '+name
files=list(root.rglob('*'))
for path in files:
    if not path.is_file():continue
    name=path.relative_to(root).as_posix()
    assert not re.search(r'(seamless|steam_api|steam_appid|respawn_patch|\.dmp$|\.sl2$|eboot\.(bin|elf)|boot-linked\.bin|host_overrides\.json|bbport\.ini)',name,re.I), 'Unexpected package file: '+name
    if path.suffix.lower() in ('.py','.json','.md','.txt','.bat','.ps1','.sh'):
        assert not re.search(rb'(BB_SEAMLESS|bloodborne_pc_private|seamless_coop|respawn_delay_fix)',path.read_bytes(),re.I),name
assert len(list((root/'fsr4_shaders').rglob('*')))>180,'Missing FSR assets'
print('PASS: portable public package complete; no private modules, game content, saves or local configuration')
