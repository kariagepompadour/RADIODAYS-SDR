"""Live check (needs internet): real KiwiSDR audio reaches a Cardputer-style client, unscaled and without gaps.

  .venv/bin/python tests/live_audio.py [capture.bin]     (capture.bin: raw frames, for the firmware parser test)
"""
import json
import os
import sys
import time

import numpy as np

MAC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, MAC)
sys.path.insert(0, os.path.join(MAC, 'tests'))
import radiodays_sdr as r  # noqa: E402
import test_audio_output as t  # noqa: E402

servers = r.load_servers(os.path.join(MAC, 'servers.json'))
sink = t.CountingSink()
e = r.Engine(servers, sink, volume=5, fake_source=False)       # volume 5: must not matter for the stream
e.output = 'cardputer'
e.audio = r.AudioServer(e, t.TOKEN, t.free_port())
e.audio.start()
e.start()
ok = True
try:
    t0 = time.time()
    while time.time() - t0 < 60 and e.state()['link'] != 'connected':
        time.sleep(0.3)
    print('Kiwi link:', e.state()['link'], e.state()['server'], 'after %.1fs' % (time.time() - t0))
    s = t.connect(e)
    assert t.readline(s) == b'OK\n'
    raw = b''
    frames = []
    for _ in range(240):                                        # ~10 s
        hdr = t.read_exact(s, t.HDR.size)
        magic, ns, seq, rate = t.HDR.unpack(hdr)
        body = t.read_exact(s, ns * 2)
        raw += hdr + body
        frames.append((seq, rate, np.frombuffer(body, '<i2')))
    seqs = [f[0] for f in frames]
    cont = seqs == list(range(seqs[0], seqs[0] + len(seqs)))
    pcm = np.concatenate([f[2] for f in frames]).astype(np.float32)
    rms = float(np.sqrt(np.mean(pcm ** 2)))
    print('frames %d, continuous %s, rate %s, samples/frame %s, rms %.0f, peak %d, Mac speaker opens %d, plays %d' % (
        len(frames), cont, {f[1] for f in frames}, {len(f[2]) for f in frames}, rms, int(np.max(np.abs(pcm))), sink.opens, sink.plays))
    ok = cont and rms > 50 and sink.opens == 0 and sink.plays == 0
    # switch back to the Mac speaker: the stream ends, the Mac plays again
    e.set_output('mac')
    time.sleep(2)
    print('after switching to mac: client connected=%s, Mac speaker plays=%d' % (e.audio.connected, sink.plays))
    ok = ok and not e.audio.connected and sink.plays > 5
    if len(sys.argv) > 1:
        open(sys.argv[1], 'wb').write(raw)
        print('capture written:', sys.argv[1], len(raw), 'bytes')
finally:
    e.stop()
print('RESULT:', 'PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
