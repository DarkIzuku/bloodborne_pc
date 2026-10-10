# SPDX-License-Identifier: GPL-2.0-or-later
"""The single INI online profile and automatically generated game host override."""
import json
import os
from pathlib import Path
from urllib.parse import urlsplit

GAME_STATUS_ORIGIN = 'https://ss4.scej-network.jp:20443'
ONLINE_ENV = ('BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID',
              'BB_SHADNET_PASSWORD', 'BB_SHADNET_TOKEN', 'BB_UPNP', 'SHADPS4_P2P_PORT', 'SHADPS4_HTTP_HOST_OVERRIDES_JSON')

def read_settings(path):
    settings = {}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        if sep:
            settings[key.strip()] = value.strip()
    return settings

def http_base(text):
    if not text or any(ord(c) < 32 for c in text):
        raise ValueError('Online requires an explicit WebAPI / game API URL')
    value = urlsplit(text)
    if value.scheme not in ('http', 'https') or not value.hostname or value.username is not None or value.password is not None or value.query or value.fragment:
        raise ValueError('Online API must be an HTTP(S) URL without credentials, query or fragment')
    if value.port is not None and not 1 <= value.port <= 65535:
        raise ValueError('Invalid online API port')
    return text.rstrip('/')

def configure_online(settings, data, env):
    if settings.get('online_enabled') != '1' or settings.get('online_connected', '1') != '1':
        for key in ONLINE_ENV:
            env.pop(key, None)
        env['BB_ONLINE'] = '0'
        return None
    host = settings.get('online_host', '').strip().strip('[]')
    if not host or any(c.isspace() or ord(c) < 32 for c in host) or any(c in host for c in '/@?#:'):
        raise ValueError('Enter a shadNet server before enabling online')
    port = int(settings.get('online_port', ''))
    if not 1 <= port <= 65535:
        raise ValueError('shadNet TCP port must be between 1 and 65535')
    webapi = http_base(settings.get('online_webapi', ''))
    game_api = http_base(settings.get('online_game_api', '') or webapi)
    if urlsplit(webapi).path not in ('', '/') or urlsplit(game_api).path not in ('', '/'):
        raise ValueError('WebAPI and game API require a server origin without an extra path')
    username = settings.get('online_username', '')
    if not 1 <= len(username.encode('utf-8')) <= 16 or any(c.isspace() or ord(c) < 32 for c in username):
        raise ValueError('Enter the shadNet account name (up to 16 characters)')
    if not env.get('BB_SHADNET_PASSWORD'):
        raise ValueError('Sign in with the account password in the launcher')
    p2p = settings.get('online_p2p_port', '').strip()
    if p2p and not 1 <= int(p2p) <= 65535:
        raise ValueError('Local P2P port must be between 1 and 65535 or empty for automatic')
    # Runtime cache only. Never read or overwrite another application's JSON,
    # and never route private-server traffic through a public server by default.
    destination = Path(data) / 'network/host_overrides.json'
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.json.tmp')
    temporary.write_text(json.dumps({GAME_STATUS_ORIGIN: game_api}, indent=2), encoding='utf-8')
    os.replace(temporary, destination)
    env.update(BB_ONLINE='1', BB_SHADNET_SERVER=(f'[{host}]' if ':' in host else host) + f':{port}',
               BB_SHADNET_WEBAPI=webapi, BB_SHADNET_NPID=username,
               BB_UPNP='1' if settings.get('online_upnp') == '1' else '0',
               SHADPS4_HTTP_HOST_OVERRIDES_JSON=str(destination.resolve()))
    env.pop('SHADPS4_P2P_PORT', None)
    if p2p:
        env['SHADPS4_P2P_PORT'] = str(int(p2p))
    return destination
