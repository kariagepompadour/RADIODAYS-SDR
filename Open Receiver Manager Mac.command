#!/bin/bash
# Receiver Manager only (without receiving anything): edit mac/servers.json in the browser.
# Opens the page in your browser every time.  You do not need this for normal listening: Start RADIO DAYS Mac.command already opens it.
# Changes are used the next time RADIO DAYS SDR starts.  If a Receiver Manager is already running (e.g. from Start RADIO DAYS Mac.command),
# no second one is started; the existing page is opened instead.
cd "$(dirname "$0")/mac" || exit 1
pause() { echo; echo "Enterキーで閉じます / Press Enter to close"; read -r; }
PY=.venv/bin/python
[ -x "$PY" ] || PY=$(command -v python3) || { echo "python3 not found"; pause; exit 1; }
"$PY" receiver_manager.py --open "$@"
status=$?
pause
exit $status
