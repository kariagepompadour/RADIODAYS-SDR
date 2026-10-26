"""Automated tests for the Mac server.  No KiwiSDR or speakers needed.

  .venv/bin/python -m unittest discover -s tests -v          (from the mac/ folder)

HTTP tests use the built-in test tone (--no-kiwi) and no audio output.
The loopback test (volume really changes the Mac output level) needs the BlackHole 2ch
virtual device; it is skipped when that device is missing.
"""
import json
import os
import subprocess
import sys
import time
import unittest
import urllib.request

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOKEN = 'TEST_TOKEN'


def get(port, path, token=TOKEN):
    sep = '&' if '?' in path else '?'
    try:
        with urllib.request.urlopen('http://127.0.0.1:%d%s%stoken=%s' % (port, path, sep, token), timeout=3) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


class Server:
    def __init__(self, port, *extra):
        self.port = port
        self.p = subprocess.Popen([sys.executable, '-I', os.path.join(MAC, 'radiodays_sdr.py'), '--no-kiwi',
                                   '--token', TOKEN, '--port', str(port), '--discovery-port', str(port + 1000), '--audio-port', str(port + 2000), '--volume', '70', '--freq', '10000',
                                   '--servers', os.path.join('/tmp/radiodays_test_home', 'servers_%d.json' % port), '--manager-port', '0'] + list(extra),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  env=dict(os.environ, HOME='/tmp/radiodays_test_home'))
        for _ in range(60):
            try:
                get(port, '/state')
                return
            except Exception:
                time.sleep(0.2)
        raise RuntimeError('server did not start')

    def close(self):
        self.p.terminate()
        self.p.wait(5)


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.makedirs('/tmp/radiodays_test_home', exist_ok=True)
        cls.srv = Server(18765, '--no-audio')

    @classmethod
    def tearDownClass(cls):
        cls.srv.close()

    def test_auth(self):
        self.assertEqual(get(18765, '/state', token='bad')[0], 403)

    def test_state_has_fields_used_by_firmware(self):
        c, s = get(18765, '/state')
        self.assertEqual(c, 200)
        for k in ('freq', 'mode', 'volume', 'muted', 'link', 'server', 'rssi', 's', 'in_level', 'out_level', 'unix_time'):
            self.assertIn(k, s)

    def test_tune_and_step(self):
        self.assertEqual(get(18765, '/tune?khz=9580')[1]['freq'], 9580.0)
        self.assertEqual(get(18765, '/step?delta=3&khz=5')[1]['freq'], 9595.0)
        self.assertEqual(get(18765, '/tune?khz=99999')[0], 400)
        self.assertEqual(get(18765, '/tune?khz=abc')[0], 400)

    def test_mode(self):
        self.assertEqual(get(18765, '/mode?value=usb')[1]['mode'], 'usb')
        self.assertEqual(get(18765, '/mode?value=xx')[0], 400)
        get(18765, '/mode?value=am')

    def test_volume_mute(self):
        self.assertEqual(get(18765, '/volume?value=35')[1]['volume'], 35)
        self.assertEqual(get(18765, '/volume?value=101')[0], 400)
        self.assertTrue(get(18765, '/mute?value=1')[1]['muted'])
        self.assertFalse(get(18765, '/mute?value=toggle')[1]['muted'])
        get(18765, '/mute?value=1')
        self.assertFalse(get(18765, '/volume?value=40')[1]['muted'])   # changing volume unmutes
        get(18765, '/volume?value=70')

    def test_volume_is_never_blocked(self):
        """The old bridge held one lock while waiting for rigctl; volume must answer fast."""
        t = time.time()
        for v in (10, 20, 30, 40, 50):
            get(18765, '/volume?value=%d' % v)
        self.assertLess(time.time() - t, 1.0)
        get(18765, '/volume?value=70')

    def test_udp_discovery_like_firmware(self):
        import socket
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.settimeout(2)
        u.sendto(('RADIO_DAYS_DISCOVER ' + TOKEN).encode(), ('127.0.0.1', 19765))
        data, _ = u.recvfrom(100)
        self.assertEqual(data, ('RADIO_DAYS_BRIDGE ' + TOKEN).encode())
        u.sendto(b'RADIO_DAYS_DISCOVER wrong', ('127.0.0.1', 19765))
        u.settimeout(0.5)
        with self.assertRaises(socket.timeout):
            u.recvfrom(100)

    def test_output_api(self):
        c, st = get(18765, '/state')
        self.assertEqual(st['output'], 'mac')                      # default is the Mac speaker
        for k in ('audio_client', 'audio_sent', 'audio_dropped', 'audio_disconnects'):
            self.assertIn(k, st)
        self.assertEqual(get(18765, '/output?value=cardputer')[1]['output'], 'cardputer')
        self.assertEqual(get(18765, '/output?value=bluetooth')[0], 400)
        self.assertEqual(get(18765, '/state')[1]['output'], 'cardputer')
        self.assertEqual(get(18765, '/volume?value=33')[1]['volume'], 33)   # volume/mute still work in either mode
        self.assertTrue(get(18765, '/mute?value=1')[1]['muted'])
        get(18765, '/mute?value=0')
        self.assertEqual(get(18765, '/output?value=mac')[1]['output'], 'mac')
        get(18765, '/volume?value=70')

    def test_scan_api(self):
        st = get(18765, '/state')[1]
        for k in ('scan', 'scan_msg', 'scan_excess', 'scan_floor'):
            self.assertIn(k, st)
        self.assertEqual(st['scan'], 'off')
        self.assertEqual(get(18765, '/scan?dir=2')[0], 400)
        self.assertEqual(get(18765, '/scan?dir=1&step=7')[0], 400)
        c, st = get(18765, '/scan?dir=1&step=1')                     # STEP 1 kHz selected: still accepted (the scan uses 5 kHz)
        self.assertEqual((c, st['scan'], st['scan_msg']), (200, 'up', 'scanning'))
        self.assertEqual(get(18765, '/scan?dir=-1&step=5')[1]['scan'], 'down')
        self.assertEqual(get(18765, '/scan?dir=0')[1]['scan'], 'off')
        get(18765, '/scan?dir=1&step=5')
        self.assertEqual(get(18765, '/tune?khz=9000')[1]['scan'], 'off')   # typing a frequency stops a scan
        get(18765, '/scan?dir=1&step=5')
        self.assertEqual(get(18765, '/mode?value=usb')[1]['scan'], 'off')  # so does a mode change
        get(18765, '/mode?value=am')
        get(18765, '/scan?dir=1&step=5')
        self.assertEqual(get(18765, '/server?index=0')[1]['scan'], 'off')  # and a receiver change

    def test_servers(self):
        c, s = get(18765, '/servers')
        self.assertEqual(c, 200)
        self.assertGreaterEqual(len(s['servers']), 4)
        for e in s['servers']:
            self.assertIn('lat', e)
            self.assertIn('lon', e)
            self.assertTrue(-90 <= e['lat'] <= 90 and -180 <= e['lon'] <= 180)
        self.assertEqual(s['servers'][2]['lat'], s['servers'][3]['lat'])   # Ironstone twins share a site

    def test_server_switch_is_visible_at_once(self):
        c, s = get(18765, '/server?index=3')
        self.assertEqual((c, s['server_index'], s['link']), (200, 3, 'connecting'))
        self.assertEqual(get(18765, '/server?index=99')[0], 400)
        get(18765, '/server?index=0')


class EngineLinkTests(unittest.TestCase):
    """Connection state and server switching with servers that are down or hang (no internet needed)."""

    def _engine(self, servers):
        sys.path.insert(0, MAC)
        import radiodays_sdr as r
        e = r.Engine(servers, r.NullSink())
        e.connect_timeout = 4
        e.start()
        self.addCleanup(e.stop)
        return e

    def _wait(self, cond, timeout):
        t = time.time()
        while time.time() - t < timeout:
            if cond():
                return True
            time.sleep(0.05)
        return False

    def test_failure_is_reported_and_auto_failover_moves_the_index(self):
        dead = lambda n: {'name': n, 'short': n, 'host': '127.0.0.1', 'port': 9, 'password': '', 'lat': None, 'lon': None}
        e = self._engine([dead('A'), dead('B')])
        self.assertTrue(self._wait(lambda: e.state()['link'] == 'error' and e.state()['error'], 6))
        self.assertTrue(self._wait(lambda: e.state()['server_index'] == 1, 12), 'did not fail over to the next server')

    def test_switch_interrupts_a_hanging_server(self):
        import socket
        import threading
        ls = socket.socket()
        ls.bind(('127.0.0.1', 0))
        ls.listen(5)
        held = []
        threading.Thread(target=lambda: [held.append(ls.accept()[0]) for _ in range(3)], daemon=True).start()
        self.addCleanup(ls.close)
        hang = {'name': 'H', 'short': 'H', 'host': '127.0.0.1', 'port': ls.getsockname()[1], 'password': '', 'lat': None, 'lon': None}
        dead = {'name': 'D', 'short': 'D', 'host': '127.0.0.1', 'port': 9, 'password': '', 'lat': None, 'lon': None}
        e = self._engine([hang, dead])
        time.sleep(1.0)                                   # now blocked waiting for the hanging server
        t = time.time()
        e.switch_server(1)
        self.assertEqual(e.state()['server_index'], 1)    # visible immediately
        self.assertTrue(self._wait(lambda: e.state()['link'] == 'error' and 'Refused' in e.state()['error'], 3),
                        'switch did not interrupt the blocked connection')
        self.assertLess(time.time() - t, 3.0)


def _blackhole():
    try:
        import soundcard as sc
        return [m for m in sc.all_microphones(include_loopback=False) if 'BlackHole' in m.name]
    except Exception:
        return []


@unittest.skipUnless(_blackhole(), 'BlackHole 2ch not installed')
class LoopbackTest(unittest.TestCase):
    """Server plays a 600 Hz tone to BlackHole; we record BlackHole and compare levels."""

    def test_volume_changes_real_output(self):
        import numpy as np
        import soundcard as sc
        os.makedirs('/tmp/radiodays_test_home', exist_ok=True)
        srv = Server(18766, '--sound-device', 'BlackHole 2ch')
        try:
            mic = _blackhole()[0]
            rms = {}
            with mic.recorder(samplerate=48000, channels=2, blocksize=512) as rec:
                def measure(label, path):
                    get(18766, path)
                    rec.record(numframes=48000 // 2)          # flush 0.5 s of old audio
                    x = rec.record(numframes=48000)           # measure 1 s
                    rms[label] = float(np.sqrt(np.mean(x ** 2)))
                measure('v100', '/volume?value=100')
                measure('v70', '/volume?value=70')
                measure('v40', '/volume?value=40')
                measure('v10', '/volume?value=10')
                measure('mute', '/mute?value=1')
                measure('unmute', '/mute?value=0')
            print('\nmeasured RMS at BlackHole:', {k: round(v, 4) for k, v in rms.items()})
            self.assertGreater(rms['v100'], 0.05, 'no audio reached the output device')
            self.assertGreater(rms['v100'], rms['v70'] * 1.3)
            self.assertGreater(rms['v70'], rms['v40'] * 1.3)
            self.assertGreater(rms['v40'], rms['v10'] * 1.5)
            self.assertLess(rms['mute'], rms['v100'] * 0.02, 'mute did not silence the output')
            self.assertAlmostEqual(rms['unmute'], rms['v10'], delta=rms['v10'] * 0.5 + 0.002)
            # volume gain is (v/100)^2: v70 should be ~0.49 of v100
            self.assertAlmostEqual(rms['v70'] / rms['v100'], 0.49, delta=0.12)
        finally:
            srv.close()


if __name__ == '__main__':
    unittest.main()
