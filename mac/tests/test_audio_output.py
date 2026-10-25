"""Tests of the audio output switch and the Cardputer audio stream (no KiwiSDR, no speakers).

The fake source plays a 600 Hz tone at 12 kHz with amplitude 0.5.  A test client speaks the same
protocol as the Cardputer firmware.
"""
import os
import socket
import struct
import sys
import threading
import time
import unittest

import numpy as np

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import radiodays_sdr as r  # noqa: E402

TOKEN = 'T0KEN'
HDR = struct.Struct('<2sHIH')


class CountingSink:
    name = 'counting'

    def __init__(self):
        self.opens = self.plays = 0

    def open(self, rate):
        self.opens += 1

    def play(self, samples):
        self.plays += 1

    def close(self):
        pass


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def make(output='mac'):
    servers = [{'name': 'x', 'short': 'x', 'host': '127.0.0.1', 'port': 9, 'password': '', 'lat': None, 'lon': None}]
    sink = CountingSink()
    e = r.Engine(servers, sink, volume=70, fake_source=True)
    e.output = output
    e.audio = r.AudioServer(e, TOKEN, free_port())
    e.audio.start()
    e.start()
    return e, sink


def connect(e, token=TOKEN, rcvbuf=None):
    s = socket.socket()
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(3)
    s.connect(('127.0.0.1', e.audio.port))
    s.sendall(('RDSDR-AUDIO 1 %s\n' % token).encode())
    return s


def readline(s):
    line = b''
    while not line.endswith(b'\n'):
        ch = s.recv(1)
        if not ch:
            return line
        line += ch
    return line


def read_exact(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b


def read_frames(s, n):
    out = []
    for _ in range(n):
        magic, ns, seq, rate = HDR.unpack(read_exact(s, HDR.size))
        assert magic == b'RD'
        out.append((seq, rate, np.frombuffer(read_exact(s, ns * 2), '<i2')))
    return out


class AudioOutputTests(unittest.TestCase):
    def setUp(self):
        self.engines = []

    def tearDown(self):
        for e in self.engines:
            e.stop()

    def mk(self, output='mac'):
        e, sink = make(output)
        self.engines.append(e)
        return e, sink

    def test_default_is_mac_and_mac_plays_locally(self):
        e, sink = self.mk('mac')
        time.sleep(1.0)
        self.assertEqual(e.state()['output'], 'mac')
        self.assertGreater(sink.opens, 0)
        self.assertGreater(sink.plays, 5)

    def test_bad_token_and_busy(self):
        e, _ = self.mk('cardputer')
        s = connect(e, token='WRONG')
        self.assertEqual(s.recv(10), b'')                       # closed without a word
        e.set_output('mac')
        s = connect(e)
        self.assertTrue(readline(s).startswith(b'BUSY'))        # output is mac: refused

    def test_stream_is_continuous_unscaled_and_a_clean_tone(self):
        e, sink = self.mk('cardputer')
        s = connect(e)
        self.assertEqual(readline(s), b'OK\n')
        e.set_volume(5)                                          # volume must NOT change what is sent
        e.set_muted(True)                                        # ... nor mute: the Cardputer applies both
        frames = read_frames(s, 90)
        seqs = [f[0] for f in frames]
        self.assertEqual(seqs, list(range(seqs[0], seqs[0] + 90)), 'frames lost or reordered')
        self.assertTrue(all(f[1] == 12000 for f in frames))
        pcm = np.concatenate([f[2] for f in frames]).astype(np.float32) / 32768.0
        self.assertAlmostEqual(float(np.max(np.abs(pcm))), 0.5, delta=0.01)   # full-scale of the source, not x(5/100)^2
        spec = np.abs(np.fft.rfft(pcm * np.hanning(len(pcm))))
        peak_hz = np.argmax(spec) * 12000 / len(pcm)
        self.assertAlmostEqual(peak_hz, 600, delta=8)
        self.assertEqual(sink.opens, 0, 'the Mac speaker must stay unused in cardputer mode')
        self.assertEqual(sink.plays, 0)
        st = e.state()
        self.assertTrue(st['audio_client'])
        self.assertEqual(st['output'], 'cardputer')

    def test_audio_socket_keeps_a_brief_wifi_stall_from_becoming_a_disconnect(self):
        e, _ = self.mk('cardputer')
        s = connect(e)
        self.assertEqual(readline(s), b'OK\n')
        server_sock = e.audio.client[0]
        self.assertTrue(server_sock.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE))
        self.assertEqual(server_sock.gettimeout(), 8.0)

    def test_real_time_pace(self):
        e, _ = self.mk('cardputer')
        s = connect(e)
        readline(s)
        read_frames(s, 5)
        t = time.time()
        n = 70
        read_frames(s, n)
        rate = n / (time.time() - t)                             # 12000/512 = 23.4 frames/s
        self.assertAlmostEqual(rate, 23.4, delta=2.5)

    def test_switch_to_mac_closes_the_stream_and_mac_plays_again(self):
        e, sink = self.mk('cardputer')
        s = connect(e)
        readline(s)
        read_frames(s, 3)
        e.set_output('mac')
        end = time.time() + 3
        closed = False
        while time.time() < end:
            try:
                if not s.recv(4096):
                    closed = True
                    break
            except socket.timeout:
                break
            except OSError:
                closed = True
                break
        self.assertTrue(closed)
        time.sleep(1.0)
        self.assertGreater(sink.plays, 5)
        self.assertFalse(e.state()['audio_client'])

    def test_newest_client_wins(self):
        e, _ = self.mk('cardputer')
        a = connect(e); readline(a); read_frames(a, 2)
        b = connect(e); self.assertEqual(readline(b), b'OK\n'); read_frames(b, 2)
        got_eof = False
        try:
            for _ in range(200):
                if not a.recv(65536):
                    got_eof = True
                    break
        except (socket.timeout, OSError):
            pass
        self.assertTrue(got_eof, 'the first client should have been closed')

    def test_stalled_client_never_blocks_the_kiwi_path(self):
        """A client that stops reading (real socket): the receive path keeps its pace for the whole stall.
        (macOS loopback buffers ~350 frames, so drops are checked with a blocking fake socket below.)"""
        e, _ = self.mk('cardputer')
        s = connect(e, rcvbuf=2048)
        readline(s)                                              # ... and then never read again
        b0 = e.state()['blocks']
        t0 = time.time()
        time.sleep(5)
        rate = (e.state()['blocks'] - b0) / (time.time() - t0)
        self.assertGreater(rate, 20, 'the receive path slowed down: %.1f blocks/s' % rate)

    def test_blocked_sender_drops_oldest_counts_them_and_offer_never_waits(self):
        e, _ = self.mk('mac')                                    # (no live source traffic into the server)
        srv = e.audio
        gate = threading.Event()
        sent = []

        class Stuck:
            def sendall(self, data):
                gate.wait(10)
                sent.append(data)
                raise OSError('connection reset')                # when it finally fails
            def close(self): pass
            def shutdown(self, how): pass

        q = __import__('queue').Queue(maxsize=srv.QMAX)
        srv.client = (Stuck(), q)
        th = threading.Thread(target=srv._send_loop, args=(srv.client[0], q), daemon=True)
        th.start()
        tone = np.zeros(512, np.float32)
        worst = 0.0
        for i in range(150):
            t = time.perf_counter()
            srv.offer(tone + i * 1e-3, 12000)
            worst = max(worst, time.perf_counter() - t)
        self.assertLess(worst, 0.05, 'offer() blocked for %.3f s' % worst)
        self.assertGreaterEqual(srv.dropped, 150 - srv.QMAX - 2)
        self.assertLessEqual(q.qsize(), srv.QMAX)
        # the frames that remain are the NEWEST ones
        frames = []
        while True:
            try:
                frames.append(q.get_nowait())
            except Exception:
                break
        last_seq = HDR.unpack(frames[-1][:HDR.size])[2]
        first_seq = HDR.unpack(frames[0][:HDR.size])[2]
        self.assertEqual(last_seq, srv.seq)
        self.assertEqual(last_seq - first_seq, len(frames) - 1)
        gate.set()                                               # now the send fails: the client is dropped and counted
        th.join(3)
        self.assertFalse(srv.connected)
        self.assertEqual(srv.disconnects, 1)

    def test_a_dead_client_is_replaced_by_a_new_one(self):
        e, _ = self.mk('cardputer')
        s = connect(e); readline(s); read_frames(s, 2)
        s.close()                                                # the Cardputer vanished (Wi-Fi lost)
        t0 = time.time()
        while time.time() - t0 < 20 and e.audio.connected:
            time.sleep(0.2)
        s2 = connect(e)                                          # ... and comes back
        self.assertEqual(readline(s2), b'OK\n')
        frames = read_frames(s2, 5)
        self.assertEqual(len(frames), 5)

    def test_set_output_validation(self):
        e, _ = self.mk('mac')
        with self.assertRaises(ValueError):
            e.set_output('bluetooth')


if __name__ == '__main__':
    unittest.main()
