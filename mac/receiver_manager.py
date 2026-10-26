#!/usr/bin/env python3
"""Receiver Manager: choose, order and edit the KiwiSDR receivers (servers.json) in a browser.

  - It is started by radiodays_sdr.py (http://127.0.0.1:8780/), or on its own:  python3 receiver_manager.py
  - It listens on the loopback interface only (127.0.0.1): it is not reachable from other computers.
  - All reading and writing of servers.json goes through servers_store.py (validated, atomic, with backups).
  - When radiodays_sdr.py is running, a saved list is loaded into it at once; the Cardputer shows it the next time
    the globe (G) is opened.

What the "check" buttons really check (they never claim more):
  HTTP check   GET http://host:port/status  - the receiver's web server answers and says what it is doing (status,
               users).  It does NOT prove that audio can be received.
  Audio check  one real, short connection (about 7 s) with this program's own KiwiSDR client - audio blocks arrived,
               or the exact error.  It uses one user slot of that receiver briefly, so it is only run when you press it.
Candidates are a snapshot of a public list: none of them is known to work now.
"""
import argparse
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'vendor', 'kiwiclient'))
import check_history  # noqa: E402
import platform_support  # noqa: E402
import servers_store  # noqa: E402

DEFAULT_PORT = 8780
WEB_DIR = os.path.join(HERE, 'manager')
STATIC = {'/': ('index.html', 'text/html; charset=utf-8'), '/index.html': ('index.html', 'text/html; charset=utf-8'),
          '/app.js': ('app.js', 'text/javascript; charset=utf-8'), '/style.css': ('style.css', 'text/css; charset=utf-8'),
          '/guide': ('radio/index.html', 'text/html; charset=utf-8'), '/guide.html': ('radio/index.html', 'text/html; charset=utf-8'),
          '/radio/app.js': ('radio/app.js', 'text/javascript; charset=utf-8'), '/radio/style.css': ('radio/style.css', 'text/css; charset=utf-8'),
          '/radio/wifi-toggle.js': ('radio/wifi-toggle.js', 'text/javascript; charset=utf-8'),
          '/radio/assets/cardputer-main.png': ('radio/assets/cardputer-main.png', 'image/png'),
          '/guide-details.html': ('guide.html', 'text/html; charset=utf-8'),
          '/guide.js': ('guide.js', 'text/javascript; charset=utf-8'), '/guide.json': ('guide.json', 'application/json; charset=utf-8')}
APP_ID = 'RADIO DAYS SDR Receiver Manager'     # what /api/identify answers: how a second start tells "ours" from some other program on the port
BULK_CONCURRENCY = 4        # at most this many HTTP checks run at once (enforced here; the page's bulk run uses the same number)
MAX_BODY = 256 * 1024
CSRF_HEADER = 'X-RDSDR-Manager'


# --------------------------------------------------------------------------- checks
def _now():
    return check_history.now_iso()


def check_status(host, port, timeout=6.0):
    """HTTP check: does the receiver's web server answer /status?  (Says nothing about audio.)"""
    t0 = time.time()
    res = {'level': 'status', 'host': host, 'port': port, 'checked_at': _now(), 'ok': False}
    try:
        with urllib.request.urlopen('http://%s:%d/status' % (host, port), timeout=timeout) as r:
            body = r.read(16384).decode('utf-8', 'replace')
            res['http'] = r.status
    except urllib.error.HTTPError as e:
        res.update(error='HTTP %d' % e.code, http=e.code)
        return res
    except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError, OSError) as e:
        reason = getattr(e, 'reason', e)
        if isinstance(reason, (socket.timeout, TimeoutError)) or 'timed out' in str(reason):
            res['error'] = 'no answer within %.0f s' % timeout
        elif isinstance(reason, socket.gaierror):
            res['error'] = 'host name not found'
        elif isinstance(reason, ConnectionRefusedError):
            res['error'] = 'connection refused (nothing listens on this port)'
        else:
            res['error'] = '%s' % str(reason)[:120]
        return res
    kv = dict(l.split('=', 1) for l in body.splitlines() if '=' in l and not l.startswith('<'))
    if 'status' not in kv:
        res['error'] = 'answered, but it does not look like a KiwiSDR /status page'
        return res
    res['ok'] = True
    res['ms'] = int((time.time() - t0) * 1000)
    res['status'] = kv.get('status', '')[:20]
    res['offline'] = kv.get('offline', '')[:10]
    res['name'] = kv.get('name', '')[:80]

    def num(k):
        try:
            return int(kv.get(k, ''))
        except ValueError:
            return None
    res['users'], res['users_max'] = num('users'), num('users_max')
    res['available'] = (res['status'] == 'active' and res['offline'] in ('no', ''))
    return res


class Manager:
    def __init__(self, store, engine=None, engine_cls=None, null_sink_cls=None):
        self.store = store
        self.engine = engine
        self.engine_cls = engine_cls
        self.null_sink_cls = null_sink_cls
        self.audio_lock = threading.Lock()
        self.http_slots = threading.BoundedSemaphore(BULK_CONCURRENCY)
        # the history lives next to servers.json (so a test or second setup with its own servers file has its own history)
        self.history = check_history.CheckHistory(os.path.join(os.path.dirname(os.path.abspath(store.path)), 'check_history.json'))

    def run_check(self, level, host, port):
        """Run one check and remember the result.  Whatever was really observed is stored; a check that could not be run
        (another audio check in progress, KiwiClient missing) is returned to the page but not stored."""
        if level == 'audio':
            pw = ''
            for c in self._servers_for_ui():                       # use the stored password of a registered receiver
                if (c['host'].lower(), c['port']) == (host.lower(), port):
                    pw = c.get('password', '')
            res = self.check_audio(host, port, pw)
        else:
            level = 'status'
            with self.http_slots:                                   # never more than BULK_CONCURRENCY at once, whoever asks
                res = check_status(host, port)
        if not res.pop('not_run', False):
            res['recorded'] = self.history.record(host, port, level, res)
        return res

    # ---- the audio check: one real short connection
    def check_audio(self, host, port, password='', seconds=7.0):
        res = {'level': 'audio', 'host': host, 'port': port, 'checked_at': _now(), 'ok': False}
        eng = self.engine
        if eng is not None:                          # already listening to it right now: that IS the proof, and a 2nd connection would be refused
            try:
                cur = eng.servers[eng.server_index]
                st = eng.state()
                if (cur['host'].lower(), cur['port']) == (host.lower(), port) and st['link'] == 'connected' and st['blocks'] > 0:
                    res.update(ok=True, in_use=True, blocks=st['blocks'], sample_rate=st['sample_rate'], elapsed=0.0)
                    return res
            except Exception:
                pass
        if not self.audio_lock.acquire(blocking=False):
            res['error'] = 'another audio check is still running'
            res['not_run'] = True
            return res
        try:
            if self.engine_cls is None:
                import radiodays_sdr as r
                self.engine_cls, self.null_sink_cls = r.Engine, r.NullSink
            srv = [{'name': 'check', 'short': 'check', 'host': host, 'port': port, 'password': password or '', 'lat': None, 'lon': None}]
            e = self.engine_cls(srv, self.null_sink_cls(), volume=0)
            e.connect_timeout = max(8.0, seconds + 2)
            t0 = time.time()
            e.start()
            try:
                while time.time() - t0 < seconds + 8:
                    st = e.state()
                    if st['link'] == 'connected' and st['blocks'] >= 3:
                        res.update(ok=True, blocks=st['blocks'], sample_rate=st['sample_rate'], rssi=st['rssi'])
                        break
                    if st['link'] == 'error' and st['error']:
                        res['error'] = st['error']
                        break
                    time.sleep(0.15)
                else:
                    res['error'] = 'no audio within %.0f s' % (seconds + 8)
            finally:
                e.stop()
            res['elapsed'] = round(time.time() - t0, 1)
            return res
        except Exception as ex:                      # e.g. the KiwiClient library is not installed
            res['error'] = '%s: %s' % (type(ex).__name__, ex)
            res['not_run'] = True
            return res
        finally:
            self.audio_lock.release()

    # ---- the state shown in the browser
    def state(self):
        d = {'servers': [servers_store.to_file_form(c) | {'password_set': bool(c.get('password'))} for c in self._servers_for_ui()],
             'file': self.store.info(), 'limits': {'max': servers_store.MAX_SERVERS, 'name_max': servers_store.NAME_MAX,
                                                    'short_max': servers_store.SHORT_MAX},
             'running': self.engine is not None}
        for s in d['servers']:                       # the password itself never goes to the browser
            s.pop('password', None)
        if self.engine is not None:
            try:
                cur = self.engine.servers[self.engine.server_index]
                d['current'] = {'host': cur['host'], 'port': cur['port'], 'link': self.engine.link, 'index': self.engine.server_index}
            except Exception:
                pass
        return d

    def _servers_for_ui(self):
        try:
            return self.store.load()
        except servers_store.ServersError:
            servers, _ = self.store.load_with_fallback()
            return servers

    def radio_state(self):
        """Live state for the interactive Operation Guide. The Engine remains the single source of truth."""
        servers = [{'index': i, 'short': s['short'], 'name': s['name'],
                    'lat': s.get('lat'), 'lon': s.get('lon'),
                    'has_position': s.get('lat') is not None and s.get('lon') is not None}
                   for i, s in enumerate(self._servers_for_ui())]
        if self.engine is None:
            return {'running': False, 'freq': 7100.0, 'mode': 'am', 'volume': 70, 'muted': False,
                    'scan': 'off', 'output': 'mac', 'link': 'off', 's': 0, 'server_index': 0, 'servers': servers}
        state = self.engine.state()
        state['running'] = True
        state['servers'] = servers
        return state

    def radio_action(self, data):
        """Apply a browser control through the same Engine methods used by the Cardputer API."""
        if self.engine is None:
            raise ValueError('RADIO DAYS SDR is not running; start it to enable live controls')
        action, value = str(data.get('action', '')), data.get('value')
        if action == 'step':
            step = float(data.get('step', 5))
            if step not in (0.1, 1.0, 5.0, 10.0) or int(value) not in (-1, 1):
                raise ValueError('invalid tuning step')
            self.engine.set_freq(self.engine.freq + int(value) * step)
        elif action == 'frequency':
            freq = float(value)
            if not 10 <= freq <= 30000:
                raise ValueError('frequency must be 10–30000 kHz')
            self.engine.set_freq(freq)
        elif action == 'mode':
            self.engine.set_mode(str(value).lower())
        elif action == 'volume-delta':
            self.engine.set_volume(self.engine.volume + int(value))
        elif action == 'mute':
            self.engine.set_muted(not self.engine.muted)
        elif action == 'scan':
            self.engine.start_scan(1 if int(value or 1) >= 0 else -1, float(data.get('step', 5)))
        elif action == 'scan-stop':
            self.engine.stop_scan()
        elif action == 'server':
            self.engine.switch_server(int(value))
        elif action == 'server-delta':
            self.engine.switch_server((self.engine.server_index + int(value)) % len(self.engine.servers))
        elif action == 'output':
            self.engine.set_output('cardputer' if self.engine.output == 'mac' else 'mac')
        elif action in ('wifi', 'wifi-toggle', 'mac-ip', 'help', 'globe', 'diagnostics', 'radio'):
            self.engine.request_card_command(action)
        else:
            raise ValueError('unknown radio action')
        return self.radio_state()

    def save(self, servers, base_mtime=None, force=False):
        """Validate + save + load into the running program.  Returns the report for the browser."""
        existing_pw = {}
        try:
            for c in self.store.load():
                existing_pw[(c['host'].lower(), c['port'])] = c.get('password', '')
        except servers_store.ServersError:
            pass
        incoming = []
        for s in servers:                            # the browser never sees passwords: keep the stored one unless a new one was typed
            s = dict(s)
            if s.pop('keep_password', False) and not s.get('password'):
                s['password'] = existing_pw.get((str(s.get('host', '')).lower(), int(s.get('port', 8073) or 8073)), '')
            incoming.append(s)
        cleaned = servers_store.clean_servers(incoming)
        info = self.store.info()
        if base_mtime is not None and info['mtime'] is not None and not force and abs(info['mtime'] - base_mtime) > 0.5:
            raise ConflictError('servers.json was changed outside this page after you opened it')
        res = self.store.save(cleaned)
        report = {'ok': True, 'count': res['count'], 'backup': bool(res['backup']), 'corrupt_copy': bool(res['corrupt_copy']),
                  'reloaded': False, 'switched_to_first': False, 'warnings': []}
        if res['corrupt_copy']:
            report['warnings'].append('the previous servers.json was damaged; a copy was kept next to it (servers.json.corrupt-...)')
        if self.engine is not None:
            try:
                rr = self.engine.reload_servers(cleaned)
                report['reloaded'] = True
                report['switched_to_first'] = rr['switched_to_first']
                if rr['switched_to_first']:
                    report['warnings'].append('the receiver you were listening to is no longer in the list; the first receiver was connected instead')
            except Exception as ex:
                report['warnings'].append('saved, but could not be loaded into the running program: %s' % ex)
        report['file'] = self.store.info()
        return report

    def restore(self):
        res, servers = self.store.restore_backup()
        report = {'ok': True, 'count': len(servers), 'reloaded': False, 'switched_to_first': False, 'warnings': []}
        if self.engine is not None:
            try:
                rr = self.engine.reload_servers(servers)
                report['reloaded'] = True
                report['switched_to_first'] = rr['switched_to_first']
            except Exception as ex:
                report['warnings'].append('restored, but could not be loaded into the running program: %s' % ex)
        report['file'] = self.store.info()
        return report


class ConflictError(Exception):
    pass


# --------------------------------------------------------------------------- HTTP
def make_handler(mgr, port):
    allowed_hosts = {'127.0.0.1:%d' % port, 'localhost:%d' % port, '[::1]:%d' % port}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.0'
        server_version = 'RDSDRManager'

        def log_message(self, fmt, *args):
            pass

        def _send(self, code, body, ctype='application/json; charset=utf-8'):
            if not isinstance(body, bytes):
                body = json.dumps(body, ensure_ascii=False).encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('X-Frame-Options', 'DENY')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', "default-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; form-action 'none'")
            self.end_headers()
            self.wfile.write(body)

        def _host_ok(self):
            if self.headers.get('Host', '') not in allowed_hosts:          # blocks DNS-rebinding
                self._send(403, {'ok': False, 'error': 'forbidden host'})
                return False
            return True

        def do_GET(self):
            if not self._host_ok():
                return
            path = self.path.split('?', 1)[0]
            if path in STATIC:
                fname, ctype = STATIC[path]
                try:
                    with open(os.path.join(WEB_DIR, fname), 'rb') as f:
                        return self._send(200, f.read(), ctype)
                except OSError:
                    return self._send(404, {'ok': False, 'error': 'missing ' + fname})
            if path == '/api/state':
                return self._send(200, mgr.state())
            if path == '/api/radio/state':
                return self._send(200, mgr.radio_state())
            if path == '/api/identify':
                return self._send(200, {'app': APP_ID, 'running': mgr.engine is not None})
            if path == '/api/check-history':
                return self._send(200, {'entries': mgr.history.snapshot(), 'bulk_concurrency': BULK_CONCURRENCY, 'note': mgr.history.note})
            if path == '/api/candidates':
                try:
                    with open(os.path.join(WEB_DIR, 'receiver_candidates.json'), 'rb') as f:
                        return self._send(200, f.read())
                except OSError:
                    return self._send(200, {'receivers': [], 'note': 'receiver_candidates.json is missing'})
            return self._send(404, {'ok': False, 'error': 'not found'})

        def do_POST(self):
            if not self._host_ok():
                return
            origin = self.headers.get('Origin')
            if origin is not None and origin.replace('http://', '', 1) not in allowed_hosts:   # blocks cross-site requests
                return self._send(403, {'ok': False, 'error': 'forbidden origin'})
            if self.headers.get(CSRF_HEADER) != '1' or 'application/json' not in self.headers.get('Content-Type', ''):
                return self._send(403, {'ok': False, 'error': 'forbidden'})
            try:
                n = int(self.headers.get('Content-Length', '0'))
            except ValueError:
                n = -1
            if n < 0 or n > MAX_BODY:
                return self._send(413, {'ok': False, 'error': 'request too large'})
            try:
                data = json.loads(self.rfile.read(n) or b'{}')
                if not isinstance(data, dict):
                    raise ValueError('JSON object expected')
            except ValueError as e:
                return self._send(400, {'ok': False, 'error': 'bad JSON: %s' % e})
            path = self.path.split('?', 1)[0]
            try:
                if path == '/api/save':
                    return self._send(200, mgr.save(data.get('servers'), data.get('base_mtime'), bool(data.get('force'))))
                if path == '/api/restore':
                    return self._send(200, mgr.restore())
                if path == '/api/check':
                    host, port_ = str(data.get('host', '')).strip(), data.get('port', 8073)
                    servers_store.clean_server({'host': host, 'port': port_, 'name': host}, 0)   # same host/port rules as the list
                    return self._send(200, mgr.run_check('audio' if data.get('level') == 'audio' else 'status', host, int(port_)))
                if path == '/api/radio/action':
                    return self._send(200, mgr.radio_action(data))
            except ConflictError as e:
                return self._send(409, {'ok': False, 'code': 'conflict', 'error': str(e)})
            except servers_store.ServersError as e:
                return self._send(400, {'ok': False, 'error': str(e)})
            except Exception as e:                                         # never let a bug kill the connection silently
                return self._send(500, {'ok': False, 'error': '%s: %s' % (type(e).__name__, e)})
            return self._send(404, {'ok': False, 'error': 'not found'})

    return Handler


def probe(port, timeout=2.0):
    """Who is on 127.0.0.1:port?  'free' (connection refused), 'ours' (answers /api/identify as this Receiver Manager)
    or 'other' (anything else that answers or hangs).  An occupied port alone never counts as "ours"."""
    def get(path):
        with urllib.request.urlopen('http://127.0.0.1:%d%s' % (port, path), timeout=timeout) as r:
            return json.loads(r.read(65536).decode('utf-8', 'replace'))
    try:
        d = get('/api/identify')
        return 'ours' if isinstance(d, dict) and d.get('app') == APP_ID else 'other'
    except urllib.error.HTTPError as e:
        if e.code != 404:
            return 'other'
        try:                                           # a Receiver Manager from before /api/identify existed (still running after an update)
            d = get('/api/state')
            if isinstance(d, dict) and {'servers', 'file', 'limits', 'running'} <= set(d):
                return 'ours'
        except (ValueError, OSError):
            pass
        return 'other'
    except urllib.error.URLError as e:
        return 'free' if isinstance(e.reason, ConnectionRefusedError) else 'other'
    except (ValueError, OSError):                      # not JSON, timeout, reset ...
        return 'other'


def port_busy_text(port):
    hint = platform_support.port_check_hint(port)
    return ('ポート %d は RADIO DAYS 以外のプログラムが使っているようです。Receiver Manager は起動できませんでした。\n'
            '  確認: %s  （使っているプログラムが分かります。不要なら終了してください）\n'
            '  または別のポートで: --manager-port / --port に別の番号を指定（その場合のアドレスは http://127.0.0.1:<番号>/）\n'
            'Port %d is used by a program that is not RADIO DAYS, so the Receiver Manager could not start.\n'
            '  Check: %s  (shows which program; quit it if you do not need it)\n'
            '  Or use another port with --manager-port / --port (then the address is http://127.0.0.1:<number>/)') % (port, hint, port, hint)


def open_in_browser(url):
    """Open the page in the default browser.  Never raises; returns False (and says what to do) if it did not work."""
    try:
        import webbrowser
        if webbrowser.open(url):
            return True
    except Exception:
        pass
    print('ブラウザを自動で開けませんでした。次のアドレスを手動で開いてください / Could not open the browser. Open this address yourself: %s' % url)
    return False


def start(store, engine=None, port=DEFAULT_PORT, engine_cls=None, null_sink_cls=None):
    """Start the manager in a thread.  Returns (httpd, manager)."""
    mgr = Manager(store, engine, engine_cls, null_sink_cls)
    httpd = platform_support.LocalHTTPServer(('127.0.0.1', port), make_handler(mgr, port))
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, mgr


def main():
    ap = argparse.ArgumentParser(description='RADIO DAYS SDR - Receiver Manager (stand-alone)')
    ap.add_argument('--servers', default=os.path.join(HERE, 'servers.json'))
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--open', action='store_true', help='open the page in the default browser (also when one is already running)')
    args = ap.parse_args()
    store = servers_store.ServerStore(args.servers, default_path=os.path.join(HERE, 'servers.default.json'))
    _, msg = store.load_with_fallback()
    if msg:
        print(msg)
    url = 'http://127.0.0.1:%d/' % args.port
    try:
        httpd, _ = start(store, None, args.port)
    except OSError as e:
        who = probe(args.port)                          # the port being busy does not by itself mean it is ours
        if who == 'ours':
            print('Receiver Manager はすでに起動しています。二重には起動せず、既存の画面を開きます。 / Receiver Manager is already running; not starting a second one.')
            print('  %s' % url)
            if args.open:
                open_in_browser(url)
            return 0
        print(port_busy_text(args.port) if who == 'other' else 'Receiver Manager を起動できませんでした / Could not start the Receiver Manager: %s' % e)
        return 1
    print('Receiver Manager: %s  (stand-alone: changes are saved to %s; start RADIO DAYS SDR to use them)' % (url, store.path))
    if args.open:
        open_in_browser(url)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
