"""Receiver Manager tests: store safety, HTTP security, save/restore, status check.  No internet, no KiwiSDR."""
import http.server
import json
import os
import socket
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import check_history as ch  # noqa: E402
import receiver_manager as rm  # noqa: E402
import servers_store as ss  # noqa: E402

A = {'name': 'Test A', 'short': 'A', 'host': 'a.example.org', 'port': 8073, 'lat': 35.0, 'lon': 139.0}
B = {'name': 'Test B', 'short': 'B', 'host': 'b.example.org', 'port': 8074, 'lat': None, 'lon': None}


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.path = os.path.join(self.d, 'servers.json')
        self.default = os.path.join(MAC, 'servers.default.json')
        self.store = ss.ServerStore(self.path, self.default)

    def test_first_start_copies_default(self):
        self.assertEqual(len(self.store.load()), 4)
        self.assertTrue(os.path.exists(self.path))

    def test_save_roundtrip_and_backup(self):
        self.store.ensure()
        before = open(self.path, 'rb').read()
        self.store.save([A, B])
        self.assertEqual([s['short'] for s in self.store.load()], ['A', 'B'])
        self.assertEqual(open(self.path + '.bak', 'rb').read(), before)

    def test_invalid_does_not_touch_file(self):
        self.store.ensure()
        before = open(self.path, 'rb').read()
        bad = [dict(A, host='http://x/y'), dict(A, port=0), dict(A, name='日本語'), dict(A, lat=95), dict(A, lat=1.0, lon=None), [], [A, A], [dict(A, host='h%d.example.org' % i) for i in range(33)]]
        for b in bad:
            with self.assertRaises(ss.ServersError):
                self.store.save(b if isinstance(b, list) else [b])
        self.assertEqual(open(self.path, 'rb').read(), before)
        self.assertEqual([f for f in os.listdir(self.d) if f.startswith('.') and f.endswith('.tmp')], [])

    def test_corrupt_file_falls_back_and_is_kept(self):
        self.store.ensure()
        self.store.save([A])
        self.store.save([A, B])                    # .bak now holds [A]
        open(self.path, 'w').write('{ broken')
        got, warn = self.store.load_with_fallback()
        self.assertTrue(warn)
        self.assertEqual(len(got), 1)
        self.store.save([B])
        self.assertTrue([f for f in os.listdir(self.d) if '.corrupt-' in f])

    def test_password_not_exposed(self):
        self.store.save([dict(A, password='secret')])
        r = rm.Manager(self.store).state()
        self.assertNotIn('secret', json.dumps(r))
        self.assertTrue(r['servers'][0]['password_set'])
        rm.Manager(self.store).save([dict(A, keep_password=True)])
        self.assertEqual(self.store.load()[0]['password'], 'secret')


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.d = tempfile.mkdtemp()
        cls.store = ss.ServerStore(os.path.join(cls.d, 'servers.json'), os.path.join(MAC, 'servers.default.json'))
        cls.store.ensure()
        cls.port = free_port()
        cls.httpd, cls.mgr = rm.start(cls.store, None, cls.port)

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()

    def req(self, path, body=None, headers=None, host=None):
        h = {'Host': host or '127.0.0.1:%d' % self.port}
        h.update(headers or {})
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h.setdefault('Content-Type', 'application/json')
        r = urllib.request.Request('http://127.0.0.1:%d%s' % (self.port, path), data=data, headers=h)
        try:
            with urllib.request.urlopen(r, timeout=10) as f:
                return f.status, json.loads(f.read() or b'{}') if 'json' in f.headers.get('Content-Type', '') else f.read()
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b'{}')

    CSRF = {'X-RDSDR-Manager': '1'}

    def test_static_and_state(self):
        for p in ('/', '/app.js', '/style.css'):
            self.assertEqual(self.req(p)[0], 200)
        c, st = self.req('/api/state')
        self.assertEqual((c, st['running']), (200, False))
        c, cand = self.req('/api/candidates')
        self.assertGreater(len(cand['receivers']), 50)

    def test_host_header_blocked(self):
        self.assertEqual(self.req('/api/state', host='evil.example.com')[0], 403)

    def test_post_needs_csrf_header_and_origin(self):
        self.assertEqual(self.req('/api/save', {'servers': [A]})[0], 403)
        self.assertEqual(self.req('/api/save', {'servers': [A]}, dict(self.CSRF, Origin='http://evil.example.com'))[0], 403)
        self.assertEqual(self.req('/api/save', {'servers': [A]}, dict(self.CSRF, **{'Content-Type': 'text/plain'}))[0], 403)

    def test_too_large_and_bad_json(self):
        r = urllib.request.Request('http://127.0.0.1:%d/api/save' % self.port, data=b'{bad', headers=dict(self.CSRF, **{'Content-Type': 'application/json'}))
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(r)
        self.assertEqual(cm.exception.code, 400)
        self.assertEqual(self.req('/api/save', {'servers': [A], 'pad': 'x' * 300000}, self.CSRF)[0], 413)

    def test_save_restore_conflict(self):
        c, st = self.req('/api/state')
        c, r = self.req('/api/save', {'servers': [A, B], 'base_mtime': st['file']['mtime']}, self.CSRF)
        self.assertEqual((c, r['count']), (200, 2))
        c, r = self.req('/api/save', {'servers': [A], 'base_mtime': st['file']['mtime']}, self.CSRF)   # stale
        self.assertEqual((c, r.get('code')), (409, 'conflict'))
        self.assertEqual(len(self.store.load()), 2)
        c, r = self.req('/api/save', {'servers': [A], 'base_mtime': st['file']['mtime'], 'force': True}, self.CSRF)
        self.assertEqual(c, 200)
        c, r = self.req('/api/restore', {}, self.CSRF)
        self.assertEqual(c, 200)
        self.assertEqual(len(self.store.load()), 2)

    def test_invalid_save_is_400_and_file_kept(self):
        before = open(self.store.path, 'rb').read()
        c, r = self.req('/api/save', {'servers': [dict(A, port=99999)]}, self.CSRF)
        self.assertEqual(c, 400)
        self.assertEqual(open(self.store.path, 'rb').read(), before)

    def test_check_status_wording(self):
        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(s):
                body = b'status=active\nname=T\nusers=1\nusers_max=4\noffline=no\n'
                s.send_response(200)
                s.send_header('Content-Length', str(len(body)))
                s.end_headers()
                s.wfile.write(body)

            def log_message(s, *a):
                pass
        p = free_port()
        srv = http.server.HTTPServer(('127.0.0.1', p), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            r = rm.check_status('127.0.0.1', p)
            self.assertEqual((r['level'], r['ok']), ('status', True))
            self.assertNotIn('audio', json.dumps(r).replace('"level": "status"', ''))
        finally:
            srv.shutdown()
        r = rm.check_status('127.0.0.1', free_port(), timeout=2)
        self.assertFalse(r['ok'])

    def test_check_rejects_bad_host(self):
        self.assertEqual(self.req('/api/check', {'host': 'a/b', 'port': 80}, self.CSRF)[0], 400)


class CandidateTests(unittest.TestCase):
    def test_schema_and_honesty(self):
        d = json.load(open(os.path.join(MAC, 'manager', 'receiver_candidates.json'), encoding='utf-8'))
        self.assertIn('NOT', d['about'])
        keys = set()
        shorts = set()
        for c in d['receivers']:
            ss.clean_server(dict(c), 0)
            k = (c['host'].lower(), c['port'])
            self.assertNotIn(k, keys)
            keys.add(k)
            self.assertNotIn(c['short'], shorts)
            shorts.add(c['short'])
        self.assertGreaterEqual(sum(1 for c in d['receivers'] if c['country'] == 'Japan'), 10)


class HistoryTests(unittest.TestCase):
    """check_history.json: the last HTTP check and the last audio check of each receiver, kept apart from servers.json"""

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.path = os.path.join(self.d, 'check_history.json')

    def test_http_and_audio_are_separate_and_survive_reopening(self):
        h = ch.CheckHistory(self.path)
        self.assertEqual(h.snapshot(), {})                                     # never checked: nothing stored, nothing invented
        self.assertTrue(h.record('A.Example.org', 8073, 'status', {'ok': True, 'available': True, 'users': 1, 'users_max': 4, 'checked_at': '2026-10-24T01:02:03Z'}))
        self.assertNotIn('audio', h.snapshot()['a.example.org:8073'])           # an HTTP check does not create an audio result
        h.record('a.example.org', 8073, 'audio', {'ok': False, 'error': 'busy', 'checked_at': '2026-10-24T02:00:00Z'})
        again = ch.CheckHistory(self.path).snapshot()                            # "closing the browser / restarting the program"
        e = again['a.example.org:8073']
        self.assertEqual((e['status']['ok'], e['status']['users'], e['status']['checked_at']), (True, 1, '2026-10-24T01:02:03Z'))
        self.assertEqual((e['audio']['ok'], e['audio']['error']), (False, 'busy'))
        h.record('a.example.org', 8073, 'status', {'ok': False, 'error': 'timeout', 'checked_at': '2026-10-24T03:00:00Z'})
        e = ch.CheckHistory(self.path).snapshot()['a.example.org:8073']
        self.assertEqual(e['status']['error'], 'timeout')                        # the newest replaces the previous one of the same kind
        self.assertEqual(e['audio']['error'], 'busy')                            # and leaves the other kind alone

    def test_only_known_small_fields_are_stored(self):
        h = ch.CheckHistory(self.path)
        h.record('a.example.org', 8073, 'status', {'ok': True, 'name': 'secret-name', 'password': 'pw', 'error': 'x' * 1000, 'evil': {'a': 1}})
        raw = open(self.path, encoding='utf-8').read()
        for bad in ('secret-name', '"pw"', 'evil'):
            self.assertNotIn(bad, raw)
        self.assertLessEqual(len(h.snapshot()['a.example.org:8073']['status']['error']), 160)
        self.assertRegex(h.snapshot()['a.example.org:8073']['status']['checked_at'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')

    def test_damaged_file_is_set_aside_and_history_starts_empty(self):
        open(self.path, 'w').write('{ not json')
        h = ch.CheckHistory(self.path)
        self.assertEqual(h.snapshot(), {})
        self.assertTrue(h.note)
        self.assertTrue([f for f in os.listdir(self.d) if '.corrupt-' in f])
        self.assertTrue(h.record('a.example.org', 1, 'status', {'ok': True}))    # and it works again
        open(self.path, 'w').write(json.dumps({'entries': {'x:1': {'status': 'not a dict'}, 'y:2': 5}}))
        self.assertEqual(ch.CheckHistory(self.path).snapshot(), {})              # wrong shapes are ignored, not trusted

    def test_unwritable_location_never_raises(self):
        h = ch.CheckHistory(os.path.join(self.d, 'no-such-dir', 'check_history.json'))
        self.assertFalse(h.record('a.example.org', 1, 'status', {'ok': True}))   # reported as "not stored", no exception
        self.assertIn('a.example.org:1', h.snapshot())                           # still available for this session

    def test_oldest_entries_are_dropped_at_the_limit(self):
        h = ch.CheckHistory(self.path)
        for i in range(ch.MAX_ENTRIES + 5):
            h.entries['h%d.example.org:80' % i] = {'status': {'ok': True, 'checked_at': '2026-01-01T00:%02d:%02dZ' % (i // 60 % 60, i % 60)}}
        h.record('new.example.org', 80, 'status', {'ok': True, 'checked_at': '2026-12-31T00:00:00Z'})
        snap = h.snapshot()
        self.assertEqual(len(snap), ch.MAX_ENTRIES)
        self.assertIn('new.example.org:80', snap)

    def test_servers_json_is_never_touched(self):
        store = ss.ServerStore(os.path.join(self.d, 'servers.json'), os.path.join(MAC, 'servers.default.json'))
        store.ensure()
        before, mt = open(store.path, 'rb').read(), os.stat(store.path).st_mtime_ns
        mgr = rm.Manager(store)
        mgr.history.record('a.example.org', 8073, 'status', {'ok': True})
        self.assertEqual((open(store.path, 'rb').read(), os.stat(store.path).st_mtime_ns), (before, mt))
        self.assertEqual(os.path.dirname(mgr.history.path), self.d)               # next to the servers file it belongs to
        self.assertNotIn('check', ' '.join(json.dumps(s) for s in store.load()))  # no history field leaks into a receiver entry


class CheckApiTests(unittest.TestCase):
    """checks through the HTTP API: recording, wording, the limit on simultaneous requests"""

    @classmethod
    def setUpClass(cls):
        cls.d = tempfile.mkdtemp()
        cls.store = ss.ServerStore(os.path.join(cls.d, 'servers.json'), os.path.join(MAC, 'servers.default.json'))
        cls.store.ensure()
        cls.port = free_port()
        cls.calls = []
        cls.live = 0
        cls.peak = 0
        cls.lock = threading.Lock()
        outer = cls

        class H(http.server.BaseHTTPRequestHandler):                              # a fake KiwiSDR /status page that is slow on purpose
            def do_GET(s):
                with outer.lock:
                    outer.live += 1
                    outer.peak = max(outer.peak, outer.live)
                    outer.calls.append(s.path)
                import time
                time.sleep(0.25)
                body = b'status=active\nname=T\nusers=1\nusers_max=4\noffline=no\n'
                s.send_response(200)
                s.send_header('Content-Length', str(len(body)))
                s.end_headers()
                s.wfile.write(body)
                with outer.lock:
                    outer.live -= 1

            def log_message(s, *a):
                pass
        cls.fake_port = free_port()
        cls.fake = http.server.ThreadingHTTPServer(('127.0.0.1', cls.fake_port), H)
        cls.fake.daemon_threads = True
        threading.Thread(target=cls.fake.serve_forever, daemon=True).start()
        cls.httpd, cls.mgr = rm.start(cls.store, None, cls.port)

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.fake.shutdown()

    def req(self, path, body=None):
        h = {'Host': '127.0.0.1:%d' % self.port}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h.update({'Content-Type': 'application/json', 'X-RDSDR-Manager': '1'})
        with urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:%d%s' % (self.port, path), data=data, headers=h), timeout=20) as f:
            return json.loads(f.read())

    def test_opening_the_page_asks_no_receiver(self):
        before = len(self.calls)
        for p in ('/', '/app.js', '/guide.html', '/api/state', '/api/candidates', '/api/check-history'):
            urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:%d%s' % (self.port, p), headers={'Host': '127.0.0.1:%d' % self.port}), timeout=10).read()
        self.assertEqual(len(self.calls), before)

    def test_check_is_recorded_and_served_back(self):
        r = self.req('/api/check', {'host': '127.0.0.1', 'port': self.fake_port, 'level': 'status'})
        self.assertTrue(r['ok'] and r['recorded'])
        self.assertRegex(r['checked_at'], r'Z$')
        e = self.req('/api/check-history')['entries']['127.0.0.1:%d' % self.fake_port]
        self.assertTrue(e['status']['ok'])
        self.assertNotIn('audio', e)                                              # HTTP check recorded no audio result
        self.assertTrue(os.path.exists(os.path.join(self.d, 'check_history.json')))

    def test_failed_check_is_recorded_with_its_reason(self):
        p = free_port()
        r = self.req('/api/check', {'host': '127.0.0.1', 'port': p, 'level': 'status'})
        self.assertFalse(r['ok'])
        e = self.req('/api/check-history')['entries']['127.0.0.1:%d' % p]['status']
        self.assertFalse(e['ok'])
        self.assertIn('refused', e['error'])

    def test_simultaneous_http_checks_are_limited(self):
        self.__class__.peak = 0
        ports = [self.fake_port] * 12
        out = []

        def one(i):
            out.append(self.req('/api/check', {'host': '127.0.0.1', 'port': ports[i], 'level': 'status'}))
        ts = [threading.Thread(target=one, args=(i,)) for i in range(12)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(len(out), 12)
        self.assertTrue(all(o['ok'] for o in out))
        self.assertLessEqual(self.peak, rm.BULK_CONCURRENCY)
        self.assertGreaterEqual(self.peak, 2)                                    # (so the test really exercised parallel requests)
        self.assertEqual(self.req('/api/check-history')['bulk_concurrency'], rm.BULK_CONCURRENCY)

    def test_audio_check_is_recorded_apart_and_a_check_that_could_not_run_is_not(self):
        class Eng:
            def __init__(s, servers, sink, volume=0):
                s.connect_timeout = 0

            def start(s):
                pass

            def stop(s):
                pass

            def state(s):
                return {'link': 'connected', 'blocks': 9, 'sample_rate': 12000, 'rssi': -80, 'error': ''}
        mgr = rm.Manager(self.store, None, Eng, lambda: None)
        mgr.history = ch.CheckHistory(os.path.join(self.d, 'h2.json'))
        r = mgr.run_check('audio', '127.0.0.1', 5)
        self.assertTrue(r['ok'] and r['recorded'])
        e = mgr.history.snapshot()['127.0.0.1:5']
        self.assertTrue(e['audio']['ok'])
        self.assertNotIn('status', e)                                            # an audio check does not stand in for an HTTP check
        mgr.audio_lock.acquire()
        try:
            r2 = mgr.run_check('audio', '127.0.0.1', 6)
        finally:
            mgr.audio_lock.release()
        self.assertFalse(r2['ok'])
        self.assertNotIn('recorded', r2)
        self.assertNotIn('127.0.0.1:6', mgr.history.snapshot())                  # "another check was running" is not a result about the receiver

    def test_guide_files_are_served(self):
        for p, ct in (('/guide.html', 'text/html'), ('/guide.js', 'text/javascript'), ('/guide.json', 'application/json')):
            with urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:%d%s' % (self.port, p), headers={'Host': '127.0.0.1:%d' % self.port}), timeout=10) as f:
                self.assertEqual(f.status, 200)
                self.assertTrue(f.headers['Content-Type'].startswith(ct), p)
                body = f.read()
            if p == '/guide.json':
                self.assertGreater(len(json.loads(body)['sections']), 5)


if __name__ == '__main__':
    unittest.main()
