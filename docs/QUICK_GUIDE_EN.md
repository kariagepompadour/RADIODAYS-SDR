<!-- Generated from mac/manager/guide.json by mac/tools/build_guide_docs.py. Do not edit by hand. -->

# RADIO DAYS SDR - Operation Guide

**RADIO DAYS SDR quick reference (v1.7)** · [日本語](QUICK_GUIDE_JA.md)

A quick reference for the Cardputer ADV and the PC (Mac / Windows). More detail is in docs/OPERATION_JA.md (Japanese).

## Basic operation

Order of use: (1) double-click the start file for your computer (Mac: Start RADIO DAYS Mac.command / Windows 11: Start RADIO DAYS Win.bat), (2) switch the Cardputer ADV on and, the first time, pick a Wi-Fi and type its password, (3) wait for the WiFi / PC / Kiwi lamps at the top right to turn green (the computer is found automatically). The computer and the Cardputer must be on the same LAN.

| Key / action | |
|---|---|
| Turn U135 | MANUAL: change the frequency by the selected tuning increment. AUTO: scan automatically in the direction you turn |
| Hold U135 and turn | volume ±5 |
| Short press U135 | mute (while AUTO is scanning it stops the scan instead) |
| `;` `.` `+` `-` | volume ±5 (; and + raise, . and - lower) |
| `M` | mute |
| `1` `2` `3` `4` | set the tuning increment to 0.1 / 1 / 5 / 10 kHz (shown by the bottom row of the upper-right panel) |
| `F` | type a frequency (ENTER to confirm) |
| `A` `U` `L` | mode AM / USB / LSB. Changing mode selects its recommended increment (AM = 5 kHz; USB/LSB = 1 kHz); keys 1-4 can then override it |
| `W` | Wi-Fi setup (pick a network and type its password) |
| `I` | type the PC's IP address by hand (when it is not found automatically; use the PC IP printed in the start window) |
| `H` | key list (on the Cardputer screen) |
| `D` | diagnostics screen (to tell where drop-outs come from) |

Note: , and / lower / raise the frequency (in MANUAL). The SIGNAL meter is the signal strength the KiwiSDR reports; the VOL meter shows the setting, not the actual sound level.

## Choosing a receiver (KiwiSDR)

| Key / action | |
|---|---|
| `G` | open the globe (it fetches the current list from the PC each time) |
| Turn U135 `;` `.` `,` `/` | choose receivers in turn inside the globe (the chosen place turns to the front) |
| `ENTER` Short press U135 | ask the PC to connect to the chosen receiver |
| `Esc` `` ` `` | close the globe without changing anything |
| `N` `P` | go to the next / previous receiver in the list without opening the globe |

- The contents and order of the list are set in the Receiver Manager (see below). After saving, press G again on the Cardputer to see the new list.
- On the globe a yellow dot is a registered receiver, red + white is the selected one, and a ring marks the one in use (green = receiving, orange = connecting, red = failed). A receiver without a position has no dot but can still be selected and connected.
- If the requested receiver cannot be reached, the PC switches to the next one automatically.

## MANUAL / AUTO tuning

| Key / action | |
|---|---|
| `Tab` `T` | switch MANUAL ⇄ AUTO (it starts in MANUAL and is not saved) |
| Turn U135 | AUTO: scan in the direction you turn (right = up, left = down) and stop at a signal that looks receivable |
| `Esc` `` ` `` Push U135 | stop the scan (Esc, or push U135) |

- Turn the same way again after it stops to skip that signal and look for the next; turn the other way to reverse.
- The scan step is 5 kHz or more and takes about 1.4-1.5 s per step (measured on real KiwiSDR receivers). Fine-tune in MANUAL after it stops.
- At the band edge it shows END OF BAND; if the KiwiSDR link is lost it ends with SCAN ENDED: KIWI LINK LOST.
- Reception fades, so a signal it stopped on may be weaker a little later.

## Switching the audio output

| Key / action | |
|---|---|
| `O` | switch the audio output between the PC speaker and the Cardputer speaker (the choice is saved and restored after a restart) |

- OUT PC (PC speaker) and OUT ADV (Cardputer speaker) are both shown at the top right; the lamp of the one in use is lit. They never play at the same time. Volume and mute work the same for both.
- Until OUT ADV starts playing, the lower black-and-green status line shows ADV audio: waiting for PC / connecting / buffering / reconnecting / no memory. If memory is short it stays on the PC speaker automatically.
- While the Cardputer is down the PC does not start playing by itself (so it never surprises you).
- Judge the loudness by ear on the real device.

## Using the Receiver Manager

Opening the start file (Start RADIO DAYS Mac.command / Start RADIO DAYS Win.bat) opens the Receiver Manager in your browser by itself, every time. If it did not open, or after you closed it, open http://127.0.0.1:8780/ in a browser on the same computer. Closing the browser does not stop the radio (press Ctrl+C in the window that started it). For listening the start file alone is enough. "Open Receiver Manager Mac.command" / "Open Receiver Manager Win.bat" is only for editing the list without starting the radio; if a Receiver Manager is already running it opens the existing page. The page runs only on that computer and cannot be opened from other devices.

- Registered (left): reorder with the ≡ handle or ↑↓, unregister with ×. Name, short name, host, port and latitude/longitude can be edited in place. This order is the order of the globe list (up to 32).
- Candidates (right): a snapshot of the public KiwiSDR list. Tick to register, untick to unregister. Search, "Japan only", country and registered/unregistered filters are available. Whether a candidate can be reached now is unknown.
- Add by hand: give a host name and port directly (for example your own KiwiSDR).
- Save and apply: the input is validated, saved, and loaded into the running program at once. The previous save stays in servers.json.bak and .servers_backups/ (10 generations); "Restore previous save" goes back. "Discard changes" returns to the state when the page was opened.
- After saving, press G on the Cardputer again to see the new list. If you remove the receiver in use and save, the first receiver is connected instead.

### Check buttons and check history

- HTTP check: only asks whether the receiver's web server answers /status (one request). It does not show that audio can be received.
- Audio check (~7 s): really connects and shows whether audio blocks arrive. It uses one user slot of that receiver briefly, so press it one receiver at a time when needed. There is no bulk run.
- Each result is saved with its date and time as the "last check" and is shown again after closing the browser or restarting the PC-side program (check_history.json). HTTP and audio checks are recorded separately; anything not yet checked shows "not checked".
- "Answers" and "audio received" are the result at that date and time. They do not guarantee a connection now (public receivers get busy or go offline).
- Opening the page asks nothing. "Re-check registered (HTTP)" and "Check shown candidates (HTTP)" run only when pressed, show the number of receivers and ask for confirmation first, query at most 4 at a time, and can be stopped midway.

## The five-row status panel

The white panel at the upper right has five rows: (1) OUT PC / OUT ADV, (2) WiFi / PC / Kiwi, (3) MANUAL / AUTO, (4) AM / USB / LSB, and (5) 0.1 / 1 / 5 / 10 kHz. Only the three link lamps use traffic-light colours: green=healthy, yellow=connecting, red=error, dark=unknown. In the other rows amber means selected and dark means not selected.

| Lamp | Green | Yellow | Red | Grey |
|---|---|---|---|---|
| WiFi | Wi-Fi connected | trying to connect | not connected | — |
| PC | the PC-side program answers | Wi-Fi is up but the PC does not answer | — | no Wi-Fi |
| Kiwi | connected to the KiwiSDR | connecting / reconnecting | connection error | the PC does not answer (K cannot be known) |

## When the connection does not work

The status line in the lower black-and-green display and the WiFi / PC / Kiwi lamps at the upper right show where it is stuck. Check in this order.

| Symptom | What it may be / what to do |
|---|---|
| W red or yellow / status line Wi-Fi: … | The Cardputer is not on Wi-Fi. Press W and choose the network again. |
| P yellow / PC: searching… / PC: connect failed | The PC-side program is not running, the IP is wrong, or the devices are on different networks. Check that the start file (Start RADIO DAYS Mac.command / Start RADIO DAYS Win.bat) is running and press I to type the PC IP shown in its window. Guest Wi-Fi that blocks device-to-device traffic does not work. Allow the firewall dialog if it appears (on Windows allow "Private networks"). |
| after 3+ s of silence the screen shows PC NOT CONNECTED | The cause cannot be told (PC side, wrong IP or network look the same). The last two lines (error text and PC IP) are the clue. The normal screen returns by itself once the PC answers. |
| P green but K yellow or red / Kiwi: <error> | The PC-side program runs but the KiwiSDR cannot be reached (full, offline, password, a second connection from the same IP refused, ...). It moves to the next receiver by itself; use N / P, or G to pick another. Do not connect two programs to the same receiver at once. |
| connected but no sound | Check OUT PC / OUT ADV, volume and mute. For OUT PC check the PC's output device (--sound-device); for OUT ADV read ADV audio: … in the status line. The D screen tells whether drop-outs come from Wi-Fi or the Cardputer. |
| the Receiver Manager does not open / "Cannot reach the PC-side program" | Check that the start file (Start RADIO DAYS Mac.command / Start RADIO DAYS Win.bat, or Open Receiver Manager Mac.command / Open Receiver Manager Win.bat) is running and open http://127.0.0.1:8780/ in a browser on the same computer. It cannot be opened from other devices. It is off when started with --manager-port 0. If its window says "Receiver Manager is NOT available", another program uses port 8780 (find it with the check command shown, or give --manager-port another number). |
| the saved list does not appear on the Cardputer | Press G on the Cardputer again (it fetches the list each time it opens). In the Receiver Manager check that saving succeeded and that "unsaved changes" is gone. |
| "answers" but cannot receive | The HTTP check only shows that the web server answered, at that earlier time. Try the audio check on that one receiver, and suspect crowding, a password or a time limit. |

---
More detail: [OPERATION_JA.md](OPERATION_JA.md) (Japanese)
