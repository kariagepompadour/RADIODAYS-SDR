# RADIO DAYS SDR

**English | [日本語](README_JA.md)**

> **Turn the dial. Someone out there is still awake.**

Everything is instant now. Anything we want is only a click away.
That is exactly why we want you to pick up a slightly inconvenient radio.

Turn the dial late at night. Search through the static for a distant voice.
Find a broadcast you were not looking for, let the casual voice of a radio host make the night feel a little less lonely, and stay up just a little longer.
This is for those who remember waiting for a QSL card—and for those who never had the chance.

**RADIO DAYS SDR** imagines a classic silver-face analog tuner brought back to life with ESP32 and KiwiSDR.
Not as a museum piece, but as a radio ready to hear today's world.

Listen to shortwave through public [KiwiSDR](http://kiwisdr.com/) receivers around the world, with an
**M5Stack Cardputer ADV** (plus the **U135 rotary encoder**) as the remote control and a **Mac or a Windows 11 PC** as the engine.
Includes a browser-based **Receiver Manager** to choose, order and edit the receivers shown on the Cardputer's globe.

The browser **Operation Guide is also a live PC control panel**. It mirrors the Cardputer display and controls the same
Python radio engine, so RADIO DAYS SDR can be tried with a PC and browser before buying or connecting a Cardputer.
When a Cardputer is connected, hardware and browser controls remain synchronized.

**The Web interface does not replace the Cardputer.** It is another input and display for the same PC radio engine: useful
for trying RADIO DAYS SDR, setup and secondary control. The intended experience is still physical—holding a tiny computer,
turning its dial and hearing a distant station from that little device. That is the point. The Web interface is served by
the Python program on a Mac or Windows PC; it is not a stand-alone hosted page and is not intended for phone-only use.

## Before you start: a PC with Python is required

RADIO DAYS SDR **does not work on the Cardputer ADV alone.** The radio itself is a Python program on your PC (Mac or Windows 11):
it connects to the KiwiSDR, decodes the sound and serves the Receiver Manager and Web Controller. The Cardputer is its remote control
and display. This is the same with **USB** and with **Wi-Fi**: Wi-Fi mode links the Cardputer to the PC program over your home network;
it is not a stand-alone mode.

The PC therefore needs **Python 3.9 or newer**, installed **before** the first start. The start files install everything else, but not Python itself.
If you are installing Python now, take a current release such as **3.12 or 3.13** from python.org (tested: 3.12 on Windows 11, 3.15 on a Mac).

| | Check whether Python is installed | If it is missing |
|---|---|---|
| Windows 11 | in Command Prompt: `py --version` (or `python --version`) | install it from <https://www.python.org/downloads/windows/> and tick **"Add python.exe to PATH"** in the installer |
| macOS | in Terminal: `python3 --version` | install it from <https://www.python.org/downloads/macos/> (or `brew install python`) |

If the command is not found, opens the Microsoft Store, offers to install the macOS developer tools, or shows a version older than 3.9,
install Python first and then double-click the start file again.

> **Status: hobby project. The v1.10 GUI has been flashed to a real Cardputer ADV and its appearance and main controls were approved by the author on 2026-10-25. The final OS-neutral public build passed the full PC-side regression suite and was installed on the device through Launcher (Wi-Fi) on 2026-10-25, where the knob tuning, volume, MANUAL/AUTO, globe screen and a 30-minute OUT ADV playback were confirmed by the author (by ear, not measured).**
> **SDR-1.28 uses TinyUSB for the Cardputer ADV cable link. Real-device testing confirmed USB control and OUT ADV operation with Cardputer Wi-Fi disabled. Wi-Fi can be temporarily disabled without erasing saved networks, and the Web Controller synchronizes G/D screens in both directions.**
> **Windows 11 has been tried on one PC** (Windows 11, Python 3.12.10, with a real Cardputer ADV): start, Receiver Manager, Cardputer link, volume/mute/output switching and shutdown worked. Other Windows PCs and long runs are untested.
> See [What is verified and what is not](#what-is-verified-and-what-is-not) before relying on anything. Linux is not supported or tested.

![main screen (simulation)](docs/preview/s01_normal_outpc_manual.png)

*The picture is a PC-side rendering of the firmware's drawing code, not a photo of the device.*

## Who does what

```
Cardputer ADV ──USB serial (control), or Wi-Fi (control + OUT ADV audio)──▶ Mac / Windows PC ──WebSocket──▶ KiwiSDR
 knob, keys, screen                                              decoding, volume, AUTO scan, sound out
                                                                  Web radio / Receiver Manager (PC browser, 127.0.0.1:8780)
```

| | Cardputer ADV | PC (Mac or Windows 11) |
|---|---|---|
| Role | Remote control and display: frequency, mode, volume, globe receiver chooser, status lamps. Optionally plays the sound on its own speaker (`O` key). | Does the real work: connects to the KiwiSDR, decodes audio, plays it (or streams it to the Cardputer), runs AUTO scan, keeps the receiver list `mac/servers.json`, and serves the live Web radio and Receiver Manager. |
| Software | `firmware/RadioDays_SDR/RadioDays_SDR.ino` (+ prebuilt `firmware/bin/`) | `mac/radiodays_sdr.py` (the folder is called `mac` for historical reasons; it is shared by both systems), started by the start file of your OS (below) |

The white panel at the upper right has five rows: output, **WiFi / PC / Kiwi** link health, MANUAL/AUTO, AM/USB/LSB, and the 0.1 / 1 / 5 / 10 kHz tuning increment. The three link lamps use green/yellow/red; amber means selected in the other rows.

## Requirements

* **A PC: macOS** or **Windows 11**, with **Python 3.9 or newer** (how to check and install: [Before you start](#before-you-start-a-pc-with-python-is-required)) and an internet connection for the first start. The PC program is needed with both USB and Wi-Fi.
  The start file installs the rest (`numpy`, `soundcard` and its `cffi`; the KiwiClient library) into `mac/.venv` by itself. Nothing else is needed.
* M5Stack **Cardputer ADV** (+ U135 encoder). USB alone supports both **OUT PC** and **OUT ADV**, so Cardputer Wi-Fi is optional. When using Wi-Fi instead of USB, keep the Cardputer and PC on the same LAN; guest Wi-Fi with client isolation will not work.
* To rebuild the firmware yourself: `arduino-cli`, the `m5stack:esp32` core 3.3.10, libraries M5Cardputer, M5Unified, M5GFX, ArduinoJson 7 (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

## Which file do I double-click?

Download this repository (or `git clone` it) and keep the whole folder together. In the top folder there are exactly four start files, **named after the OS they are for**:

| | macOS | Windows 11 |
|---|---|---|
| **Listen** (starts the radio and opens the Web Controller in your browser) | `Start RADIO DAYS Mac.command` | `Start RADIO DAYS Win.bat` |
| Only edit the receiver list (the radio is **not** started) | `Open Receiver Manager Mac.command` | `Open Receiver Manager Win.bat` |

Normal use: **double-click only the first row for your OS.** The second row is for editing the list without the radio; you never need both.
Everything else (the program in `mac/`, the Receiver Manager page, `servers.json`, the check history, the Operation Guide) is **the same on both systems**.

### macOS
1. Double-click **`Start RADIO DAYS Mac.command`** (or run it in Terminal).
   * First run only: it creates `mac/.venv`, installs the packages from `mac/requirements.txt` and downloads the KiwiClient library
     (one fixed commit, SHA-256 verified; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for why it is not bundled).
   * If macOS says the developer cannot be verified: right-click → Open, or `xattr -dr com.apple.quarantine <this folder>`.
   * Allow the "local network" / firewall dialogs for Python; the Cardputer needs them.
2. The Terminal prints `Mac IP …`, and **your browser opens the Receiver Manager by itself** (every start; see below). Closing the browser does not stop the radio. Stop the radio with Ctrl+C in the Terminal window.

Sound goes to the Mac's default output. Another device: `"./Start RADIO DAYS Mac.command" --sound-device "part of the name"` (list: `--list-devices`).

### Windows 11 (tried on one PC)
1. Install **Python 3.12 or 3.13** from python.org (tick *Add python.exe to PATH*). Python 3.9 or newer is required.
2. Double-click **`Start RADIO DAYS Win.bat`**. A black window opens.
   * First run only: it creates `mac\.venv`, installs the packages and downloads the KiwiClient library (needs the internet; a few minutes).
   * If Windows SmartScreen says "Windows protected your PC": *More info → Run anyway* (the file is plain text; you can read it first).
   * When the Windows firewall asks about Python, allow **Private networks** (the Cardputer needs it).
3. The window prints the PC's IP address, and **your browser opens the Receiver Manager by itself**. Closing the browser does not stop the radio. Stop the radio by pressing Ctrl+C in the black window or closing it.
4. The black window's messages from the start file are in English on purpose; the program's own messages are Japanese and English.

Sound goes to the default Windows playback device (Cardputer-speaker output does not use the PC speaker). Another device: `"Start RADIO DAYS Win.bat" --sound-device "part of the name"` (list: `--list-devices`).
Windows is kept awake while the radio runs.

## Install on the Cardputer ADV

**The Cardputer ADV cannot receive SDR broadcasts by itself.** Listening requires
the RADIO DAYS SDR program (including the Receiver Manager) running on a Mac or
Windows PC, an internet connection from that computer to a KiwiSDR receiver,
and either a USB connection or a LAN connection between the Cardputer and computer. USB alone supports controls, the globe receiver list, location selection, OUT PC and OUT ADV audio. Cardputer Wi-Fi is an optional alternate path.

### Install with M5Burner

1. Open M5Burner and search for **`RADIO DAYS SDR`**.
2. Select **RADIO DAYS SDR** and use M5Burner's installation controls to install
   it on the Cardputer ADV.

Installing RADIO DAYS SDR may overwrite Launcher or other firmware already
installed on the Cardputer ADV. Before installing, make sure you can restore
anything you want to keep.

### Manual installation

Prebuilt images are in `firmware/bin/` (checksums in `SHA256SUMS.txt`). Board settings used: M5Cardputer, flash 8 MB, partition scheme `default_8MB`.

```bash
pip install esptool
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0 firmware/bin/RadioDays_SDR.ino.merged.bin
```

If writing fails, hold the side button (G0) while powering on (download mode). **This replaces whatever firmware is on the device** (e.g. the internet-radio RADIO DAYS).
To build from source instead: `firmware/build.sh` (needs the tools listed above). The `.ino` and the `.bin` files in this repository belong together (same source, version tag `SDR-1.28`).

**OUT PC and OUT ADV over USB:** start RADIO DAYS on the PC, connect USB, and then power on/start the Cardputer app. USB control and audio are detected automatically and no Cardputer Wi-Fi setup is needed. On the tested Cardputer ADV, unplugging and reinserting USB while the app remains running may not restore the cable link; if `CABLE ok` does not return, leave USB connected and restart/power-cycle the Cardputer. **Wi-Fi operation:** press `W` to rescan, choose a network, and enter its password. Credentials are saved only after a successful connection, for up to three networks. Press `S` in Wi-Fi setup to view saved networks and `DEL` to remove one. Press `X` to temporarily turn Wi-Fi OFF/ON without deleting any saved credentials. When USB is not used, keep the PC and Cardputer on the same LAN; automatic UDP discovery is used, with `I` available for manual PC IP entry.
The clock is synchronized from the PC over USB as well as from NTP over Wi-Fi. The best known time is saved every ten minutes and restored offline; if the hardware RTC is available it is also updated. Without a working hardware RTC, time spent fully powered off cannot be measured, so the saved fallback resumes from its last checkpoint.
**About the Cardputer display:** v1.10 puts low, wide SIGNAL/VOLUME analogue meters at upper left and a top-aligned five-row status panel at upper right, with a raised full-width tuning scale below. The five rows have equal height, pale-grey separators and M5Stack-inspired colour-coded labels; link lamps keep their green/amber/red status meaning. Instrument edges are only one pixel thick, with grey top/left inner shadows to look recessed. At the bottom, separate radio-information and digital-watch-style JST LCD modules use black/fluorescent-green glass and equal-height metal rails. Visible slotted screws sit outside the glass—four above and below the wide module, two above and below the clock. Both the tuned frequency and time use large, bold bitmap digits; `kHz` and the status line remain smaller ordinary text for clarity. Changing mode selects a recommended increment: AM=5 kHz, USB/LSB=1 kHz. The globe satellite still floats and talks with the Earth as in the first RADIO DAYS. Cardputer connection notices use the OS-neutral term `PC`.
The five status rows have equal height and pale-grey separators. Both lower LCDs have equal-height silver rails above and below their glass.
The live Web radio opens in **English** for international use. The detailed guide retains its **日本語 / EN** switch and remembers the choice in that browser.

Key reference: the **Operation Guide** ([English](docs/QUICK_GUIDE_EN.md) / [日本語](docs/QUICK_GUIDE_JA.md), also opened from the Receiver Manager page) and the detailed [docs/OPERATION_JA.md](docs/OPERATION_JA.md) (Japanese). The short version: knob = tune, knob+turn = volume, `G` globe, `Tab`/`T` MANUAL⇄AUTO, `O` sound output, `D` diagnostics, `H` help.

## Receiver Manager (choose and order the receivers)

**The "Start RADIO DAYS" file (Mac: `Start RADIO DAYS Mac.command`, Windows: `Start RADIO DAYS Win.bat`) opens the Web Controller in your default browser every time it starts** (address: **<http://127.0.0.1:8780/guide.html>**; you can also type it yourself while the radio runs). Closing the browser tab does not stop the radio; if the browser cannot be opened, the window prints the address and the radio keeps running.
For normal listening you start **only the "Start RADIO DAYS" file**. The "Open Receiver Manager" file (`... Mac.command` / `... Win.bat`) is for editing the list **without starting the radio** (it also opens the browser each time); if a Receiver Manager is already running, it does not start a second one and opens the existing page instead.
If port 8780 is used by another program, the window says so (with how to find it: `lsof` on Mac, `netstat` on Windows; or use `--manager-port` / `--port` with another number); it is only treated as RADIO DAYS' own Receiver Manager when the program on the port identifies itself as such.
The page is served by the PC-side program, only to this computer (`127.0.0.1`), and reads/writes `mac/servers.json` through it.

* **Registered receivers** (left): drag `≡` or use ↑↓ to reorder, `×` to unregister, edit name / short name / host / port / latitude / longitude in place, password under "details". The order is the order on the Cardputer's globe list (max 32).
* **Candidates** (right): about 120 public receivers (25 in Japan) from the public KiwiSDR directory. Tick to register, untick to unregister. Search, "Japan only", country and registered/unregistered filters.
* **Add by hand**: host name and port (e.g. your own KiwiSDR).
* **Save and apply**: writes `mac/servers.json` and loads it into the running program at once. On the Cardputer, press `G` again to see the new list. If the receiver in use was removed, the program switches to the first one.
* **Checks** (buttons per receiver; each result says exactly what was done):
  * *HTTP check*: asks `http://host:port/status` once. It only shows that the receiver's web server answered. **It does not show that audio can be received.**
  * *Audio check (~7 s)*: really connects, and reports whether audio blocks arrived. It uses one user slot of that receiver briefly; use it sparingly.
  * **Check history**: the last HTTP check and the last audio check of each receiver are stored **separately, with date and time**, in `mac/check_history.json` (next to `servers.json`, not inside it, so the list format is untouched; git-ignored). They are shown again after closing the browser or restarting the program, and as "not checked" when there is none. Each result is the state *at that time*: "answers" is not a promise that it works now. Opening the page asks no receiver.
  * **Bulk HTTP re-check** (only when you press "Re-check registered (HTTP)" or "Check shown candidates (HTTP)"): asks for confirmation with the number of receivers, runs at most 4 requests at a time with short gaps, can be stopped, and the program itself also never runs more than 4 HTTP checks at once. There is no bulk audio check.
  * Candidates are **never presented as working**: the list is a snapshot of the public directory (its date is shown on the page and stored in `mac/manager/receiver_candidates.json`). Proxy-hosted receivers (`*.proxy.kiwisdr.com`) are listed with port 8073, which is an assumption and is marked "port assumed".
  * Positions of candidates come from what each receiver reports about itself; please verify them. Receivers without a position are shown as "position unverified" and get no dot on the globe.
* **Live Operation Guide / Web radio**: the button at the top opens the Cardputer-style live control panel. It mirrors the radio state, accepts keyboard, button and JOG input, includes the globe receiver chooser and works without a Cardputer while the Python radio engine is running. **Detailed Guide** opens the former bilingual reference; its text source remains `mac/manager/guide.json`, from which `docs/QUICK_GUIDE_*.md` are generated.
* **Safety of saving**: input is validated first (host, port 1-65535, ASCII names ≤ 43 / 20 chars, latitude/longitude, no duplicates, 1-32 entries). The new file is written to a temporary file, flushed, read back and compared, and only then swapped in atomically. The previous good file is kept as `mac/servers.json.bak` and in `mac/.servers_backups/` (last 10). A damaged `servers.json` is kept as `servers.json.corrupt-<time>` and the program starts from the last good copy. **"Restore previous save"** swaps back. If the file was changed elsewhere after the page was opened, saving asks before overwriting.
* **Security**: the page listens on `127.0.0.1` only; requests need the right `Host`, no foreign `Origin`, a custom header and a JSON body; passwords are never sent to the browser (it only learns "password set"). `servers.json` is saved with mode 600.

`mac/servers.json` is created from `mac/servers.default.json` on first start and is ignored by git. Format (you may also edit it by hand while the program is stopped):

```json
{"name": "Osaka, Japan (label)", "short": "Osaka JP", "host": "example.ddns.net", "port": 8073,
 "lat": 34.8585, "lon": 135.5144, "pos_source": "where the position comes from (optional note)", "password": ""}
```

## Network notes

* The PC-side program's API (TCP 8765), discovery (UDP 8766) and Cardputer-speaker audio (TCP 8767) listen on **all network interfaces** of the computer, protected only by a shared token (`RADIO_DAYS_LOCAL_2026` by default, compiled into the firmware and the `--token` option). That is meant for a trusted home LAN. It is **not** a secret and not suitable for an untrusted network. To change it, change `TOKEN` in the `.ino`, rebuild and flash, and start the PC-side program with `--token`.
* USB control is automatically detected on Espressif/M5Stack serial devices and also carries OUT ADV audio. Use `--serial-port off` to disable it or `--serial-port COM4` / `--serial-port /dev/cu.usbmodem...` to select a port explicitly.
* The Receiver Manager (TCP 8780) is loopback-only; `--manager-port 0` turns it off.
* Many KiwiSDR owners refuse a second connection from the same IP address; do not run two copies against the same receiver.

## What is verified and what is not

Legend: **tested** = run and passed here; **real** = a real KiwiSDR/real hardware was involved; **not verified** = not run.

| Item | State |
|---|---|
| Mac program: API, volume/mute measured at the audio output (BlackHole loopback), output switch, AUTO scan logic (simulated sky), audio streaming protocol | **tested**: 37 automated tests |
| Mac program receiving from real KiwiSDR receivers, AUTO scan on real receivers | **tested, real** (on 2026-10-23/24; reception depends on time and receiver) |
| Receiver Manager: store and validation, atomic save/backup/restore, corrupt-file fallback, HTTP security (host/origin/CSRF/size), conflict detection, status check wording, candidate-file schema | **tested**: 26 automated tests (14 for saving/security + 12 for the check history, bulk-check limit and recording) |
| Receiver Manager page logic (reorder, validation, payload, filters, wording of results) | **tested**: 15 Node tests (also the date/time wording in Japanese and English and the bulk-run limit) |
| Live Web radio: page/assets, synchronized state API and control actions | **tested**: 3 automated tests; browser display and receiver switching also checked manually (2026-10-26) |
| Cardputer USB control protocol and shared USB/HTTP command handling | **real-device verified with SDR-1.28 on 2026-10-27**: TinyUSB enumerated on macOS, `CABLE ok` was confirmed, and operation continued with Cardputer Wi-Fi disabled. On Windows 11, after a PC-side fix (DTR is now raised when the port opens), `CABLE ok`, tuning with Cardputer Wi-Fi OFF and `OUT ADV` audio over USB alone were confirmed. Hot replug may require a Cardputer restart. |
| Receiver Manager page in a browser against a running program: select a candidate, reorder, save; `/servers` of the running program shows the new list; backup and file written | **tested** (manual run in a browser, 2026-10-24) |
| Checks against the public directory's Japanese entries (2026-10-24, twice): HTTP check: 23 of 25 answered in each of the two runs (not necessarily the same 23); audio check run on one (Adachi) and audio blocks arrived | **real, a single snapshot**; says nothing about other times |
| Check history stored and shown again after reloading the page; HTTP and audio kept apart; bulk HTTP check of the 25 Japanese entries from the page with at most 4 at a time; opening the page sends no check request; Operation Guide opens in a new tab in Japanese and English | **tested** (browser, 2026-10-24) |
| Operation Guide agrees with the code: every handled key appears in it, keys exist in `docs/OPERATION_JA.md`, and numbers and upper-right labels match | **tested**: automated tests |
| Firmware compiles; `.ino` + `.bin` built from the same source; bootloader and partition table identical to the previous version | **tested** (arduino-cli build succeeded) |
| Firmware drawing logic and screen changes rendered on a PC from the real source (91 checks), audio protocol and playout rules (host tests) | **tested** (PC simulation, not the device) |
| **Anything on the real Cardputer ADV**: flashing, boot, screen look, five-row status panel, JST clock, keys/knob, Wi-Fi, globe, **the globe showing a list saved from the Receiver Manager**, AUTO scan, Cardputer speaker playback | **not verified** (the author checks on the device) |
| **Windows 11**: all of the PC-side program running on Windows (start file, Python setup, audio output, firewall, browser opening, Receiver Manager, Cardputer link) | **tried by the author on one Windows 11 PC** (Python 3.12.10, real Cardputer ADV): start, browser, Receiver Manager, Cardputer link, volume/mute/output switch, shutdown, second start. **Not verified:** other Windows PCs, long runs, Python 3.9/3.10, the cause of one earlier run where the PC speaker played regardless of the Cardputer (not reproduced) |
| Linux | **not supported / not tested** |
| Long runs, sleep/wake, other Wi-Fi setups, other Macs/macOS versions | **not verified** |

Details: [docs/VERIFICATION.md](docs/VERIFICATION.md) (English summary) and [docs/VERIFICATION_JA.md](docs/VERIFICATION_JA.md) (full log in Japanese).

## Run the tests

```bash
cd mac
.venv/bin/python -m unittest discover -s tests -p "test_*.py"     # 111 tests; volume test needs the BlackHole 2ch device, otherwise it is skipped
node tests/js/test_logic.js                                       # Receiver Manager page logic
cd ../firmware/tools/host_render && M5GFX=/path/to/libraries/M5GFX ./run.sh   # screen drawing tests on a PC (g++ needed)
cd ../host_tests && PYTHON=../../../mac/.venv/bin/python ./run.sh             # audio protocol / playout tests on a PC
```

## Folder layout

```
Start RADIO DAYS Mac.command   macOS: start the radio and open the Receiver Manager in the browser
Start RADIO DAYS Win.bat       Windows 11: the same
Open Receiver Manager Mac.command / ... Win.bat   Receiver Manager only, without the radio (opens the existing page if one is running)
mac/                           the PC-side program, shared by macOS and Windows (the name is historical)
mac/radiodays_sdr.py           the program (radio, Cardputer link)
mac/platform_support.py        the few OS-specific parts (LAN address, port sharing, console, sleep)
mac/servers_store.py           validating, crash-safe reader/writer of servers.json
mac/receiver_manager.py        Receiver Manager backend (loopback HTTP)
mac/check_history.py           per-receiver check history (check_history.json)
mac/manager/                   Receiver Manager and detailed Operation Guide
mac/manager/radio/             live Cardputer-style Web radio, controls and display assets
mac/servers.default.json       the starting list
mac/tools/                     fetch_kiwiclient.py, build_candidates.py, kiwi_position.py, build_guide_docs.py
mac/tests/                     automated tests (live_*.py use real receivers)
firmware/RadioDays_SDR/        Cardputer ADV source (.ino and headers)
firmware/bin/                  prebuilt images + SHA256SUMS.txt        firmware/build.sh  rebuild
firmware/tools/                PC-side tests of drawing and audio
docs/                          operation guide (QUICK_GUIDE_EN/JA generated, OPERATION_JA detailed), verification, preview images
```

## License

MIT, see [LICENSE](LICENSE). Third-party components and data: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
This project is not affiliated with KiwiSDR or M5Stack. Public KiwiSDR receivers belong to their owners: respect their limits and rules.
