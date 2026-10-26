"""Start-up behaviour: the browser opens by itself, a second start does not duplicate, a busy port is explained.
Uses a recording stand-in for the browser (the BROWSER environment variable), so no real browser is opened."""
import http.server
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import receiver_manager as rm  # noqa: E402
import servers_store as ss  # noqa: E402

HOME = '/tmp/radiodays_test_home'


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Env:
    """a temp dir with a fake browser that records the URLs it is asked to open"""

    def __init__(self, browser_exit=0):
        os.makedirs(HOME, exist_ok=True)
        self.d = tempfile.mkdtemp(dir=HOME)
        self.log = os.path.join(self.d, 'opened.txt')
        self.script = os.path.join(self.d, 'fakebrowser.sh')
        with open(self.script, 'w') as f:
            f.write('#!/bin/sh\necho "$1" >> "%s"\nexit %d\n' % (self.log, browser_exit))
        os.chmod(self.script, os.stat(self.script).st_mode | stat.S_IEXEC)
        self.servers = os.path.join(self.d, 'servers.json')

    def env(self):
        return dict(os.environ, HOME=HOME, BROWSER=self.script + ' %s', PYTHONUNBUFFERED='1')

    def opened(self):
        for _ in range(30):
            if os.path.exists(self.log):
                break
            time.sleep(0.1)
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return f.read().split()


def run_manager(env, port, *extra, timeout=15):
    return subprocess.run([sys.executable, '-I', '-u', os.path.join(MAC, 'receiver_manager.py'), '--servers', env.servers, '--port', str(port), *extra],
                          capture_output=True, text=True, timeout=timeout, env=env.env())


class Radio:
    def __init__(self, env, manager_port, *extra):
        self.port, self.mport = free_port(), manager_port
        self.p = subprocess.Popen([sys.executable, '-I', '-u', os.path.join(MAC, 'radiodays_sdr.py'), '--no-kiwi', '--no-audio', '--token', 't',
                                   '--port', str(self.port), '--discovery-port', str(free_port()), '--audio-port', str(free_port()),
                                   '--servers', env.servers, '--manager-port', str(manager_port), *extra],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env.env())
        self.lines = []
        threading.Thread(target=lambda: [self.lines.append(l) for l in self.p.stdout], daemon=True).start()
        for _ in range(80):
            try:
                self.state()
                return
            except Exception:
                time.sleep(0.2)
        self.close()
        raise RuntimeError('radio did not start: ' + ''.join(self.lines))

    def state(self):
        with urllib.request.urlopen('http://127.0.0.1:%d/state?token=t' % self.port, timeout=3) as r:
            return json.loads(r.read())

    def out(self):
        time.sleep(0.3)
        return ''.join(self.lines)

    def close(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


class Listener:
    """some other program on a port: kind 'silent' accepts and never answers, 'html' answers a web page, 'json' answers JSON of another app"""

    def __init__(self, kind):
        self.port = free_port()
        self.kind = kind
        if kind == 'silent':
            self.s = socket.socket()
            self.s.bind(('127.0.0.1', self.port))
            self.s.listen(5)
            self.httpd = None
        else:
            body = b'<html>hello</html>' if kind == 'html' else json.dumps({'app': 'something else'}).encode()
            legacy = json.dumps({'servers': [], 'file': {}, 'limits': {}, 'running': True}).encode()

            class H(http.server.BaseHTTPRequestHandler):
                def do_GET(s):
                    if kind == 'legacy':                       # a Receiver Manager from before /api/identify: 404 there, but its /api/state
                        if s.path.startswith('/api/state'):
                            s.send_response(200)
                            s.send_header('Content-Length', str(len(legacy)))
                            s.end_headers()
                            s.wfile.write(legacy)
                        else:
                            s.send_response(404)
                            s.send_header('Content-Length', '0')
                            s.end_headers()
                        return
                    s.send_response(200)
                    s.send_header('Content-Length', str(len(body)))
                    s.end_headers()
                    s.wfile.write(body)

                def log_message(s, *a):
                    pass
            self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', self.port), H)
            threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        if self.httpd:
            self.httpd.shutdown()
            self.httpd.server_close()
        else:
            self.s.close()


class ProbeTests(unittest.TestCase):
    def test_free_ours_and_other(self):
        self.assertEqual(rm.probe(free_port()), 'free')
        e = Env()
        port = free_port()
        store = ss.ServerStore(e.servers, os.path.join(MAC, 'servers.default.json'))
        store.ensure()
        httpd, _ = rm.start(store, None, port)
        try:
            self.assertEqual(rm.probe(port), 'ours')
        finally:
            httpd.shutdown()
            httpd.server_close()
        l = Listener('legacy')
        try:
            self.assertEqual(rm.probe(l.port, timeout=0.6), 'ours')           # an older RADIO DAYS still running after an update
        finally:
            l.close()
        for kind in ('html', 'json', 'silent'):                       # a busy port is never taken for ours
            l = Listener(kind)
            try:
                self.assertEqual(rm.probe(l.port, timeout=0.6), 'other', kind)
            finally:
                l.close()


class StandaloneManager(unittest.TestCase):
    def test_nothing_running_starts_and_opens_browser(self):             # case 2
        e = Env()
        port = free_port()
        p = subprocess.Popen([sys.executable, '-I', '-u', os.path.join(MAC, 'receiver_manager.py'), '--servers', e.servers, '--port', str(port), '--open'],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=e.env())
        try:
            self.assertEqual(e.opened(), ['http://127.0.0.1:%d/' % port])
            self.assertEqual(rm.probe(port), 'ours')
        finally:
            p.terminate()
            p.wait(5)

    def test_already_running_is_not_duplicated_and_existing_page_is_opened(self):   # case 3
        e = Env()
        port = free_port()
        radio = Radio(e, port)
        try:
            self.assertEqual(rm.probe(port), 'ours')
            r = run_manager(e, port, '--open')
            self.assertEqual(r.returncode, 0)
            self.assertNotIn('Traceback', r.stdout + r.stderr)
            self.assertIn('already running', r.stdout)
            self.assertEqual(e.opened(), ['http://127.0.0.1:%d/' % port])
            self.assertTrue(radio.state())                                  # the radio is untouched
        finally:
            radio.close()

    def test_port_used_by_another_program_is_explained_not_a_traceback(self):      # case 5
        for kind in ('html', 'silent'):
            e = Env()
            l = Listener(kind)
            try:
                r = run_manager(e, l.port, '--open')
            finally:
                l.close()
            out = r.stdout + r.stderr
            self.assertEqual(r.returncode, 1, kind)
            self.assertNotIn('Traceback', out)
            self.assertIn('lsof', out)
            self.assertNotIn('already running', out)                        # busy port alone does not mean RADIO DAYS is there
            self.assertEqual(e.opened(), [], kind)                          # and the browser is not pointed at the stranger

    def test_browser_failure_does_not_stop_the_manager(self):
        e = Env(browser_exit=1)
        port = free_port()
        p = subprocess.Popen([sys.executable, '-I', '-u', os.path.join(MAC, 'receiver_manager.py'), '--servers', e.servers, '--port', str(port), '--open'],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=e.env())
        try:
            e.opened()
            time.sleep(1)
            self.assertIsNone(p.poll())
            self.assertEqual(rm.probe(port), 'ours')
        finally:
            p.terminate()
            p.wait(5)


class RadioStart(unittest.TestCase):
    def test_open_manager_opens_browser_once_and_radio_survives(self):          # cases 1 and 4
        e = Env()
        port = free_port()
        radio = Radio(e, port, '--open-manager')
        try:
            self.assertEqual(e.opened(), ['http://127.0.0.1:%d/guide.html' % port])
            time.sleep(0.5)
            self.assertEqual(len(e.opened()), 1)
            self.assertEqual(rm.probe(port), 'ours')
            # "closing the browser" = the browser process is gone; nothing in the radio depends on it
            time.sleep(1)
            self.assertIsNone(radio.p.poll())
            self.assertTrue(radio.state())
        finally:
            radio.close()

    def test_without_flag_no_browser(self):
        e = Env()
        radio = Radio(e, free_port())
        try:
            time.sleep(0.5)
            self.assertEqual(e.opened(), [])
        finally:
            radio.close()

    def test_browser_failure_keeps_the_radio_running(self):                       # case 6
        for bexit in (1, None):
            e = Env(browser_exit=bexit or 0)
            if bexit is None:
                e.script = '/nonexistent/browser'                              # a browser that cannot even be started
            radio = Radio(e, free_port(), '--open-manager')
            try:
                time.sleep(1)
                self.assertIsNone(radio.p.poll(), bexit)
                self.assertTrue(radio.state())
                self.assertNotIn('Traceback', radio.out())
            finally:
                radio.close()

    def test_manager_port_used_by_another_program(self):                        # case 5, radio side
        e = Env()
        l = Listener('html')
        radio = Radio(e, l.port, '--open-manager')
        try:
            out = radio.out()
            self.assertIn('NOT available', out)
            self.assertIn('lsof', out)
            self.assertNotIn('Traceback', out)
            self.assertNotIn('Receiver Manager: http://', out)                  # not presented as started
            self.assertEqual(e.opened(), [])
            self.assertTrue(radio.state())                                      # the radio itself runs
        finally:
            radio.close()
            l.close()

    def test_manager_port_held_by_our_own_manager(self):
        e = Env()
        port = free_port()
        store = ss.ServerStore(e.servers, os.path.join(MAC, 'servers.default.json'))
        store.ensure()
        httpd, _ = rm.start(store, None, port)
        radio = Radio(e, port, '--open-manager')
        try:
            out = radio.out()
            self.assertIn('already running', out)
            self.assertNotIn('NOT available', out)
            self.assertEqual(e.opened(), [])  # repeated radio starts must not create duplicate browser tabs
            self.assertTrue(radio.state())
        finally:
            radio.close()
            httpd.shutdown()
            httpd.server_close()

    def test_launch_scripts_pass_the_flag(self):
        root = os.path.dirname(MAC)
        with open(os.path.join(root, 'Start RADIO DAYS Mac.command')) as f:
            self.assertEqual(f.read().count('radiodays_sdr.py --open-manager "$@"'), 2)   # with and without caffeinate
        with open(os.path.join(root, 'Open Receiver Manager Mac.command')) as f:
            self.assertIn('receiver_manager.py --open', f.read())
        with open(os.path.join(root, 'Start RADIO DAYS Win.bat')) as f:
            self.assertIn('radiodays_sdr.py --open-manager %*', f.read())
        with open(os.path.join(root, 'Open Receiver Manager Win.bat')) as f:
            self.assertIn('receiver_manager.py --open %*', f.read())


if __name__ == '__main__':
    unittest.main()
