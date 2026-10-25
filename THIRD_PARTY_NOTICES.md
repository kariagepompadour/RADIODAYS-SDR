# Third-party notices

**English | [日本語](#日本語)**

The source code written for RADIO DAYS SDR (`.ino`, `.h`, Python, HTML/JavaScript, scripts) is licensed under the MIT License; see [LICENSE](LICENSE).
This file lists what else is used, how it reaches you, and under which terms. It is a good-faith summary, not legal advice; check the upstream licenses before redistributing.

## 1. KiwiClient (not redistributed here)

The Mac program talks to KiwiSDR receivers with the Python client from <https://github.com/jks-prv/kiwiclient> (`kiwi/`, `mod_pywebsocket/`, `chunk/`).
**It is not included in this repository.** `mac/tools/fetch_kiwiclient.py` downloads one fixed commit
(`4eb733e6b6147f7fbeb97ced64cdac029b202d18`) from GitHub on your computer and verifies the SHA-256 of every file against a list in the script.
Reason: when this repository was prepared, the upstream repository showed no license file for the client code as a whole, and one file (`kiwi/rigctld.py`) carries a GPL-2.0-or-later notice.
Because the license situation is unclear, the files are fetched from their author instead of being copied. They are used unmodified, are git-ignored (`mac/vendor/`) and are not part of any release file of this project.

## 2. Python packages (installed by pip from `mac/requirements.txt`, not included)

| Package | License (as reported by the installed package) |
|---|---|
| numpy | BSD-3-Clause (plus bundled components under 0BSD / MIT / Zlib / CC0-1.0) |
| SoundCard | BSD-3-Clause |
| cffi, pycparser (installed with SoundCard; used for the Windows audio output) | cffi MIT-0, pycparser BSD-3-Clause |

## 3. Firmware: what the compiled `.bin` files contain

The `.bin` files in `firmware/bin/` were built from `firmware/RadioDays_SDR/` and link the following (versions used for the build):

| Component | Version | License |
|---|---|---|
| M5Cardputer | 1.1.1 | MIT (M5Stack) |
| M5Unified | 0.2.27 | MIT (M5Stack) |
| M5GFX (LovyanGFX-based) | 0.2.34 | MIT (M5Stack); bundled components keep their own notices |
| ArduinoJson | 7.4.3 | MIT (Benoit Blanchon) |
| Arduino core for ESP32, M5Stack board package `m5stack:esp32` | 3.3.10 | LGPL-2.1 (core); Apache-2.0 for ESP-IDF; bundled components keep their own licenses |

The firmware links the ESP32 Arduino core (LGPL-2.1). You are free to rebuild and re-flash the firmware from the source in this repository with `firmware/build.sh`.
A byte-identical binary is not guaranteed (build paths and library versions are embedded).
The board/package files themselves are not included in this repository.

## 4. Data

* `firmware/RadioDays_SDR/globe_land.h`: a 256×128 1-bit land mask for the globe screen, derived from **Natural Earth** 1:110m land data (public domain, <https://www.naturalearthdata.com/>). It was taken unchanged from the author's earlier RADIO DAYS project (MIT).
* `mac/manager/receiver_candidates.json`: a list of public KiwiSDR receivers **generated from the public KiwiSDR directory** (<https://kiwisdr.com/public/>, read through a mirror) by `mac/tools/build_candidates.py`.
  It contains only facts that each receiver publishes about itself (name, location, host/port, antenna text). Its snapshot date is stored in the file.
  **It does not say that any of these receivers can be reached today.** Each receiver belongs to its owner; please follow the owner's rules (time limits, user limits, passwords).

## 5. Trademarks

KiwiSDR is a product of its respective owners; M5Stack and Cardputer are trademarks of M5Stack. This is an independent hobby project with no affiliation.

---

## 日本語

* 本プロジェクトで書いたコード（`.ino`/`.h`/Python/HTML・JavaScript/スクリプト）は MIT License です（[LICENSE](LICENSE)）。
* **KiwiClient**（<https://github.com/jks-prv/kiwiclient>）は**このリポジトリに含めていません**。公開時点で、クライアント全体に対するライセンスファイルが確認できず、一部（`kiwi/rigctld.py`）は GPL-2.0 以降の表示があるためです。`mac/tools/fetch_kiwiclient.py` が、固定した 1 つのコミットを GitHub から**お使いの PC に**取得し、各ファイルの SHA-256 を検証します。無改変で使い、`mac/vendor/` は git の管理対象外です。
* Python パッケージ numpy（BSD-3-Clause 系）と SoundCard（BSD-3-Clause）は pip で導入されます（同梱しません）。
* ファームウェアの `.bin` には、M5Cardputer / M5Unified / M5GFX / ArduinoJson（いずれも MIT）と、ESP32 Arduino コア（LGPL-2.1。ESP-IDF は Apache-2.0）が含まれます。ソースから `firmware/build.sh` で再ビルド・再書き込みできます。
* `globe_land.h` は Natural Earth 1:110m（パブリックドメイン）から作った地球儀の陸地データで、作者の以前のプロジェクト（MIT）から無改変で取り込みました。
* `receiver_candidates.json` は公開 KiwiSDR 一覧から生成した**候補リスト**で、各受信機が自己公開している情報だけを含みます。**今日接続できることを示すものではありません。** 各受信機は運営者のものです。運営者のルール（時間制限・人数制限・パスワード）を守ってください。
* KiwiSDR、M5Stack、Cardputer は各権利者の製品・商標です。本プロジェクトは個人の趣味のプロジェクトで、これらの権利者とは無関係です。
