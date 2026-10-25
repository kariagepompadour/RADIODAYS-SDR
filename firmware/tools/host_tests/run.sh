#!/bin/bash
# Tests the firmware's audio protocol parser and playout rules on a PC (no Arduino needed).
#   ./run.sh [capture.bin]    capture.bin: raw frames from mac/tests/live_audio.py (optional)
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -o /tmp/rdsdr_audio_host test_audio_host.cpp
CAP=${1:-/tmp/rdsdr_synthetic.bin}
if [ ! -f "$CAP" ]; then
  echo "no capture given: making one from the Mac server's test tone"
  (cd ../../../mac && "${PYTHON:-.venv/bin/python}" - "$CAP" <<'PY'
import sys
sys.path.insert(0, 'tests'); sys.path.insert(0, '.')
import test_audio_output as t
e, _ = t.make('cardputer'); s = t.connect(e); assert t.readline(s) == b'OK\n'
open(sys.argv[1], 'wb').write(t.read_exact(s, 1034 * 120)); e.stop()
PY
  )
fi
/tmp/rdsdr_audio_host "$CAP"
