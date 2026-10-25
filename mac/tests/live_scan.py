"""Live check (needs internet): AUTO scan on a real KiwiSDR.

  .venv/bin/python tests/live_scan.py SERVER_INDEX START_KHZ STOPS [DIR]
Scans from START in direction DIR (+1/-1) for STOPS stops and prints every stop with the time spent and the RSSI
profile it saw.  Compare with a slow survey (see VALIDATION) to judge the stops.
"""
import os
import sys
import time

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
import radiodays_sdr as r  # noqa: E402

idx, start, stops = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
direction = int(sys.argv[4]) if len(sys.argv) > 4 else 1
r.save_state = lambda e: None
servers = r.load_servers(os.path.join(MAC, 'servers.json'))
e = r.Engine([servers[idx]], r.NullSink(), start_freq=start, volume=0)
trace = []
orig = e._scan_measure
def traced(f, n=r.SCAN_MEASURE_BLOCKS):
    v = orig(f, n)
    if isinstance(v, list):
        trace.append((f, min(v), len(v)))
    return v
e._scan_measure = traced
e.start()
t0 = time.time()
while time.time() - t0 < 60 and e.state()['link'] != 'connected':
    time.sleep(0.2)
print('connected to', servers[idx]['short'], e.state()['link'])
time.sleep(1.5)
total0 = time.time()
for k in range(stops):
    n0 = len(trace); t1 = time.time()
    e.start_scan(direction, 5.0)
    while time.time() - t1 < 900:
        time.sleep(0.2)
        if e.state()['scan'] == 'off':
            break
    st = e.state()
    steps = trace[n0:]
    print('stop %d: %-6s at %8.1f kHz  excess %s dB (floor %s)  %d measurements in %.0f s  (%.2f s each)' % (
        k + 1, st['scan_msg'], st['freq'], st['scan_excess'], st['scan_floor'], len(steps), time.time() - t1,
        (time.time() - t1) / max(1, len(steps))), flush=True)
    print('         ' + ' '.join('%.0f:%.0f' % (f, m) for f, m, n in steps[-12:]), flush=True)
    if st['scan_msg'] != 'signal':
        break
e.stop()
