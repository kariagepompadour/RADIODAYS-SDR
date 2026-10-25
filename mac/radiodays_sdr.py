#!/usr/bin/env python3
"""RADIO DAYS SDR - Mac side.

One process does everything:
  KiwiSDR (WebSocket)  ->  decode  ->  volume  ->  Mac audio output
  Cardputer ADV        ->  HTTP (port 8765) / UDP discovery (port 8766)

Unlike the previous version there is no rigctl hop and no helper process:
frequency, mode and volume are plain in-memory settings, so the HTTP API never
blocks on the KiwiSDR link.  The KiwiSDR link state is reported in /state
("link": connecting | connected | error) instead of being turned into HTTP 503.
"""
import argparse
import collections
import json
import logging
import math
import os
import queue
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler
from types import SimpleNamespace
from urllib.parse import parse_qs, urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# The KiwiSDR client library (jks-prv/kiwiclient) is not part of this repository: tools/fetch_kiwiclient.py downloads it
# into vendor/kiwiclient/ (the start file for your OS does that on first start).
sys.path.insert(0, os.path.join(HERE, 'vendor', 'kiwiclient'))

import numpy as np

import platform_support
import servers_store

HTTP_PORT = 8765
DISCOVERY_PORT = 8766
AUDIO_PORT = 8767
OUTPUTS = ('mac', 'cardputer')

# ---- AUTO scan (see Engine._run_scan).  Numbers are from measurements on real KiwiSDRs (2026-10-24):
# after a retune the new frequency's first valid RSSI block arrives 0.65-0.9 s later (4-5 stale blocks of 171 ms).
SCAN_SETTLE_S = 0.95        # wait this long after the Kiwi applied a retune before using a block
SCAN_MEASURE_BLOCKS = 2     # blocks per measurement; the lower one is used (an impulse hits one block)
SCAN_VERIFY_BLOCKS = 3      # fresh blocks that must all agree before stopping
SCAN_MIN_STEP = 5.0         # kHz: the broadcast raster; a smaller STEP would only make the scan 5x slower
SCAN_T_HI = 12.0            # dB above the noise floor: a candidate
SCAN_T_LO = 6.0             # dB: below this the receiver is "between stations" again (re-arms the detector)
SCAN_T_VER = 10.0           # dB: the MEDIAN of the verification blocks must be at least this far above the floor ...
SCAN_T_VER_MIN = 6.0        # ... and none of them may be below this (a fading station dips, a noise burst does not last)
SCAN_WARMUP = 3             # measurements before a floor estimate is trusted
SCAN_FLOOR_WINDOW = 20      # measurements the floor estimate looks back over (its 20th percentile)
MODES = ('am', 'usb', 'lsb')
FREQ_MIN, FREQ_MAX = 10.0, 30000.0       # kHz
STATE_FILE = os.path.join(os.path.expanduser('~'), '.radiodays_sdr_state.json')
log = logging.getLogger('radiodays')


# --------------------------------------------------------------------------- helpers
def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def rssi_to_s(rssi):
    """dBm -> S units (HF convention: S9 = -73 dBm, 6 dB per S unit). Returns (s, over_db)."""
    if rssi is None:
        return 0.0, 0
    s = (rssi + 127.0) / 6.0
    if s <= 9.0:
        return round(max(0.0, s), 1), 0
    return 9.0, int(round((rssi + 73.0) / 10.0) * 10)


def load_servers(path):
    """The receiver list from servers.json (validated, see servers_store.py)."""
    return servers_store.ServerStore(path).load()


# --------------------------------------------------------------------------- audio sinks
class SoundcardSink:
    """Plays mono float32 at the KiwiSDR sample rate on a Mac output device."""

    def __init__(self, device=None):
        import soundcard as sc          # imported late: --list-devices and tests need it only here
        self.sc = sc
        self.device = device
        self.player = None
        self.rate = None
        self.name = ''

    def open(self, rate):
        self.close()
        sc = self.sc
        speaker = None
        if self.device:
            for sp in sc.all_speakers():
                if self.device.lower() in sp.name.lower():
                    speaker = sp
                    break
            if speaker is None:
                raise RuntimeError('output device not found: %s' % self.device)
        else:
            speaker = sc.default_speaker()
        if speaker is None:
            raise RuntimeError('no output device')
        self.player = speaker.player(samplerate=rate, blocksize=512)
        self.player.__enter__()
        self.rate = rate
        self.name = speaker.name

    def play(self, samples):
        self.player.play(samples)

    def close(self):
        if self.player is not None:
            try:
                self.player.__exit__(None, None, None)
            except Exception:
                pass
        self.player = None
        self.rate = None


class NullSink:
    name = 'none'

    def open(self, rate):
        self.rate = rate

    def play(self, samples):
        pass

    def close(self):
        pass


# --------------------------------------------------------------------------- audio to the Cardputer
class AudioServer:
    """Streams the decoded KiwiSDR audio (unscaled 16-bit mono PCM) to ONE Cardputer.

    Protocol (TCP, default port 8767):
      client -> "RDSDR-AUDIO 1 <token>\n"
      server -> "OK\n"   (or "BUSY ...\n" / nothing and close on a wrong token)
      then frames:  b'RD' | u16 nsamples | u32 seq | u16 rate_hz | nsamples * int16 (all little endian)
    Volume and mute are applied by the Cardputer, so they act at once.  The Kiwi thread only ever calls
    offer(), which never blocks: each client has a bounded queue and the oldest frame is dropped when it is full.
    """
    HDR = struct.Struct('<2sHIH')
    QMAX = 40                                   # ~1.7 s of audio at 42.7 ms per frame
    MAX_FRAME = 1024                            # samples; the firmware accepts up to 1024

    def __init__(self, engine, token, port=AUDIO_PORT):
        self.engine, self.token, self.port = engine, token, port
        self.lock = threading.Lock()
        self.client = None                      # (sock, queue)
        self.seq = 0
        self.sent = self.dropped = self.disconnects = 0
        self.srv = None

    def start(self):
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        platform_support.prepare_listener(self.srv)
        self.srv.bind(('0.0.0.0', self.port))
        self.srv.listen(4)
        threading.Thread(target=self._accept_loop, daemon=True).start()

    @property
    def connected(self):
        return self.client is not None

    def stop(self):
        self.close_client()
        if self.srv:
            try:
                self.srv.close()
            except OSError:
                pass

    def _accept_loop(self):
        while True:
            try:
                conn, addr = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._handshake, args=(conn, addr), daemon=True).start()

    def _handshake(self, conn, addr):
        try:
            conn.settimeout(3.0)
            line = b''
            while not line.endswith(b'\n') and len(line) < 80:
                ch = conn.recv(1)
                if not ch:
                    raise OSError('closed')
                line += ch
            if line.strip() != ('RDSDR-AUDIO 1 ' + self.token).encode():
                raise OSError('bad handshake')
            if self.engine.output != 'cardputer':
                conn.sendall(b'BUSY output=mac\n')
                raise OSError('output is mac')
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32768)   # no big kernel buffer: latency stays bounded
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            # Detect a vanished Wi-Fi peer, but do not let a brief radio/UI stall make the PC
            # give up first. The Cardputer has its own 2.5 s no-data watchdog and normally
            # closes/reconnects before this sender timeout expires.
            for opt_name, value in (('TCP_KEEPIDLE', 15), ('TCP_KEEPALIVE', 15), ('TCP_KEEPINTVL', 5), ('TCP_KEEPCNT', 3)):
                opt = getattr(socket, opt_name, None)
                if opt is not None:
                    try:
                        conn.setsockopt(socket.IPPROTO_TCP, opt, value)
                    except OSError:
                        pass
            conn.sendall(b'OK\n')
        except OSError as e:
            log.debug('audio client %s refused: %s', addr, e)
            try:
                conn.close()
            except OSError:
                pass
            return
        conn.settimeout(8.0)                    # tolerate a brief stall; keepalive still detects a vanished peer
        q = queue.Queue(maxsize=self.QMAX)
        with self.lock:
            old, self.client = self.client, (conn, q)
        if old:                                 # exclusive: the newest client wins
            self._kill(old)
        log.info('Cardputer audio connected from %s', addr[0])
        self._send_loop(conn, q)

    def _send_loop(self, conn, q):
        try:
            while True:
                item = q.get()
                if item is None:
                    return
                conn.sendall(item)
                self.sent += 1
        except OSError as e:
            log.warning('Cardputer audio connection lost: %s', e)
            self.disconnects += 1
        finally:
            with self.lock:
                if self.client and self.client[0] is conn:
                    self.client = None
            try:
                conn.close()
            except OSError:
                pass

    def _kill(self, client):
        sock, q = client
        try:
            q.put_nowait(None)
        except queue.Full:
            pass
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

    def close_client(self):
        with self.lock:
            old, self.client = self.client, None
        if old:
            self._kill(old)

    def offer(self, samples, rate):
        """samples: float32 mono in [-1, 1].  Never blocks."""
        c = self.client
        if c is None:
            return
        pcm_all = np.clip(samples * 32767.0, -32768, 32767).astype('<i2')
        q = c[1]
        for i in range(0, len(pcm_all), self.MAX_FRAME):      # a Kiwi block is 2048 samples (171 ms): send 1024-sample frames, never more than the Cardputer accepts
            pcm = pcm_all[i:i + self.MAX_FRAME]
            self.seq = (self.seq + 1) & 0xFFFFFFFF
            frame = self.HDR.pack(b'RD', len(pcm), self.seq, int(rate)) + pcm.tobytes()
            while True:
                try:
                    q.put_nowait(frame)
                    break
                except queue.Full:
                    try:
                        q.get_nowait()
                        self.dropped += 1
                    except queue.Empty:
                        pass


# --------------------------------------------------------------------------- engine
class Engine:
    """Settings + KiwiSDR connection + audio playback.  All public methods are thread-safe."""

    def __init__(self, servers, sink, start_freq=10000.0, start_mode='am', volume=70,
                 server_index=0, connect_timeout=15.0, fake_source=False):
        self.servers = servers
        self.sink = sink
        self.lock = threading.RLock()
        self.stop_ev = threading.Event()
        self.fake_source = fake_source
        self.connect_timeout = connect_timeout
        # user settings
        self.freq = float(start_freq)
        self.mode = start_mode
        self.volume = int(volume)
        self.muted = False
        self.output = 'mac'               # mac | cardputer (chosen on the Cardputer, pushed to us)
        self.audio = None                 # AudioServer, set by main()
        self.server_index = server_index % len(servers)
        self.tune_ver = 1                 # bumped on every freq/mode change; stream applies it
        self.switch_req = None
        # measured values
        self.link = 'connecting'          # connecting | connected | error
        self.error = ''
        self.rssi = None
        self.blocks = 0
        self.in_level = 0.0               # RMS of the received audio (before volume)
        self.out_level = 0.0              # RMS of what is sent to the Mac output (after volume)
        self.played_blocks = 0
        self.audio_error = ''
        self.sample_rate = 0
        self.connected_at = None
        self.started = time.time()
        self.failures = {}
        self._q = queue.Queue(maxsize=14)  # ~0.6 s of audio; oldest dropped when full
        self._threads = []
        self._cur = None
        # AUTO scan state
        self.block_seq = 0                              # never reset (blocks above resets on a reconnect)
        self.block_rssi = collections.deque(maxlen=256)  # (seq, arrival time, rssi)
        self.applied_ver = 0                            # the last tune_ver the Kiwi thread has sent
        self.applied_t = 0.0                            # ... and when
        self.scan = 'off'                               # off | up | down
        self.scan_msg = ''                              # scanning | signal | end | stopped | lost | ''
        self.scan_excess = None
        self.scan_floor = None
        self.scan_time_scale = 1.0                      # tests run the scan faster with a simulated sky
        self.scan_pending = None
        self.scan_stop_ev = threading.Event()
        self.scan_go = threading.Event()
        self.scan_dir = 0

    # ----- settings --------------------------------------------------------
    def set_freq(self, khz, user=True):
        if user:
            self.stop_scan()
        with self.lock:
            self.freq = round(clamp(float(khz), FREQ_MIN, FREQ_MAX), 3)
            self.tune_ver += 1
            return self.freq

    def set_mode(self, mode):
        self.stop_scan()
        mode = mode.lower()
        if mode not in MODES:
            raise ValueError('invalid mode')
        with self.lock:
            self.mode = mode
            self.tune_ver += 1

    def set_volume(self, value):
        value = int(value)
        if not 0 <= value <= 100:
            raise ValueError('invalid volume')
        with self.lock:
            changed = value != self.volume
            self.volume = value
            if value > 0:
                self.muted = False
        if changed:
            log.info('volume: %d', value)      # visible proof that the Cardputer's operation arrived
        return value

    def set_muted(self, muted):
        with self.lock:
            changed = bool(muted) != self.muted
            self.muted = bool(muted)
        if changed:
            log.info('mute: %s', 'on' if muted else 'off')

    def set_output(self, value):
        value = str(value).lower()
        if value not in OUTPUTS:
            raise ValueError('invalid output')
        with self.lock:
            if value == self.output:
                return
            self.output = value
            self.out_level = 0.0
        if value == 'mac' and self.audio is not None:
            self.audio.close_client()     # the Cardputer stops its speaker; we play locally again
        log.info('audio output: %s', value)

    def reload_servers(self, new_servers):
        """Use a new receiver list (the Receiver Manager saved servers.json).  A receiver that is in use and still in the
        list keeps its connection; if it was removed, the first receiver is connected instead.  Returns a short report."""
        new = [dict(s) for s in new_servers]
        if not new:
            raise ValueError('empty receiver list')
        with self.lock:
            old = self.servers[self.server_index] if self.server_index < len(self.servers) else None
            key = (old['host'].lower(), old['port']) if old else None
            idx = next((i for i, s in enumerate(new) if (s['host'].lower(), s['port']) == key), None)
            self.servers = new
            if idx is not None:
                self.server_index = idx
                changed = False
            else:
                self.server_index = 0
                changed = True
        if changed:                                   # the receiver in use was taken off the list
            self.stop_scan()
            self.switch_server(0)
        log.info('receiver list reloaded: %d receivers%s', len(new), ', switched to the first one' if changed else '')
        return {'count': len(new), 'switched_to_first': changed, 'index': self.server_index}

    def switch_server(self, index):
        if not 0 <= index < len(self.servers):
            raise ValueError('invalid server index')
        self.stop_scan()
        with self.lock:
            self.switch_req = index
            self.server_index = index          # /state shows the requested server at once
            self.link = 'connecting'
            self.error = ''
            self.rssi = None
            self.blocks = 0
            rec = self._cur
        sock = getattr(rec, '_socket', None)   # wake a connect/handshake/read that is blocking on the old server
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def gain(self):
        with self.lock:
            if self.muted:
                return 0.0
            return (self.volume / 100.0) ** 2     # perceptual-ish curve; 100 = full scale

    def state(self):
        with self.lock:
            s, over = rssi_to_s(self.rssi)
            srv = self.servers[self.server_index]
            return {
                'freq': self.freq, 'mode': self.mode, 'volume': self.volume, 'muted': self.muted,
                'link': self.link, 'error': self.error,
                'server': srv['short'], 'server_name': srv['name'], 'server_index': self.server_index,
                'server_count': len(self.servers),
                'rssi': None if self.rssi is None else round(self.rssi, 1),
                's': s, 's_over_db': over,
                'in_level': round(self.in_level, 4), 'out_level': round(self.out_level, 4),
                'blocks': self.blocks, 'played_blocks': self.played_blocks,
                'audio_device': getattr(self.sink, 'name', ''), 'audio_error': self.audio_error,
                'sample_rate': self.sample_rate,
                'scan': self.scan, 'scan_msg': self.scan_msg,
                'scan_excess': None if self.scan_excess is None else round(self.scan_excess, 1),
                'scan_floor': None if self.scan_floor is None else round(self.scan_floor, 1),
                'output': self.output,
                'audio_client': bool(self.audio and self.audio.connected),
                'audio_sent': self.audio.sent if self.audio else 0,
                'audio_dropped': self.audio.dropped if self.audio else 0,
                'audio_disconnects': self.audio.disconnects if self.audio else 0,
                'uptime': int(time.time() - self.started),
            }

    # ----- audio path ------------------------------------------------------
    def push_audio(self, samples, rssi, rate):
        """Called from the KiwiSDR thread with float32 mono samples in [-1, 1]."""
        if self.switch_req is not None:       # a server change was requested: drop the old server's audio
            return
        with self.lock:
            self.blocks += 1
            self.block_seq += 1
            self.block_rssi.append((self.block_seq, time.time(), rssi))
            self.sample_rate = rate
            a = 0.1
            self.rssi = rssi if self.rssi is None else (1 - a) * self.rssi + a * rssi
            self.in_level = float(np.sqrt(np.mean(samples * samples))) if len(samples) else 0.0
            if self.link != 'connected':
                self.link = 'connected'
                self.error = ''
                self.connected_at = time.time()
        if self.output != 'mac':             # the Cardputer plays it: unscaled PCM, volume/mute are applied there
            if self.audio is not None:
                self.audio.offer(samples, rate)
            return
        item = (rate, samples)
        while True:
            try:
                self._q.put_nowait(item)
                return
            except queue.Full:
                try:
                    self._q.get_nowait()
                except queue.Empty:
                    pass

    def _playback_loop(self):
        open_rate = None
        while not self.stop_ev.is_set():
            if self.output != 'mac':          # the Mac speaker is not used: free the device, drop what is queued
                if open_rate is not None:
                    self.sink.close()
                    open_rate = None
                while not self._q.empty():
                    try:
                        self._q.get_nowait()
                    except queue.Empty:
                        break
                self.stop_ev.wait(0.1)
                continue
            try:
                rate, samples = self._q.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if open_rate != rate:
                    self.sink.open(rate)
                    open_rate = rate
                    with self.lock:
                        self.audio_error = ''
                out = np.clip(samples * self.gain(), -1.0, 1.0).astype(np.float32)
                rms = float(np.sqrt(np.mean(out * out))) if len(out) else 0.0
                self.sink.play(out)
                with self.lock:
                    self.out_level = rms
                    self.played_blocks += 1
            except Exception as e:                       # audio device trouble must not kill the link
                with self.lock:
                    self.audio_error = '%s: %s' % (type(e).__name__, e)
                log.error('audio output failed: %s', e)
                open_rate = None
                self.stop_ev.wait(1.0)

    # ----- KiwiSDR connection ---------------------------------------------
    def _make_options(self, srv):
        return SimpleNamespace(
            server_host=srv['host'], server_port=srv['port'], password=srv['password'], tlimit_password='',
            user='RADIO DAYS', modulation=self.mode, frequency=self.freq, lp_cut=None, hp_cut=None,
            ifreq=None, thresh=None, agc_gain=None, compression=True, nb=False, nb_test=False,
            de_emp=False, resample=0, quiet=True, S_meter=-1, sdt=0, stats=False, tstamp=False,
            tlimit=None, ADC_OV=False, sound=True, admin=False, no_api=False, nolocal=False,
            bad_cmd=False, camp_allow_1ch=False, netcat=False, rev_bin=False, wideband=False,
            is_kiwi_wav=False, is_kiwi_tdoa=False, test_mode=False, station=None, filename=None,
            dir=None, wf_cal=None, freq_pbc=False, idx=0, multiple_connections=0, status=0,
            socket_timeout=8, ws_timestamp=int(time.time() * 1000) & 0xffffffff,
            rigctl_enabled=False, squelch_tail=1)

    def _apply_pending(self, rec):
        with self.lock:
            ver, freq, mode = self.tune_ver, self.freq, self.mode
        if ver != rec.applied_ver:
            rec.applied_ver = ver
            rec.set_mod(mode, None, None, freq)
            self.applied_t = time.time()
            self.applied_ver = ver

    def _connect_once(self, srv):
        from kiwi.client import KiwiSDRStream
        engine = self

        class RadioStream(KiwiSDRStream):
            applied_ver = 0

            def _setup_rx_params(self_):
                self_.set_name('RADIO DAYS')
                with engine.lock:
                    ver, freq, mode = engine.tune_ver, engine.freq, engine.mode
                self_.applied_ver = ver
                self_._modulation = mode
                self_.set_mod(mode, None, None, freq)
                self_.set_agc(on=True)
                self_._output_sample_rate = int(self_._sample_rate)

            def _process_audio_samples(self_, seq, samples, rssi, fmt):
                f = samples.astype(np.float32) / 32768.0
                engine.push_audio(f, rssi, int(self_._sample_rate))

            def _process_iq_samples(self_, *a, **k):
                pass

        opts = self._make_options(srv)
        rec = RadioStream()
        rec._options = opts
        rec._type = 'SND'
        rec._freq = opts.frequency
        rec._lowcut = rec._highcut = 0
        rec._camp_wait_event = None
        rec._reader = True
        rec._start_time = None
        self._cur = rec
        rec.connect(srv['host'], srv['port'])
        try:
            rec.open()
            deadline = time.time() + self.connect_timeout
            while not self.stop_ev.is_set():
                with self.lock:
                    if self.switch_req is not None:
                        return
                    connected = self.link == 'connected'
                if not connected and time.time() > deadline:
                    raise TimeoutError('no audio from %s within %.0f s' % (srv['host'], self.connect_timeout))
                rec.run()
                if rec.applied_ver:
                    self._apply_pending(rec)
        finally:
            try:
                rec.close()
            except Exception:
                pass

    def _kiwi_loop(self):
        fails = 0
        while not self.stop_ev.is_set():
            with self.lock:
                if self.switch_req is not None:
                    self.server_index, self.switch_req = self.switch_req, None
                    fails = 0
                idx = self.server_index
                self.link = 'connecting'
                self.rssi = None
            srv = self.servers[idx]
            log.info('connecting to %s (%s:%d)', srv['name'], srv['host'], srv['port'])
            try:
                self._connect_once(srv)
                if self.switch_req is not None:
                    continue
                err = 'connection ended'
            except Exception as e:
                err = '%s: %s' % (type(e).__name__, e)
            if self.stop_ev.is_set():
                break
            if self.switch_req is not None:      # we were interrupted on purpose: not a failure
                continue
            fails += 1
            with self.lock:
                self.link = 'error'
                self.error = err[:120]
                self.blocks = 0
                nxt = idx
                if fails >= 2 and len(self.servers) > 1:      # fail over to the next server in the list
                    nxt = (idx + 1) % len(self.servers)
                    self.server_index = nxt
                    fails = 0
                    log.warning('%s failed (%s); trying %s', srv['short'], err, self.servers[nxt]['short'])
                else:
                    log.warning('%s failed (%s); retrying', srv['short'], err)
            self.stop_ev.wait(3.0)

    def _fake_loop(self):
        """--no-kiwi: a 600 Hz tone at 12 kHz so the audio/volume path can be tested offline."""
        rate, n, t = 12000, 512, 0
        nxt = time.time()
        while not self.stop_ev.is_set():
            x = 0.5 * np.sin(2 * np.pi * 600 * (np.arange(n) + t) / rate).astype(np.float32)
            t += n
            self.push_audio(x, -80.0, rate)
            nxt += n / rate
            self.stop_ev.wait(max(0.0, nxt - time.time()))

    # ----- AUTO scan ----------------------------------------------------------
    # The receiver is tuned step by step in one direction; at every step the RSSI that the KiwiSDR reports with its
    # audio blocks is measured once it has settled.  A signal stops the scan when it is clearly above the noise floor
    # (the 20th percentile of the recent measurements), is the local maximum (the peak is followed, then tuned back to),
    # and three fresh blocks agree.  A single noisy block never stops it.
    def start_scan(self, direction, step):
        direction = 1 if direction > 0 else -1
        with self.lock:
            self.scan_pending = (direction, max(float(step), SCAN_MIN_STEP))
            self.scan = 'up' if direction > 0 else 'down'
            self.scan_msg = 'scanning'
            self.scan_excess = None
        self.scan_stop_ev.set()                 # aborts a scan that is running (a different direction)
        self.scan_go.set()

    def stop_scan(self, msg='stopped'):
        with self.lock:
            had = self.scan != 'off'
            self.scan_pending = None
            if had:
                self.scan_msg = msg
                self.scan = 'off'
        self.scan_stop_ev.set()
        return had

    def _scan_finish(self, msg, excess=None):
        with self.lock:
            if self.scan_pending is None:       # (when a new scan is already waiting, it owns the state)
                self.scan = 'off'
                self.scan_msg = msg
                self.scan_excess = excess
        save_state(self)

    def _scan_aborted(self):
        return self.stop_ev.is_set() or self.scan_stop_ev.is_set()

    def _scan_collect(self, t_ready, n):
        """The next n blocks that arrived after t_ready (waits for them).  None if the scan is aborted or the link is lost."""
        got, seen = [], set()
        lost_since = None
        while len(got) < n:
            if self._scan_aborted():
                return None
            if self.link != 'connected':
                lost_since = lost_since or time.time()
                if time.time() - lost_since > 15 * self.scan_time_scale + 5:
                    return 'lost'
            else:
                lost_since = None
            with self.lock:
                new = [(q, rssi) for (q, t, rssi) in self.block_rssi if t >= t_ready and q not in seen]
            for q, rssi in new:
                seen.add(q)
                got.append(rssi)
            if len(got) < n:
                self.stop_ev.wait(0.02)
        return got[:n]

    def _scan_measure(self, f, nblocks=SCAN_MEASURE_BLOCKS):
        """Tune to f and return the settled RSSI blocks (list), None (aborted) or 'lost'."""
        self.set_freq(f, user=False)
        ver = self.tune_ver
        t_end = time.time() + 10 * self.scan_time_scale + 5
        while self.applied_ver < ver:
            if self._scan_aborted():
                return None
            if time.time() > t_end:
                return 'lost'
            self.stop_ev.wait(0.02)
        return self._scan_collect(self.applied_t + SCAN_SETTLE_S * self.scan_time_scale, nblocks)

    @staticmethod
    def _percentile(values, pct):
        v = sorted(values)
        return v[min(len(v) - 1, int(len(v) * pct / 100.0))]

    def _run_scan(self, direction, step):
        grid = lambda f: round(f, 3)
        with self.lock:
            self.scan_msg = 'scanning'
            self.scan_excess = None
            self.scan_floor = None
        meas = collections.deque(maxlen=SCAN_FLOOR_WINDOW)
        floor_of = lambda: self._percentile(meas, 20) if len(meas) >= SCAN_WARMUP else None
        f = self.freq
        first = self._scan_collect(time.time(), SCAN_MEASURE_BLOCKS)      # where we start (already settled)
        if first is None:
            return self._scan_finish('stopped') if not self.stop_ev.is_set() else None
        if first == 'lost':
            return self._scan_finish('lost')
        meas.append(min(first))
        armed = False
        first_step = True
        while True:
            # the next grid point in the scan direction (a multiple of the step, so 5 kHz scans hit 5 kHz channels)
            if first_step:
                nxt = (math.floor(f / step + 1e-9) + 1) * step if direction > 0 else (math.ceil(f / step - 1e-9) - 1) * step
                first_step = False
            else:
                nxt = f + direction * step
            nxt = grid(nxt)
            if nxt < FREQ_MIN or nxt > FREQ_MAX:
                return self._scan_finish('end')
            vals = self._scan_measure(nxt)
            if vals is None:
                return self._scan_finish('stopped') if not self.stop_ev.is_set() else None
            if vals == 'lost':
                return self._scan_finish('lost')
            f = nxt
            m = min(vals)
            fl = floor_of()
            with self.lock:
                self.scan_floor = fl
                self.scan_excess = None if fl is None else m - fl
            if fl is None:                                   # warm-up: collect the first readings
                meas.append(m)
                fl2 = floor_of()
                if fl2 is not None:                          # the floor is known now: are we between stations?
                    armed = (m - fl2) < SCAN_T_LO
                continue
            excess = m - fl
            if excess < SCAN_T_LO:                           # quiet: between stations
                armed = True
                meas.append(m)
                continue
            if excess < SCAN_T_HI:                           # a skirt or a very weak signal: neither quiet nor a candidate
                continue
            if not armed:                                    # still on the signal we started on (or its skirt)
                continue
            # ---- a candidate: follow it uphill (at most 3 steps past the best), then go back to the peak
            best_f, best_m = f, m
            past = 0
            cur = f
            while past < 3:
                n2 = grid(cur + direction * step)
                if n2 < FREQ_MIN or n2 > FREQ_MAX:
                    break
                v2 = self._scan_measure(n2)
                if v2 is None:
                    return self._scan_finish('stopped') if not self.stop_ev.is_set() else None
                if v2 == 'lost':
                    return self._scan_finish('lost')
                cur = n2
                m2 = min(v2)
                if m2 > best_m + 0.5:
                    best_f, best_m, past = cur, m2, 0
                else:
                    past += 1
                if m2 < best_m - 3.0:
                    break
            f = cur
            if cur != best_f:                               # back to the peak, then fresh blocks to verify
                ver_blocks = self._scan_measure(best_f, SCAN_VERIFY_BLOCKS)
                f = best_f
            else:                                           # already there: the next blocks are fresh
                ver_blocks = self._scan_collect(time.time(), SCAN_VERIFY_BLOCKS)
            if ver_blocks is None:
                return self._scan_finish('stopped') if not self.stop_ev.is_set() else None
            if ver_blocks == 'lost':
                return self._scan_finish('lost')
            ver_excess = float(np.median(ver_blocks)) - fl
            with self.lock:
                self.scan_excess = ver_excess
            if ver_excess >= SCAN_T_VER and min(ver_blocks) - fl >= SCAN_T_VER_MIN:
                return self._scan_finish('signal', ver_excess)
            armed = False                                    # a false alarm: wait for a quiet reading before trying again

    def _scan_loop(self):
        while not self.stop_ev.is_set():
            if not self.scan_go.wait(0.5):
                continue
            self.scan_go.clear()
            with self.lock:
                p, self.scan_pending = self.scan_pending, None
            if p is None:
                continue
            self.scan_stop_ev.clear()
            try:
                self._run_scan(*p)
            except Exception as e:                           # never leave the UI in "scanning"
                log.error('scan failed: %s', e)
                self._scan_finish('stopped')

    def start(self):
        for target in (self._playback_loop, self._scan_loop, self._fake_loop if self.fake_source else self._kiwi_loop):
            th = threading.Thread(target=target, daemon=True)
            th.start()
            self._threads.append(th)

    def stop(self):
        self.stop_ev.set()
        if self.audio is not None:
            self.audio.stop()
        for th in self._threads:
            th.join(timeout=3)
        self.sink.close()


# --------------------------------------------------------------------------- persistence
def load_saved():
    try:
        with open(STATE_FILE, encoding='utf-8') as f:
            d = json.load(f)
        return {'freq': float(d['freq']), 'mode': d['mode'] if d['mode'] in MODES else 'am',
                'volume': clamp(int(d['volume']), 0, 100), 'server_index': int(d.get('server_index', 0))}
    except (OSError, ValueError, KeyError, TypeError):
        return {}


def save_state(engine):
    try:
        tmp = STATE_FILE + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'freq': engine.freq, 'mode': engine.mode, 'volume': engine.volume,
                       'server_index': engine.server_index}, f)
        os.replace(tmp, STATE_FILE)
    except OSError as e:
        log.warning('could not save settings: %s', e)


# --------------------------------------------------------------------------- HTTP API
def make_handler(engine, token):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.0'

        def log_message(self, fmt, *args):
            log.debug('http %s', fmt % args)

        def reply(self, code, body):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            parts = urlsplit(self.path)
            q = parse_qs(parts.query)
            if q.get('token', [''])[0] != token:
                return self.reply(403, {'error': 'unauthorized'})
            path = parts.path
            try:
                if path == '/state':
                    pass
                elif path == '/tune':
                    khz = float(q['khz'][0])
                    if not math.isfinite(khz) or not FREQ_MIN <= khz <= FREQ_MAX:
                        return self.reply(400, {'error': 'invalid frequency'})
                    engine.set_freq(khz)
                elif path == '/step':
                    delta = int(q['delta'][0])
                    step = float(q.get('khz', ['1'])[0])
                    if not -1000 <= delta <= 1000 or step not in (0.1, 1.0, 5.0, 9.0, 10.0):
                        return self.reply(400, {'error': 'invalid step'})
                    engine.set_freq(engine.freq + delta * step)
                elif path == '/mode':
                    engine.set_mode(q['value'][0])
                elif path == '/volume':
                    engine.set_volume(q['value'][0])
                elif path == '/mute':
                    v = q.get('value', ['toggle'])[0]
                    engine.set_muted((not engine.muted) if v == 'toggle' else v == '1')
                elif path == '/scan':
                    d = int(q.get('dir', ['0'])[0])
                    if d not in (-1, 0, 1):
                        return self.reply(400, {'error': 'invalid scan direction'})
                    if d == 0:
                        engine.stop_scan()
                    else:
                        step = float(q.get('step', ['5'])[0])
                        if step not in (0.1, 1.0, 5.0, 10.0):
                            return self.reply(400, {'error': 'invalid step'})
                        engine.start_scan(d, step)
                elif path == '/output':
                    engine.set_output(q['value'][0])
                elif path == '/servers':
                    body = engine.state()
                    body['servers'] = [{'index': i, 'short': s['short'], 'name': s['name'],
                                        'lat': s['lat'], 'lon': s['lon']}
                                       for i, s in enumerate(engine.servers)]
                    return self.reply(200, body)
                elif path == '/server':
                    engine.switch_server(int(q['index'][0]))
                else:
                    return self.reply(404, {'error': 'unknown route'})
            except (ValueError, KeyError, IndexError) as e:
                return self.reply(400, {'error': str(e) or 'bad request'})
            save_state(engine) if path not in ('/state', '/servers', '/output', '/scan') else None
            return self.reply(200, engine.state())

    return Handler


def discovery_server(token, port=DISCOVERY_PORT):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    platform_support.prepare_listener(udp)
    udp.bind(('0.0.0.0', port))
    want = ('RADIO_DAYS_DISCOVER ' + token).encode()
    while True:
        payload, addr = udp.recvfrom(512)
        if payload == want:
            udp.sendto(('RADIO_DAYS_BRIDGE ' + token).encode(), addr)


def lan_ip():
    return platform_support.lan_ip()


# --------------------------------------------------------------------------- self test
def selftest(engine, seconds):
    """Connects for real, tunes between frequencies, reports measured values.  Exit 0 only if
    audio blocks arrived, audio was non-silent, the output path ran and the tune commands were
    accepted."""
    print('selftest: server=%s' % engine.servers[engine.server_index]['name'])
    t0 = time.time()
    while time.time() - t0 < 45 and engine.state()['link'] != 'connected':
        time.sleep(0.5)
    st = engine.state()
    if st['link'] != 'connected':
        print('FAIL link=%s error=%s' % (st['link'], st['error']))
        return 1
    print('connected after %.1fs, sample_rate=%s' % (time.time() - t0, st['sample_rate']))
    results = []
    for f, m in ((10000.0, 'am'), (9580.0, 'am'), (15000.0, 'am'), (10000.0, 'am')):
        engine.set_freq(f)
        engine.set_mode(m)
        time.sleep(max(3.0, seconds / 4))
        s = engine.state()
        results.append(s)
        print('  tuned %8.1f kHz %-3s  rssi=%6s dBm  S=%.1f  in_level=%.4f  out_level=%.4f  blocks=%d played=%d'
              % (f, m, s['rssi'], s['s'], s['in_level'], s['out_level'], s['blocks'], s['played_blocks']))
    last = results[-1]
    ok = True
    if last['blocks'] < 50:
        print('FAIL: too few audio blocks'); ok = False
    if last['in_level'] <= 0.0:
        print('FAIL: received audio is silent'); ok = False
    if last['played_blocks'] < 50 or last['audio_error']:
        print('FAIL: audio output did not run (%s)' % last['audio_error']); ok = False
    rssis = [r['rssi'] for r in results if r['rssi'] is not None]
    if rssis and max(rssis) - min(rssis) < 1.0:
        print('WARN: RSSI did not change across frequencies (tuning may not reach the Kiwi)')
    print('RESULT: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description='RADIO DAYS SDR Mac server')
    ap.add_argument('--token', default='RADIO_DAYS_LOCAL_2026')
    ap.add_argument('--port', type=int, default=HTTP_PORT)
    ap.add_argument('--discovery-port', type=int, default=DISCOVERY_PORT)
    ap.add_argument('--audio-port', type=int, default=AUDIO_PORT, help='TCP port that serves audio to the Cardputer speaker')
    ap.add_argument('--output', choices=OUTPUTS, default='mac', help='audio output at start (the Cardputer sets it afterwards)')
    ap.add_argument('--servers', default=os.path.join(HERE, 'servers.json'), help='the receiver list (created from servers.default.json on first start)')
    ap.add_argument('--open-manager', action='store_true', help='open the Receiver Manager in the default browser once it is up (the start files do)')
    ap.add_argument('--manager-port', type=int, default=8780, help='port of the Receiver Manager web page (127.0.0.1 only); 0 = off')
    ap.add_argument('--server-index', type=int, default=None)
    ap.add_argument('--freq', type=float, default=None)
    ap.add_argument('--mode', choices=MODES, default=None)
    ap.add_argument('--volume', type=int, default=None)
    ap.add_argument('--sound-device', default=None, help='part of an output device name (see --list-devices)')
    ap.add_argument('--list-devices', action='store_true')
    ap.add_argument('--no-audio', action='store_true', help='do not open any audio output')
    ap.add_argument('--no-kiwi', action='store_true', help='use a test tone instead of a KiwiSDR (offline tests)')
    ap.add_argument('--selftest', type=float, nargs='?', const=16.0, default=None, metavar='SEC',
                    help='connect, tune, print measurements, exit with 0 only if everything worked')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    platform_support.setup_console()
    if platform_support.keep_awake():
        print('Windows: スリープを防止しています（終了すると解除） / keeping Windows awake while this runs')
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format='%(asctime)s %(levelname)s %(message)s')
    if args.list_devices:
        import soundcard as sc
        for sp in sc.all_speakers():
            print(sp.name)
        return 0

    if not args.no_kiwi:
        try:
            import kiwi  # noqa: F401
        except ImportError:
            print('The KiwiSDR client library is missing.  Run:  python3 tools/fetch_kiwiclient.py   (the start file does this for you)', file=sys.stderr)
            return 2
    saved = load_saved() if args.selftest is None else {}
    store = servers_store.ServerStore(args.servers, default_path=os.path.join(HERE, 'servers.default.json'))
    try:
        servers, warn = store.load_with_fallback()
    except servers_store.ServersError as e:
        print('Cannot read the receiver list: %s' % e, file=sys.stderr)
        return 2
    if warn:
        print(warn)
    sink = NullSink() if args.no_audio else SoundcardSink(args.sound_device)
    engine = Engine(
        servers, sink,
        start_freq=args.freq if args.freq is not None else saved.get('freq', 10000.0),
        start_mode=args.mode or saved.get('mode', 'am'),
        volume=args.volume if args.volume is not None else saved.get('volume', 70),
        server_index=args.server_index if args.server_index is not None else saved.get('server_index', 0),
        fake_source=args.no_kiwi)
    engine.output = args.output
    engine.start()
    if args.selftest is not None:
        try:
            return selftest(engine, args.selftest)
        finally:
            engine.stop()

    engine.audio = AudioServer(engine, args.token, args.audio_port)
    engine.audio.start()
    if args.manager_port:
        try:
            import receiver_manager
            receiver_manager.start(store, engine, args.manager_port, Engine, NullSink)
            print('Receiver Manager: http://127.0.0.1:%d/   (choose and order receivers in your browser)' % args.manager_port)
            if args.open_manager:                      # a browser problem never stops the radio
                receiver_manager.open_in_browser('http://127.0.0.1:%d/' % args.manager_port)
        except OSError as e:
            who = receiver_manager.probe(args.manager_port)    # a busy port alone does not mean it is ours
            murl = 'http://127.0.0.1:%d/' % args.manager_port
            if who == 'ours':
                print('Receiver Manager はすでに別のプロセスで起動しています（%s など）' % platform_support.MANAGER_FILE + '。この画面で保存した一覧は、このラジオを再起動すると使われます。')
                print('A RADIO DAYS Receiver Manager is already running in another process (e.g. %s): %s\n'
                      '  Lists saved there are used by this radio from its next start. Quit that one and restart this to edit while listening.' % (platform_support.MANAGER_FILE, murl))
                if args.open_manager:
                    receiver_manager.open_in_browser(murl)
            elif who == 'other':
                print('!' * 60 + '\n! Receiver Manager は使えません / Receiver Manager is NOT available\n' + receiver_manager.port_busy_text(args.manager_port) + '\n  (ラジオ本体は動いています / the radio itself keeps running)\n' + '!' * 60)
            else:
                print('!! Receiver Manager を起動できませんでした / Receiver Manager could not start (port %d: %s). ラジオ本体は動いています / the radio keeps running.' % (args.manager_port, e))
    threading.Thread(target=discovery_server, args=(args.token, args.discovery_port), daemon=True).start()
    try:
        httpd = platform_support.LocalHTTPServer(('0.0.0.0', args.port), make_handler(engine, args.token))
    except OSError as e:
        print('!! RADIO DAYS SDR を起動できません（ポート %d: %s）。すでに RADIO DAYS SDR が動いていませんか？ 別のウィンドウで動いているものを Ctrl+C で止めてから、もう一度起動してください。' % (args.port, e))
        print('!! Cannot start (port %d: %s). Is RADIO DAYS SDR already running in another window? Stop that one with Ctrl+C and start again.' % (args.port, e))
        engine.stop()
        return 1
    httpd.daemon_threads = True
    print('RADIO DAYS SDR  HTTP :%d  discovery UDP :%d  Mac IP %s' % (args.port, DISCOVERY_PORT, lan_ip()))
    print('KiwiSDR: %s   audio: %s' % (servers[engine.server_index]['name'], getattr(sink, 'name', '') or 'default device'))
    print('Audio plays on this PC until the Cardputer chooses its own speaker (O key, "OUT ADV"); its volume/mute keys apply either way.')
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        engine.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
