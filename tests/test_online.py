# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from online import configure_online, http_base, ONLINE_ENV, GAME_STATUS_ORIGIN


class OnlineProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.settings = dict(online_enabled='1', online_connected='1',
            online_host='server.example', online_port='31413',
            online_webapi='http://server.example:31415', online_username='Hunter',
            online_upnp='0')
        self.env = {'BB_SHADNET_PASSWORD': 'synthetic-test-password', 'UNRELATED': 'keep'}

    def test_offline_clears_inherited_online_credentials_and_override(self):
        for settings in ({}, {'online_enabled':'0'}, {'online_enabled':'1','online_connected':'0'}):
            env = dict.fromkeys(ONLINE_ENV, 'inherited')
            env['UNRELATED'] = 'keep'
            self.assertIsNone(configure_online(settings, self.data, env))
            self.assertEqual(env, {'BB_ONLINE':'0','UNRELATED':'keep'})
            self.assertFalse((self.data/'network').exists())

    def test_custom_ports_preserved_and_only_game_origin_redirected(self):
        self.settings['online_game_api'] = 'https://game.example:31443'
        self.settings['online_p2p_port'] = '32000'
        path = configure_online(self.settings, self.data, self.env)
        self.assertEqual(json.loads(path.read_text()), {GAME_STATUS_ORIGIN:'https://game.example:31443'})
        self.assertEqual(self.env['BB_SHADNET_SERVER'], 'server.example:31413')
        self.assertEqual(self.env['BB_SHADNET_WEBAPI'], 'http://server.example:31415')
        self.assertEqual(self.env['SHADPS4_P2P_PORT'], '32000')
        self.assertEqual(self.env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(path.resolve()))
        self.assertEqual(self.env['UNRELATED'], 'keep')
        self.assertNotIn('synthetic-test-password', path.read_text())
        self.assertFalse(path.with_suffix('.json.tmp').exists())

    def test_same_api_fallback_and_automatic_port(self):
        self.env['SHADPS4_P2P_PORT'] = 'inherited'
        path = configure_online(self.settings, self.data, self.env)
        self.assertEqual(json.loads(path.read_text()), {GAME_STATUS_ORIGIN:self.settings['online_webapi']})
        self.assertNotIn('SHADPS4_P2P_PORT', self.env)

    def test_invalid_profile_fails_before_writing_cache(self):
        for key, values in {
            'online_host':['', 'http://server.example', 'foo bar', '::1'],
            'online_port':['0','65536','not-a-port'],
            'online_webapi':['','ftp://server.example','http://user:pass@server.example','http://server.example:0','http://server.example/api'],
            'online_username':['','has space','x'*17,'é'*9],
            'online_p2p_port':['0','65536','bad']}.items():
            for value in values:
                with self.subTest(key=key,value=value):
                    settings = dict(self.settings, **{key:value})
                    with self.assertRaises(ValueError): configure_online(settings,self.data,dict(self.env))
                    self.assertFalse((self.data/'network').exists())

    def test_no_password_no_cache(self):
        with self.assertRaises(ValueError): configure_online(self.settings,self.data,{})
        self.assertFalse((self.data/'network').exists())

    def test_url_does_not_drop_port(self):
        self.assertEqual(http_base('https://server.example:34443/api/'),'https://server.example:34443/api')
        for url in ('http://server.example?x=1','http://server.example#x','http://server.example\n'):
            with self.assertRaises(ValueError): http_base(url)


if __name__ == '__main__': unittest.main()
