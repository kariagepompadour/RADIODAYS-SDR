"""AUTO scan tests with a simulated sky (no KiwiSDR needed).

The simulated receiver behaves like the real ones measured on 2026-10-24: noise floor about -110 dBm, 171 ms blocks,
and after a retune the old frequency is still heard for ~0.75 s.  Everything runs 10x faster than real time.
"""
import os
import random
import sys
import threading
import time
import unittest

import numpy as np

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import radiodays_sdr as r  # noqa: E402

SCALE = 0.1


class Sky:
    """stations: {kHz: dBm}.  Skirts at +-5 kHz (-22 dB) and +-10 kHz (-40 dB), fading, impulse noise."""

    def __init__(self, stations, floor=-110.0, impulse=0.0, fading=1.5, seed=1):
        self.stations, self.floor, self.impulse, self.fading = stations, floor, impulse, fading
        self.rng = random.Random(seed)

    def rssi(self, f):
        p = 10 ** (self.floor / 10)
        for sf, lvl in self.stations.items():
            d = abs(f - sf)
            att = 0 if d < 2.5 else (22 if d < 7.5 else (40 if d < 12.5 else 99))
            if att < 99:
                p += 10 ** ((lvl - att + self.rng.gauss(0, self.fading)) / 10)
        v = 10 * np.log10(p) + self.rng.gauss(0, 1.2)
        if self.rng.random() < self.impulse:
            v = max(v, self.floor + 25 + self.rng.random() * 10)           # a lightning crash in one block
        return float(v)


def make_engine(sky, start=9480.0, latency=0.75):
    servers = [{'name': 'sim', 'short': 'sim', 'host': 'x', 'port': 1, 'password': '', 'lat': None, 'lon': None}]
    e = r.Engine(servers, r.NullSink(), start_freq=start, volume=0, fake_source=True)
    e.scan_time_scale = SCALE
    state = {'linkdown': False}

    def loop():
        hist = [(0.0, e.freq)]
        last_ver = e.tune_ver
        nxt = time.time()
        while not e.stop_ev.is_set():
            now = time.time()
            with e.lock:
                ver, fr = e.tune_ver, e.freq
            if ver != last_ver:
                last_ver = ver
                e.applied_t, e.applied_ver = now, ver
                hist.append((now, fr))
            heard = hist[0][1]
            for t, fq in hist:
                if t <= now - latency * SCALE:
                    heard = fq
            if state['linkdown']:
                e.link = 'connecting'
            else:
                e.push_audio(np.zeros(64, np.float32), sky.rssi(heard), 12000)
            nxt += 0.171 * SCALE
            e.stop_ev.wait(max(0.0, nxt - time.time()))
    e._fake_loop = loop
    e.state_hooks = state
    e.start()
    return e


def wait_done(e, timeout=60):
    t = time.time()
    while time.time() - t < timeout:
        if e.state()['scan'] == 'off':
            return True
        time.sleep(0.02)
    return False


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.es = []
        r.save_state = lambda engine: None                        # do not touch the user's saved settings

    def tearDown(self):
        for e in self.es:
            e.stop()

    def eng(self, sky, **kw):
        e = make_engine(sky, **kw)
        self.es.append(e)
        time.sleep(0.2)
        return e

    STATIONS = {9500: -75, 9535: -77, 9570: -94, 9600: -90, 9665: -66}

    def test_finds_the_next_station_upward_at_its_peak(self):
        e = self.eng(Sky(self.STATIONS), start=9480.0)
        e.start_scan(+1, 5.0)
        self.assertTrue(wait_done(e))
        st = e.state()
        self.assertEqual(st['scan_msg'], 'signal')
        self.assertEqual(st['freq'], 9500.0)
        self.assertGreater(st['scan_excess'], 25)

    def test_next_scan_leaves_the_station_and_finds_the_following_one(self):
        e = self.eng(Sky(self.STATIONS), start=9480.0)
        for expect in (9500.0, 9535.0, 9570.0, 9600.0, 9665.0):
            e.start_scan(+1, 5.0)
            self.assertTrue(wait_done(e, 90))
            self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', expect))

    def test_downward_and_a_weak_station(self):
        e = self.eng(Sky(self.STATIONS), start=9640.0)
        e.start_scan(-1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', 9600.0))
        e.start_scan(-1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', 9570.0))      # -94 dBm: 16 dB over the floor

    def test_scan_starting_on_a_station_leaves_it(self):
        e = self.eng(Sky(self.STATIONS), start=9535.0)
        e.start_scan(+1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual(e.state()['freq'], 9570.0)

    def test_impulse_noise_does_not_stop_the_scan(self):
        for seed in (1, 2, 3):
            e = self.eng(Sky({9500: -75}, impulse=0.15, seed=seed), start=9400.0)
            e.start_scan(+1, 5.0)
            self.assertTrue(wait_done(e, 90))
            self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', 9500.0), 'seed %d' % seed)

    def test_a_barely_audible_signal_is_not_a_stop(self):
        e = self.eng(Sky({9450: -101, 9520: -80}), start=9420.0)       # 9 dB over the floor: below the 12 dB threshold
        e.start_scan(+1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual(e.state()['freq'], 9520.0)

    def test_fading_signal_is_found(self):
        e = self.eng(Sky({9500: -88}, fading=4.0, seed=5), start=9470.0)
        e.start_scan(+1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', 9500.0))

    def test_end_of_band(self):
        e = self.eng(Sky({}), start=29980.0)
        e.start_scan(+1, 5.0)
        self.assertTrue(wait_done(e, 60))
        st = e.state()
        self.assertEqual(st['scan_msg'], 'end')
        self.assertLessEqual(st['freq'], 30000.0)

    def test_stop_command_and_user_tuning_stop_the_scan(self):
        e = self.eng(Sky({}), start=9400.0)
        e.start_scan(+1, 5.0)
        time.sleep(1.0)
        self.assertEqual(e.state()['scan'], 'up')
        e.stop_scan()
        self.assertEqual(e.state()['scan'], 'off')
        f1 = e.state()['freq']
        time.sleep(0.6)
        self.assertEqual(e.state()['freq'], f1, 'the scan kept tuning after it was stopped')
        e.start_scan(+1, 5.0)
        time.sleep(0.8)
        e.set_freq(9000.0)                                           # the user types a frequency
        self.assertEqual(e.state()['scan'], 'off')
        time.sleep(0.6)
        self.assertEqual(e.state()['freq'], 9000.0)

    def test_reversing_direction_restarts(self):
        e = self.eng(Sky({9300: -80, 9500: -80}), start=9400.0)
        e.start_scan(+1, 5.0)
        time.sleep(0.8)
        e.start_scan(-1, 5.0)
        self.assertTrue(wait_done(e, 90))
        self.assertEqual((e.state()['scan_msg'], e.state()['freq']), ('signal', 9300.0))

    def test_scan_step_is_at_least_5_khz_and_on_the_grid(self):
        e = self.eng(Sky({9500: -75}), start=9477.3)
        e.start_scan(+1, 1.0)                                         # STEP 1 kHz selected: the scan still uses 5 kHz
        self.assertTrue(wait_done(e, 90))
        self.assertEqual(e.state()['freq'], 9500.0)

    def test_link_loss_ends_the_scan_as_lost(self):
        e = self.eng(Sky({}), start=9400.0)
        e.start_scan(+1, 5.0)
        time.sleep(0.6)
        e.state_hooks['linkdown'] = True
        self.assertTrue(wait_done(e, 60))
        self.assertEqual(e.state()['scan_msg'], 'lost')

    def test_speed_matches_the_measured_receiver(self):
        e = self.eng(Sky({}), start=9400.0)
        f0 = e.state()['freq']
        e.start_scan(+1, 5.0)
        time.sleep(6.0)
        steps = (e.state()['freq'] - f0) / 5.0
        per_step = 6.0 / SCALE / max(steps, 1)                       # real-time seconds per step
        e.stop_scan()
        self.assertTrue(1.0 < per_step < 1.9, 'a step takes %.2f s (real time)' % per_step)


if __name__ == '__main__':
    unittest.main()
