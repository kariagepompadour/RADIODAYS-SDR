"""Live check (needs internet; not part of unittest discovery): does the globe's request path match the Mac's real state?

Uses the same HTTP calls as the firmware: /servers, /server?index=N, /state.
  .venv/bin/python tests/live_switch.py
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT, TOKEN = 18777, 'LIVE'


def get(path):
    with urllib.request.urlopen('http://127.0.0.1:%d%s%stoken=%s' % (PORT, path, '&' if '?' in path else '?', TOKEN), timeout=3) as r:
        return json.loads(r.read())


p = subprocess.Popen([sys.executable, '-I', os.path.join(MAC, 'radiodays_sdr.py'), '--no-audio', '--token', TOKEN,
                      '--port', str(PORT), '--discovery-port', str(PORT + 1000), '--volume', '0'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, HOME='/tmp/radiodays_live_home'))
ok = True
try:
    for _ in range(50):
        try:
            get('/state'); break
        except Exception:
            time.sleep(0.2)
    servers = get('/servers')['servers']
    for s in servers:
        print('  list: %d %-14s lat=%s lon=%s' % (s['index'], s['short'], s['lat'], s['lon']))
    for target in (1, 2, 3, 0, 2):
        t0 = time.time()
        st = get('/server?index=%d' % target)
        first = (st['server_index'], st['link'])
        trail, last = [], None
        while time.time() - t0 < 60:
            st = get('/state')
            cur = (st['server_index'], st['link'], st['error'][:40])
            if cur != last:
                trail.append('%.1fs:%d/%s%s' % (time.time() - t0, cur[0], cur[1], ' (' + cur[2] + ')' if cur[2] else ''))
                last = cur
            if st['link'] == 'connected' and st['blocks'] > 5:
                break
            time.sleep(0.2)
        print('      ', ' -> '.join(trail))
        switched = st['server_index'] != target
        # the request must be visible at once; afterwards either the requested receiver is on air, or the Mac
        # reports an automatic switch and the receiver it moved to is really on air (blocks arriving)
        good = first == (target, 'connecting') and st['link'] == 'connected' and st['blocks'] > 5
        ok &= good
        print('%s request %d %-14s: immediately %s -> %s%s after %.1fs (rssi %s, blocks %d)' % (
            'OK  ' if good else 'FAIL', target, servers[target]['short'], first, st['link'],
            ' on %s (AUTO-SWITCHED)' % st['server'] if switched else '', time.time() - t0, st['rssi'], st['blocks']))
finally:
    p.terminate()
print('RESULT:', 'PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
