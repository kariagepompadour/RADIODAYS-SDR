#!/bin/bash
# RADIO DAYS SDR - Mac launcher (double-click in Finder, or run it in Terminal).
# Starts the radio and opens the Receiver Manager (receiver list editor, http://127.0.0.1:8780/) in your browser every time.
# Closing the browser does not stop the radio; Ctrl+C in this window does.
cd "$(dirname "$0")/mac" || exit 1
pause() { echo; echo "Enterキーで閉じます / Press Enter to close"; read -r; }

if [ ! -x .venv/bin/python ]; then
  echo "初回セットアップ中（数分かかることがあります） / First-time setup (may take a few minutes)…"
  PY=$(command -v python3) || { echo "python3 が見つかりません / python3 not found. https://www.python.org/ or 'brew install python'"; pause; exit 1; }
  "$PY" -m venv .venv || { echo "仮想環境の作成に失敗 / could not create the virtual environment"; pause; exit 1; }
  .venv/bin/python -m pip install -q -r requirements.txt || { echo "依存パッケージの導入に失敗（ネット接続を確認） / pip install failed (check the network)"; rm -rf .venv; pause; exit 1; }
fi

if [ ! -d vendor/kiwiclient/kiwi ]; then
  echo "KiwiClient を取得します（固定バージョン・SHA-256 検証付き） / Fetching KiwiClient (pinned version, SHA-256 verified)…"
  .venv/bin/python tools/fetch_kiwiclient.py || { echo "KiwiClient の取得に失敗 / could not fetch KiwiClient"; pause; exit 1; }
fi

echo "RADIO DAYS SDR を起動します。止めるには Ctrl+C。 / Starting. Stop with Ctrl+C."
# caffeinate -i keeps the Mac from idle-sleeping (and stopping reception) only while this program runs.
if command -v caffeinate >/dev/null 2>&1; then
  caffeinate -i .venv/bin/python radiodays_sdr.py --open-manager "$@"
else
  .venv/bin/python radiodays_sdr.py --open-manager "$@"
fi
status=$?
echo "終了しました / Stopped (code $status)"
pause
exit $status
