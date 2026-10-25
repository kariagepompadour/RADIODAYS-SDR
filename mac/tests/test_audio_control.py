"""The Cardputer's volume / mute / output operations against the PC audio path (no speakers, no KiwiSDR).

A recording sink stands in for the sound card; a small client sends the same requests, in the same
order, as the Cardputer firmware does at first contact and afterwards.  The same checks run on every
OS; SoundcardSink is also exercised with a fake `soundcard` module that behaves like the Windows
(WASAPI) backend: it fails on close and expects 2-D float32 data.
"""
import json
import os
import socket
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.request

import numpy as np

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import platform_support  # noqa: E402
import radiodays_sdr as r  # noqa: E402

TOKEN = 'T0KEN'


class RecordingSink:
    name = 'recording'

    def __init__(self):
        self.lock = threading.Lock()
        self.opens = self.closes = 0
        self.rms = []                       # level of each block that reached the "speaker"

    def open(self, rate):
        self.opens += 1

    def play(self, samples):
        with self.lock:
            self.rms.append(float(np.sqrt(np.mean(samples * samples))))

    def close(self):
        self.closes += 1

    def mark(self):
        with self.lock:
            return len(self.rms)

    def since(self, m):
        with self.lock:
            return list(self.rms[m:])


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Rig:
    def __init__(self, output='mac', volume=70):
        self._old_state = r.STATE_FILE
        self.tmp = tempfile.mkdtemp(prefix='rd_audio_')
        r.STATE_FILE = os.path.join(self.tmp, 'state.json')      # never touch the user's saved settings
        servers = [{'name': 'x', 'short': 'x', 'host': '127.0.0.1', 'port': 9, 'password': '', 'lat': None, 'lon': None}]
        self.sink = RecordingSink()
        self.engine = r.Engine(servers, self.sink, volume=volume, fake_source=True)
        self.engine.output = output
        self.engine.start()
        self.port = free_port()
        self.httpd = platform_support.LocalHTTPServer(('127.0.0.1', self.port), r.make_handler(self.engine, TOKEN))
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def get(self, path):
        sep = '&' if '?' in path else '?'
        with urllib.request.urlopen('http://127.0.0.1:%d%s%stoken=%s' % (self.port, path, sep, TOKEN), timeout=3) as resp:
            return json.loads(resp.read())

    def level(self, seconds=0.8):
        """Mean level reaching the speaker over a short window (waits for blocks queued before the change to drain)."""
        time.sleep(0.5)
        m = self.sink.mark()
        time.sleep(seconds)
        v = self.sink.since(m)
        return (sum(v) / len(v)) if v else None

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.engine.stop()
        r.STATE_FILE = self._old_state


class CardputerControlsPcAudio(unittest.TestCase):
    def setUp(self):
        self.rig = Rig()

    def tearDown(self):
        self.rig.close()

    def test_first_contact_order_then_volume_mute(self):
        g = self.rig.get
        g('/output?value=mac'); g('/volume?value=70'); g('/mute?value=0')       # what the firmware pushes at first contact
        full = self.rig.level()
        self.assertIsNotNone(full)
        self.assertAlmostEqual(full, 0.5 / (2 ** 0.5) * 0.49, delta=0.03)       # tone 0.5 peak, gain (70/100)^2
        g('/volume?value=100')
        self.assertGreater(self.rig.level(), full * 1.8)
        g('/volume?value=30')
        self.assertLess(self.rig.level(), full * 0.4)
        g('/mute?value=1')
        self.assertLess(self.rig.level(), 1e-6)
        g('/mute?value=0')
        self.assertGreater(self.rig.level(), 0.01)

    def test_volume_after_mute_unmutes_like_the_cardputer(self):
        g = self.rig.get
        g('/mute?value=1')
        self.assertLess(self.rig.level(), 1e-6)
        g('/volume?value=50')
        self.assertFalse(self.rig.get('/state')['muted'])
        self.assertGreater(self.rig.level(), 0.01)

    def test_mute_and_volume_survive_frequency_and_mode_changes(self):
        g = self.rig.get
        g('/volume?value=40'); g('/mute?value=1')
        g('/tune?khz=9650.0')
        g('/mode?value=usb')
        g('/mode?value=am')
        s = g('/state')
        self.assertTrue(s['muted']); self.assertEqual(s['volume'], 40)
        self.assertLess(self.rig.level(), 1e-6)
        g('/mute?value=0')
        self.assertAlmostEqual(self.rig.level(), 0.5 / (2 ** 0.5) * 0.16, delta=0.03)

    def test_output_switch_stops_and_resumes_the_pc_speaker(self):
        g = self.rig.get
        self.assertGreater(self.rig.level(), 0.01)
        g('/output?value=cardputer')
        time.sleep(0.6)
        closed = self.rig.sink.closes
        self.assertGreaterEqual(closed, 1)
        self.assertIsNone(self.rig.level(), 'PC speaker must be silent while the Cardputer speaker is chosen')
        g('/volume?value=20'); g('/mute?value=1'); g('/mute?value=0')           # controls keep working, still silent on the PC
        self.assertIsNone(self.rig.level())
        g('/output?value=mac')
        self.assertGreater(self.rig.level(), 0.001)
        self.assertEqual(self.rig.get('/state')['volume'], 20)

    def test_start_with_cardputer_output_is_silent_on_the_pc(self):
        rig = Rig(output='cardputer')
        try:
            self.assertIsNone(rig.level())
            self.assertEqual(rig.sink.opens, 0)
        finally:
            rig.close()

    def test_settings_changes_are_logged(self):
        with self.assertLogs('radiodays', level='INFO') as cm:
            self.rig.get('/volume?value=33'); self.rig.get('/mute?value=1'); self.rig.get('/mute?value=0')
        text = '\n'.join(cm.output)
        self.assertIn('volume: 33', text); self.assertIn('mute: on', text); self.assertIn('mute: off', text)


def fake_soundcard(log):
    """A stand-in for soundcard's Windows backend: closing raises (check_error), data must be 2-D float32."""
    class Player:
        def __enter__(self):
            log.append('enter'); return self

        def play(self, data):
            data = np.array(data, dtype='float32', order='C')
            if data.ndim == 1:
                data = data[:, None]
            log.append(('play', float(np.sqrt(np.mean(data * data))), data.dtype))

        def __exit__(self, *a):
            log.append('exit')
            raise OSError('WASAPI Stop failed')

    class Speaker:
        name = 'Fake Speakers (WASAPI)'

        def player(self, samplerate, blocksize):
            log.append(('player', samplerate, blocksize)); return Player()

    mod = types.ModuleType('soundcard')
    mod.all_speakers = lambda: [Speaker()]
    mod.default_speaker = lambda: Speaker()
    return mod


class SoundcardSinkWithWindowsLikeBackend(unittest.TestCase):
    def test_gain_reaches_the_backend_and_close_failure_is_harmless(self):
        log = []
        old = sys.modules.get('soundcard')
        sys.modules['soundcard'] = fake_soundcard(log)
        try:
            sink = r.SoundcardSink()
            servers = [{'name': 'x', 'short': 'x', 'host': '127.0.0.1', 'port': 9, 'password': '', 'lat': None, 'lon': None}]
            old_state = r.STATE_FILE
            r.STATE_FILE = os.path.join(tempfile.mkdtemp(), 's.json')
            e = r.Engine(servers, sink, volume=70, fake_source=True)
            e.start()
            try:
                time.sleep(0.8)
                plays = [x for x in log if isinstance(x, tuple) and x[0] == 'play']
                self.assertTrue(plays)
                self.assertTrue(all(p[2] == np.float32 for p in plays))
                e.set_muted(True); time.sleep(0.6)
                m = len(log); time.sleep(0.6)
                self.assertTrue(all(x[1] < 1e-6 for x in log[m:] if isinstance(x, tuple) and x[0] == 'play'))
                e.set_output('cardputer'); time.sleep(0.6)
                self.assertIn('exit', log)                                       # the device was released
                m = len(log); time.sleep(0.6)
                self.assertEqual([x for x in log[m:] if isinstance(x, tuple) and x[0] == 'play'], [])
                self.assertEqual(e.audio_error, '')
            finally:
                e.stop(); r.STATE_FILE = old_state
        finally:
            if old is None:
                sys.modules.pop('soundcard', None)
            else:
                sys.modules['soundcard'] = old


if __name__ == '__main__':
    unittest.main()
