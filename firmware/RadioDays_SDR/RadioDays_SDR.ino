/*
  RADIO DAYS SDR - controller for M5Stack Cardputer ADV + U135 rotary encoder.

  The Cardputer controls radiodays_sdr.py over USB serial or the same LAN. The PC always
  receives KiwiSDR audio; OUT PC plays it locally, while OUT ADV sends it to the Cardputer
  over Wi-Fi (TCP 8767).

  Structure (differs from the earlier SDR prototype on purpose):
  - All network traffic to the Mac runs in its own task (netTask), so the knob, keys and
    screen never wait for HTTP.  Commands are absolute (tune to X kHz, volume N), so a lost
    reply can never double-step.
  - The status line separates the three links: Wi-Fi, Mac, and KiwiSDR, so a failure can
    be located ("PC: connect failed" is not the same as "Kiwi: error").
  - Volume, mute and frequency are pushed to the Mac; the Mac reports the real values it
    uses (volume, S-meter from the real received signal strength, real output level).

  Ported from the existing RADIO DAYS U10 sketch (RadioDays_U10.ino):
  - key edge detection (keyboardPoll), U135 checked reads and detent counting,
    knob behaviour (turn = tune, hold + turn = volume, short press = mute),
  - Wi-Fi screens: list / password (TAB shows or hides) / saved networks (3), same keys
    (; and . move, R rescan, S saved, ` back, Fn+` back from the password).
  Not ported: its raw-NVS records with CRC (this sketch uses Preferences), and the question
  "which saved network gives way" (the least recently used one is replaced).

  Status: compiled; NOT yet run on a real Cardputer ADV.  See docs/VERIFICATION.md.
*/
#include <M5Cardputer.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <USB.h>
#include "esp32-hal-tinyusb.h"
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "soc/rtc_cntl_reg.h"
// USB runs on the OTG controller (TinyUSB, USBMode=default), so no USB Serial/JTAG
// register headers are needed here.
#include "soc/usb_struct.h"

#include "RadioDays_Types.h"
#include <freertos/stream_buffer.h>
#include "RadioDays_AudioProto.h"  // audio stream from the Mac (pure C++, tested on a PC)
#include "RadioDays_Playout.h"     // when to start / re-buffer / drop (pure C++, tested on a PC)
#include "globe_land.h"  // land mask from RADIO DAYS U10 (Natural Earth, public domain)  // types used in function signatures (Arduino makes prototypes before the code)

#define BUILD_TAG "SDR-1.28"

// ------------------------------------------------------------------ configuration
static const char* TOKEN = "RADIO_DAYS_LOCAL_2026";  // must equal --token of radiodays_sdr.py
static constexpr uint16_t MAC_HTTP_PORT = 8765;
static constexpr uint16_t MAC_DISCOVERY_PORT = 8766;

static constexpr uint8_t PORT_SDA = 2;  // Cardputer ADV HY2.0 (Port A)
static constexpr uint8_t PORT_SCL = 1;
static constexpr uint32_t I2C_FREQ = 200000;
static constexpr uint8_t U135_ADDR = 0x40;
static constexpr uint8_t U135_REG_VALUE = 0x10;
static constexpr uint8_t U135_REG_BUTTON = 0x20;
static constexpr int16_t COUNTS_PER_DETENT = 2;
static constexpr uint8_t BUTTON_PRESSED_RAW = 0;
static constexpr uint32_t SHORT_PRESS_MAX_MS = 800;
static constexpr uint32_t RELEASE_GRACE_MS = 200;
static constexpr uint32_t U135_PROBE_INTERVAL_MS = 2000;

static constexpr float FREQ_MIN = 10.0f, FREQ_MAX = 30000.0f;
static constexpr int VOL_STEP = 5;
static constexpr int WIFI_SAVED_MAX = 3;
static constexpr uint32_t WIFI_TRY_MS = 12000;
static constexpr uint32_t WIFI_JOIN_TIMEOUT_MS = 15000;

#define lcd M5Cardputer.Display
static constexpr uint16_t COL_BG = 0x0000;
static constexpr uint16_t COL_TEXT = 0xFFFF;
static constexpr uint16_t COL_DIM = 0x8410;
static constexpr uint16_t COL_AMBER = 0xFD20;
static constexpr uint16_t COL_OK = 0x07E0;
static constexpr uint16_t COL_WARN = 0xFFE0;
static constexpr uint16_t COL_BAD = 0xF800;

// ------------------------------------------------------------------ shared state (UI <-> net task)
static SemaphoreHandle_t mtx;
struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

static const char* const MODES[] = {"am", "usb", "lsb"};

// What the user has set here.  The *Ver counters tell the net task that a value must be pushed.
static float wantFreq = 10000.0f;
static int wantMode = 0;
static int wantVolume = 70;
static bool wantMuted = false;
static uint32_t verFreq = 1, verMode = 1, verVol = 1, verMute = 1, verServer = 0;
static int wantServer = 0;
static const float STEPS[] = {0.1f, 1.0f, 5.0f, 10.0f};  // kHz per tuning step (keys 1-4)
static int stepIdx = 1;
static int defaultStepForMode(int mode) { return mode == 0 ? 2 : 1; }  // AM 5 kHz; USB/LSB 1 kHz
static volatile bool tuneAuto = false;  // MANUAL (default, not saved) / AUTO scan
static int wantScanDir = 0;             // what we asked the Mac for: 0 stop, +1 up, -1 down
static uint32_t verScan = 0;
static uint32_t autoHintUntil = 0;      // "turn the knob to search" for a few seconds after switching to AUTO
static volatile int wantOutput = 0;  // 0 = Mac speaker (default), 1 = Cardputer speaker; saved in NVS
static uint32_t verOut = 1;
static bool synced = false;  // adopted the Mac's values once

// The small modern information display gets JST from NTP. Failure is cosmetic only:
// radio/network operation never waits for the clock.
static bool clockNtpStarted = false, clockSynced = false;
static uint32_t clockPollMs = 0;
static bool clockPersistPending = false;
static uint32_t clockSaveMs = 0;
static char clockDate[12] = "--- --/--";
static char clockTime[12] = "--:-- JST";

// What the Mac reports.
struct Remote {
  bool ok = false;            // last request succeeded
  char link[12] = "";         // connecting | connected | error
  char server[24] = "";
  char error[40] = "";
  float rssi = -127.0f;
  float s = 0;
  int sOver = 0;
  float outLevel = 0, inLevel = 0;
  int volume = -1;
  bool muted = false;
  int serverIndex = 0, serverCount = 1;
  uint32_t uptime = 0;
  char output[12] = "mac";     // what the Mac says it is doing: mac | cardputer
  bool audioClient = false;    // the Mac has a Cardputer audio connection
  uint32_t audioSent = 0, audioDropped = 0, audioDisc = 0;
  char scan[8] = "off";        // what the Mac's scan is doing: off | up | down
  char scanMsg[12] = "";       // scanning | signal | end | stopped | lost
  float scanExcess = 0;        // dB above the noise floor when it stopped on a signal
  float freq = 0;              // the Mac's frequency (the scan moves it)
  uint32_t scanDoneMs = 0;     // when the last scan ended (for the message that stays a few seconds)
};
static Remote rem;

// ---- Cardputer speaker (audio output 1): shared state between the UI, the network task and the player task
static constexpr uint16_t AUDIO_PORT = 8767;
static constexpr size_t AU_RING_BYTES = 24000;  // 12000 samples = 1 s of 12 kHz mono 16 bit
static constexpr int AU_CH = 0;                 // M5.Speaker virtual channel
enum AuState : uint8_t { AU_OFF, AU_NOMEM, AU_WAIT, AU_CONNECTING, AU_RECONNECT, AU_BUFFERING, AU_PLAYING };
static volatile bool auWanted = false;      // the speaker side is running
static volatile bool auConn = false;        // the audio TCP connection is up
static volatile uint8_t auNetState = AU_WAIT;
static volatile bool auNoMem = false;
static volatile float auLevel = 0;          // level of what the speaker plays (RMS x volume), for the LVL bars
static volatile uint16_t auRate = 11998;
static StreamBufferHandle_t auRing = nullptr;
static rdaudio::Playout auPlayout;
static rdaudio::NetStats auNet;
static rdaudio::FrameAssembler* auAsm = nullptr;
static int16_t auBuf[3][512];
static bool auSpeakerOn = false;

// What the speaker side is doing, in one value (computed from the parts above, so there are no races to get wrong).
static uint8_t auStateNow() {
  if (auNoMem) return AU_NOMEM;
  if (!auWanted) return AU_OFF;
  if (!auConn) return auNetState;
  return auPlayout.playing ? AU_PLAYING : AU_BUFFERING;
}
static const char* auStateName(uint8_t st) {
  switch (st) {
    case AU_NOMEM: return "no memory";
    case AU_WAIT: return "waiting for PC";
    case AU_CONNECTING: return "connecting";
    case AU_RECONNECT: return "reconnecting";
    case AU_BUFFERING: return "buffering";
    case AU_PLAYING: return "playing";
    default: return "off";
  }
}

// The receiver list lives on the Mac (servers.json); this is a copy fetched from /servers.
struct SrvInfo {
  char shortName[22];
  char name[44];
  float lat, lon;
  bool hasPos;  // false = position unverified in servers.json: no marker
};
static constexpr int SRV_MAX = 32;
static SrvInfo srvs[SRV_MAX];
static int srvCount = 0;
static volatile bool srvWant = false;     // the net task should (re)load the list
static uint32_t srvRev = 0;      // bumped when a new list arrives
static char netStatus[40] = "PC: waiting for Wi-Fi";  // result of the last HTTP request
static String macIP;
static bool macManual = false;
static uint32_t statusRev = 0;  // bumped when anything the status line shows changes

// ------------------------------------------------------------------ preferences
static Preferences prefs;
struct SavedNet {
  char ssid[33];
  char pass[65];
};
static SavedNet saved[WIFI_SAVED_MAX];
static int savedCount = 0;
static int savedLast = 0;  // index of the network that connected last

static void prefsLoad() {
  prefs.begin("rdsdr", true);
  savedCount = constrain(prefs.getInt("n", 0), 0, WIFI_SAVED_MAX);
  savedLast = prefs.getInt("last", 0);
  if (savedLast < 0 || savedLast >= max(1, savedCount)) savedLast = 0;
  for (int i = 0; i < savedCount; i++) {
    String s = prefs.getString(("s" + String(i)).c_str(), "");
    String p = prefs.getString(("p" + String(i)).c_str(), "");
    strlcpy(saved[i].ssid, s.c_str(), sizeof(saved[i].ssid));
    strlcpy(saved[i].pass, p.c_str(), sizeof(saved[i].pass));
  }
  macIP = prefs.getString("mac", "");
  macManual = prefs.getBool("macm", false);
  wantFreq = prefs.getFloat("freq", 10000.0f);
  wantMode = constrain(prefs.getInt("mode", 0), 0, 2);
  stepIdx = defaultStepForMode(wantMode);
  wantVolume = constrain(prefs.getInt("vol", 70), 0, 100);
  wantOutput = constrain(prefs.getInt("out", 0), 0, 1);  // default: Mac speaker
  int64_t savedEpoch = prefs.getLong64("epoch", 0);
  prefs.end();
  time_t now = time(nullptr);
  if (now < 1704067200 && savedEpoch >= 1704067200) {
    timeval tv = {(time_t)savedEpoch, 0};
    settimeofday(&tv, nullptr);  // fallback if this ADV has no battery-backed RTC
  }
}

static bool prefsSaveWifi() {
  if (!prefs.begin("rdsdr", false)) return false;
  bool ok = prefs.putInt("n", savedCount) > 0;
  ok &= prefs.putInt("last", savedLast) > 0;
  for (int i = 0; i < WIFI_SAVED_MAX; i++) {
    if (i < savedCount) {
      ok &= prefs.putString(("s" + String(i)).c_str(), saved[i].ssid) > 0;
      ok &= prefs.putString(("p" + String(i)).c_str(), saved[i].pass) >= 0;
    } else {
      prefs.remove(("s" + String(i)).c_str());
      prefs.remove(("p" + String(i)).c_str());
    }
  }
  prefs.end();
  return ok;
}

static void prefsSaveMac() {
  if (!prefs.begin("rdsdr", false)) return;
  prefs.putString("mac", macIP);
  prefs.putBool("macm", macManual);
  prefs.end();
}

static uint32_t lastRadioSaveMs = 0;
static bool radioDirty = false;
static void prefsSaveRadioIfIdle() {  // flash writes are rare: 3 s after the last change
  if (!radioDirty || millis() - lastRadioSaveMs < 3000) return;
  radioDirty = false;
  if (!prefs.begin("rdsdr", false)) return;
  prefs.putFloat("freq", wantFreq);
  prefs.putInt("mode", wantMode);
  prefs.putInt("vol", wantVolume);
  prefs.putInt("out", wantOutput);
  prefs.end();
}

static int savedIndexOf(const char* ssid) {
  for (int i = 0; i < savedCount; i++)
    if (strcmp(saved[i].ssid, ssid) == 0) return i;
  return -1;
}

// ------------------------------------------------------------------ input (ported from RADIO DAYS U10)

static std::vector<char> prevWord;
static bool prevEnter = false, prevDel = false, prevTab = false;

static KeyInput keyboardPoll() {
  KeyInput in;
  auto& ks = M5Cardputer.Keyboard.keysState();
  for (char c : ks.word) {
    bool wasDown = false;
    for (char p : prevWord)
      if (p == c) { wasDown = true; break; }
    if (!wasDown) in.chars += c;
  }
  in.enter = ks.enter && !prevEnter;
  in.del = ks.del && !prevDel;
  in.tab = ks.tab && !prevTab;
  in.fn = ks.fn;
  prevWord = ks.word;
  prevEnter = ks.enter;
  prevDel = ks.del;
  prevTab = ks.tab;
  return in;
}
static bool hasChar(const KeyInput& k, char c) { return k.chars.indexOf(c) >= 0; }
static bool hasCharI(const KeyInput& k, char c) { return hasChar(k, (char)tolower(c)) || hasChar(k, (char)toupper(c)); }
static bool anyKey(const KeyInput& k) { return k.chars.length() > 0 || k.enter || k.del || k.tab; }

static bool u135Found = false;
static int16_t u135Last = 0, u135Accum = 0;
static bool knobDown = false, knobTurnedWhileDown = false;
static uint32_t knobDownMs = 0, knobUpMs = 0, u135ProbeMs = 0;
static uint8_t u135ReadErrors = 0;

static bool u135Read(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(U135_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((uint8_t)U135_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    int b = Wire.read();
    if (b < 0) return false;
    buf[i] = (uint8_t)b;
  }
  return true;
}
static bool u135ReadState(int16_t& value, bool& down) {
  uint8_t v[2], b;
  if (!u135Read(U135_REG_VALUE, v, 2) || !u135Read(U135_REG_BUTTON, &b, 1) || b > 1) return false;
  value = (int16_t)(v[0] | (v[1] << 8));
  down = (b == BUTTON_PRESSED_RAW);
  return true;
}
static void u135Probe() {
  u135ProbeMs = millis();
  Wire.beginTransmission(U135_ADDR);
  if (Wire.endTransmission() != 0) return;
  int16_t v;
  bool down;
  if (!u135ReadState(v, down)) return;
  u135Found = true;
  u135Last = v;
  u135Accum = 0;
  knobDown = down;
  knobTurnedWhileDown = true;
  u135ReadErrors = 0;
  Serial.printf("[U135] found, value %d\n", v);
}
static KnobInput u135Poll() {
  KnobInput in;
  if (!u135Found) {
    if (millis() - u135ProbeMs >= U135_PROBE_INTERVAL_MS) u135Probe();
    return in;
  }
  int16_t v;
  bool down;
  if (!u135ReadState(v, down)) {
    if (++u135ReadErrors >= 10) {
      u135Found = false;
      u135ProbeMs = millis();
      Serial.println("[U135] lost");
    }
    return in;
  }
  u135ReadErrors = 0;
  u135Accum += (int16_t)(v - u135Last);
  u135Last = v;
  while (u135Accum >= COUNTS_PER_DETENT) { u135Accum -= COUNTS_PER_DETENT; in.detents++; }
  while (u135Accum <= -COUNTS_PER_DETENT) { u135Accum += COUNTS_PER_DETENT; in.detents--; }
  uint32_t now = millis();
  bool held = down || knobDown;
  bool justReleased = !held && now - knobUpMs < RELEASE_GRACE_MS;
  if (held || justReleased) in.volumeDetents = in.detents;
  else in.tuneDetents = in.detents;
  if (down && !knobDown) { knobDownMs = now; knobTurnedWhileDown = false; }
  if (held && in.detents != 0) knobTurnedWhileDown = true;
  if (!down && knobDown) {
    knobUpMs = now;
    if (!knobTurnedWhileDown && now - knobDownMs <= SHORT_PRESS_MAX_MS) in.shortPress = true;
  }
  knobDown = down;
  return in;
}

// ------------------------------------------------------------------ Wi-Fi manager (background search)
enum WifiState : uint8_t { W_IDLE, W_TRY, W_UP, W_WAIT };
static WifiState wifiState = W_IDLE;
static bool wifiPaused = false;  // temporary USB-only test mode; saved networks are preserved
static uint32_t wifiStateMs = 0, wifiWaitMs = 5000;
static int wifiOrderPos = 0, wifiOrder[WIFI_SAVED_MAX], wifiOrderN = 0;
static char wifiActive[33] = "";

static void wifiBuildOrder() {
  wifiOrderN = 0;
  if (savedCount == 0) return;
  wifiOrder[wifiOrderN++] = savedLast;
  for (int i = 0; i < savedCount; i++)
    if (i != savedLast) wifiOrder[wifiOrderN++] = i;
}
static void wifiBeginAttempt() {
  const SavedNet& n = saved[wifiOrder[wifiOrderPos]];
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(n.ssid, n.pass[0] ? n.pass : nullptr);
  wifiState = W_TRY;
  wifiStateMs = millis();
  statusRev++;
  Serial.printf("[WIFI] trying \"%s\"\n", n.ssid);
}
static void wifiService() {
  if (wifiPaused) return;
  bool up = WiFi.status() == WL_CONNECTED;
  switch (wifiState) {
    case W_IDLE:
      if (savedCount > 0) {
        wifiBuildOrder();
        wifiOrderPos = 0;
        wifiBeginAttempt();
      }
      break;
    case W_TRY:
      if (up) {
        int idx = wifiOrder[wifiOrderPos];
        strlcpy(wifiActive, saved[idx].ssid, sizeof(wifiActive));
        if (savedLast != idx) { savedLast = idx; prefsSaveWifi(); }
        wifiState = W_UP;
        wifiWaitMs = 5000;
        statusRev++;
        Serial.printf("[WIFI] connected \"%s\" IP %s\n", wifiActive, WiFi.localIP().toString().c_str());
      } else if (millis() - wifiStateMs > WIFI_TRY_MS) {
        if (++wifiOrderPos < wifiOrderN) wifiBeginAttempt();
        else { wifiState = W_WAIT; wifiStateMs = millis(); statusRev++; }
      }
      break;
    case W_UP:
      if (!up) { wifiState = W_WAIT; wifiStateMs = millis(); wifiWaitMs = 1000; statusRev++; }
      break;
    case W_WAIT:
      if (millis() - wifiStateMs >= wifiWaitMs) {
        wifiWaitMs = min<uint32_t>(wifiWaitMs * 2, 60000);
        wifiBuildOrder();
        wifiOrderPos = 0;
        wifiBeginAttempt();
      }
      break;
  }
}

// ------------------------------------------------------------------ Mac link (runs in netTask)
static WiFiUDP udp;
static volatile bool usbPcOk = false;
static uint32_t usbLastTry = 0, usbRequestId = 0;
static volatile bool requestWifiScreen = false, requestWifiToggle = false;
static volatile bool requestMacIpScreen = false, requestHelpScreen = false, requestGlobeScreen = false, requestDiagScreen = false, requestRadioScreen = false;
static volatile uint8_t reportedScreen = SC_RADIO;
static uint32_t lastCardCommandSeq = 0;

// USB uses the OTG/TinyUSB CDC path. The minimal comparison probes showed that Hardware CDC never enumerates on
// this Cardputer ADV after a Launcher start, while TinyUSB mounts and exchanges data reliably on the same cable.
static volatile uint32_t usbMounts = 0, usbUnmounts = 0, usbSuspends = 0;
static void onUsbEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (id == ARDUINO_USB_STARTED_EVENT) usbMounts++;
  else if (id == ARDUINO_USB_STOPPED_EVENT) usbUnmounts++;
  else if (id == ARDUINO_USB_SUSPEND_EVENT) usbSuspends++;
}

// Cardputer ADV does not expose VBUS sensing to TinyUSB in this Launcher configuration. After a physical unplug,
// TinyUSB can therefore retain stale mounted/DTR state and fail to announce itself when the cable is reinserted.
// Watch the real SOF counter and periodically pulse the device pull-up until host traffic returns.
static uint32_t usbLastSofValue = 0, usbLastSofMs = 0, usbLastReconnectMs = 0;
static bool usbSawHost = false;
static void usbLinkService() {
  uint32_t now = millis();
  uint32_t sof = (USB0.dsts >> 8) & 0x3FFF;
  if (sof != usbLastSofValue) {
    usbLastSofValue = sof;
    usbLastSofMs = now;
    usbSawHost = true;
    return;
  }
  if (!usbSawHost || now - usbLastSofMs < 1200) return;
  usbPcOk = false;  // never keep reporting CABLE ok from stale DTR/mount state
  if (now - usbLastReconnectMs < 2000) return;
  usbLastReconnectMs = now;
  tud_disconnect();
  delay(120);
  tud_connect();
}

// USB CDC protocol carries controls, live state and (when selected) OUT ADV audio.
// Wi-Fi remains an alternate path when USB is not used.
static bool usbMacGet(const String& path, JsonDocument& doc) {
  uint32_t now = millis();
  if (!usbPcOk && now - usbLastTry < 1500) return false;
  usbLastTry = now;
  uint32_t id = ++usbRequestId;
  String target = path + (path.indexOf('?') >= 0 ? "&" : "?") +
                  "card_wifi=" + (WiFi.status() == WL_CONNECTED ? "1" : "0");
  if (path.startsWith("/state")) {
    unsigned diagFillMs = auRing ? (unsigned)(xStreamBufferBytesAvailable(auRing) / 2 * 1000 / 12000) : 0;
    target += "&diag_wifi_rssi=" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    target += "&diag_buf_ms=" + String(diagFillMs);
    target += "&diag_frames=" + String((unsigned)auNet.frames);
    target += "&diag_gaps=" + String((unsigned)auNet.gaps);
    target += "&diag_lost=" + String((unsigned)auNet.lostFrames);
    target += "&diag_maxgap_ms=" + String((unsigned)auNet.maxGapMs);
    target += "&diag_reconnects=" + String((unsigned)auNet.reconnects);
    target += "&diag_underruns=" + String((unsigned)auPlayout.underruns);
    target += "&diag_overruns=" + String((unsigned)auPlayout.overruns);
    target += "&diag_rebuffers=" + String((unsigned)auPlayout.rebuffers);
    target += "&diag_heap=" + String((unsigned)ESP.getFreeHeap());
    target += "&diag_min_heap=" + String((unsigned)ESP.getMinFreeHeap());
    target += "&diag_largest=" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    target += "&card_screen=" + String((unsigned)reportedScreen);
  }
  Serial.printf("RDSDR_REQ %lu %s %s\n", (unsigned long)id, TOKEN, target.c_str());
  String prefix = "RDSDR_RES " + String(id) + " ";
  uint32_t deadline = millis() + 450;
  while ((int32_t)(deadline - millis()) > 0) {
    if (!Serial.available()) { vTaskDelay(2 / portTICK_PERIOD_MS); continue; }
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.startsWith(prefix)) continue;
    DeserializationError err = deserializeJson(doc, line.substring(prefix.length()));
    if (err || (doc.size() == 1 && !doc["error"].isNull())) {
      usbPcOk = false;
      setNetStatus(err ? "PC CABLE: bad reply" : "PC CABLE: request failed");
      return false;
    }
    usbPcOk = true;
    return true;
  }
  usbPcOk = false;
  return false;
}

static bool discoverMac() {
  udp.begin(0);
  udp.beginPacket(IPAddress(255, 255, 255, 255), MAC_DISCOVERY_PORT);
  udp.print(String("RADIO_DAYS_DISCOVER ") + TOKEN);
  udp.endPacket();
  uint32_t t0 = millis();
  bool found = false;
  while (millis() - t0 < 600 && !found) {
    if (udp.parsePacket() > 0) {
      char buf[80];
      int n = udp.read(buf, sizeof(buf) - 1);
      buf[max(0, n)] = 0;
      if (String(buf) == String("RADIO_DAYS_BRIDGE ") + TOKEN) {
        String ip = udp.remoteIP().toString();
        Lock l;
        macIP = ip;
        found = true;
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  udp.stop();
  if (found) prefsSaveMac();
  return found;
}

static void setNetStatus(const char* s) {
  Lock l;
  if (strcmp(netStatus, s) != 0) { strlcpy(netStatus, s, sizeof(netStatus)); statusRev++; }
}

// GET path on the Mac.  Returns true and fills `doc` on HTTP 200 + valid JSON.
static bool macGet(const String& path, JsonDocument& doc) {
  if (usbMacGet(path, doc)) return true;
  if (WiFi.status() != WL_CONNECTED) {
    setNetStatus("PC: connect CABLE or Wi-Fi");
    return false;
  }
  String ip;
  { Lock l; ip = macIP; }
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  String target = path;
  if (path.startsWith("/state")) {
    // Screen mirroring must work over either control route. Previously this
    // field was sent only by usbMacGet(), so physical G/D presses could not
    // change the browser while the Cardputer was using Wi-Fi.
    target += (target.indexOf('?') >= 0 ? "&" : "?");
    target += "card_screen=" + String((unsigned)reportedScreen);
  }
  String url = "http://" + ip + ":" + String(MAC_HTTP_PORT) + target + (target.indexOf('?') >= 0 ? "&" : "?") + "token=" + TOKEN;
  if (!http.begin(url)) { setNetStatus("PC: bad address"); return false; }
  int code = http.GET();
  if (code != 200) {
    char b[40];
    if (code < 0) snprintf(b, sizeof(b), "PC: %s", HTTPClient::errorToString(code).c_str());
    else snprintf(b, sizeof(b), "PC: HTTP %d", code);
    setNetStatus(b);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { setNetStatus("PC: bad reply"); return false; }
  return true;
}

static void absorbState(JsonDocument& doc, bool adopt) {
  Lock l;
  rem.ok = true;
  strlcpy(rem.link, doc["link"] | "", sizeof(rem.link));
  strlcpy(rem.server, doc["server"] | "", sizeof(rem.server));
  strlcpy(rem.error, doc["error"] | "", sizeof(rem.error));
  rem.rssi = doc["rssi"] | -127.0f;
  rem.s = doc["s"] | 0.0f;
  rem.sOver = doc["s_over_db"] | 0;
  rem.outLevel = doc["out_level"] | 0.0f;
  rem.inLevel = doc["in_level"] | 0.0f;
  rem.volume = doc["volume"] | -1;
  rem.muted = doc["muted"] | false;
  rem.serverIndex = doc["server_index"] | 0;
  rem.serverCount = max(1, (int)(doc["server_count"] | 1));
  strlcpy(rem.output, doc["output"] | "mac", sizeof(rem.output));
  rem.audioClient = doc["audio_client"] | false;
  rem.audioSent = doc["audio_sent"] | 0;
  rem.audioDropped = doc["audio_dropped"] | 0;
  rem.audioDisc = doc["audio_disconnects"] | 0;
  uint32_t commandSeq = doc["card_command_seq"] | 0;
  const char* command = doc["card_command"] | "";
  if (commandSeq != 0 && commandSeq != lastCardCommandSeq) {
    lastCardCommandSeq = commandSeq;
    if (strcmp(command, "wifi") == 0) requestWifiScreen = true;
    else if (strcmp(command, "wifi-toggle") == 0) requestWifiToggle = true;
    else if (strcmp(command, "mac-ip") == 0) requestMacIpScreen = true;
    else if (strcmp(command, "help") == 0) requestHelpScreen = true;
    else if (strcmp(command, "globe") == 0) requestGlobeScreen = true;
    else if (strcmp(command, "diagnostics") == 0) requestDiagScreen = true;
    else if (strcmp(command, "radio") == 0) requestRadioScreen = true;
  }
  int64_t pcEpoch = doc["unix_time"] | (int64_t)0;
  if (pcEpoch >= 1704067200) {
    time_t localEpoch = time(nullptr);
    if (localEpoch < 1704067200 || llabs((long long)localEpoch - (long long)pcEpoch) > 2) {
      timeval tv = {(time_t)pcEpoch, 0};
      settimeofday(&tv, nullptr);
    }
    clockSynced = true;
    clockPersistPending = true;
  }
  {
    static bool prevScanning = false;
    strlcpy(rem.scan, doc["scan"] | "off", sizeof(rem.scan));
    strlcpy(rem.scanMsg, doc["scan_msg"] | "", sizeof(rem.scanMsg));
    rem.scanExcess = doc["scan_excess"] | 0.0f;
    rem.freq = doc["freq"] | 0.0f;
    bool scanning = strcmp(rem.scan, "off") != 0;
    if (scanning || prevScanning) wantFreq = rem.freq;  // the Mac moves the frequency during a scan (and for the last time when it ends)
    if (prevScanning && !scanning) rem.scanDoneMs = millis();
    prevScanning = scanning;
  }
  uint32_t up = doc["uptime"] | 0;
  if (up + 2 < rem.uptime) {  // the Mac program restarted: push everything again
    verFreq++; verMode++; verVol++; verMute++; verOut++;
  }
  rem.uptime = up;
  if (adopt && !synced) {  // first contact: take over what the Mac is using
    wantFreq = doc["freq"] | wantFreq;
    String m = doc["mode"] | "am";
    for (int i = 0; i < 3; i++) if (m == MODES[i]) wantMode = i;
    stepIdx = defaultStepForMode(wantMode);
    wantVolume = constrain((int)(doc["volume"] | wantVolume), 0, 100);
    wantMuted = doc["muted"] | false;
    wantServer = rem.serverIndex;
    synced = true;
  }
  statusRev++;
}

static void netTask(void*) {
  uint32_t lastPoll = 0, lastDiscover = 0, lastTune = 0;
  int failures = 0;
  uint32_t sentFreq = 0, sentMode = 0, sentVol = 0, sentMute = 0, sentServer = 0, sentOut = 0, sentScan = 0;
  for (;;) {
    bool usb = usbPcOk;
    bool wifi = WiFi.status() == WL_CONNECTED;
    String ip;
    bool manual;
    { Lock l; ip = macIP; manual = macManual; }
    if (!usb && wifi && (ip.isEmpty() || (!manual && failures >= 4))) {
      if (millis() - lastDiscover > 3000) {
        lastDiscover = millis();
        setNetStatus("PC: searching...");
        if (discoverMac()) failures = 0;
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      if (ip.isEmpty()) continue;
    }

    // 1. push what the user changed (absolute values, newest only)
    float f; int mo, vol; bool mu; int srv;
    uint32_t vF, vM, vV, vU, vS, vO, vA;
    int outSel, scanDir;
    float scanStep;
    bool scanning;
    { Lock l; vO = verOut; outSel = wantOutput; vA = verScan; scanDir = wantScanDir; scanStep = STEPS[stepIdx]; scanning = strcmp(rem.scan, "off") != 0; f = wantFreq; mo = wantMode; vol = wantVolume; mu = wantMuted; srv = wantServer;
      vF = verFreq; vM = verMode; vV = verVol; vU = verMute; vS = verServer; }
    JsonDocument doc;
    bool did = false, ok = true, polledState = false, audioPull = false;
    if (vF != sentFreq && millis() - lastTune >= 120) {
      lastTune = millis();
      ok = macGet("/tune?khz=" + String(f, 3), doc);
      if (ok) sentFreq = vF;
      did = true;
    } else if (vA != sentScan) {
      ok = macGet("/scan?dir=" + String(scanDir) + "&step=" + String(scanStep, 1), doc);
      if (ok) sentScan = vA;
      did = true;
    } else if (vO != sentOut) {
      ok = macGet(String("/output?value=") + (outSel ? "cardputer" : "mac"), doc);
      if (ok) sentOut = vO;
      did = true;
    } else if (vV != sentVol) {
      ok = macGet("/volume?value=" + String(vol), doc);
      if (ok) sentVol = vV;
      did = true;
    } else if (vU != sentMute) {
      ok = macGet(String("/mute?value=") + (mu ? "1" : "0"), doc);
      if (ok) sentMute = vU;
      did = true;
    } else if (vM != sentMode) {
      ok = macGet(String("/mode?value=") + MODES[mo], doc);
      if (ok) sentMode = vM;
      did = true;
    } else if (vS != sentServer) {
      ok = macGet("/server?index=" + String(srv), doc);
      if (ok) sentServer = vS;
      did = true;
    } else if (srvWant) {
      ok = macGet("/servers", doc);
      if (ok) {
        JsonArray arr = doc["servers"].as<JsonArray>();
        Lock l;
        int n = 0;
        for (JsonObject o : arr) {
          if (n >= SRV_MAX) break;
          SrvInfo& e = srvs[n++];
          strlcpy(e.shortName, o["short"] | "?", sizeof(e.shortName));
          strlcpy(e.name, o["name"] | "", sizeof(e.name));
          e.hasPos = !o["lat"].isNull() && !o["lon"].isNull();
          e.lat = e.hasPos ? (float)o["lat"] : 0.f;
          e.lon = e.hasPos ? (float)o["lon"] : 0.f;
        }
        srvCount = n;
        srvRev++;
        srvWant = false;
      }
      did = true;
    } else if (millis() - lastPoll >= (scanning ? 200u : 700u)) {  // the dial follows the scan: poll often while it runs
      lastPoll = millis();
      ok = macGet("/state", doc);
      polledState = true;
      did = true;
    } else if (outSel && usbPcOk) {
      ok = macGet("/usb-audio", doc);
      audioPull = true;
      if (ok) {
        const char* b64 = doc["pcm"] | "";
        size_t inLen = strlen(b64), outLen = 0;
        uint8_t pcm[768];
        if (inLen && mbedtls_base64_decode(pcm, sizeof(pcm), &outLen,
              (const unsigned char*)b64, inLen) == 0 && outLen >= 2 && auRing) {
          auRate = doc["rate"] | 11998;
          auNet.onFrame(doc["seq"] | 0, millis());
          auConn = true;
          auNetState = AU_BUFFERING;
          size_t samples = outLen / 2;
          size_t fill = xStreamBufferBytesAvailable(auRing) / 2;
          if (auPlayout.admit((uint32_t)fill, samples)) xStreamBufferSend(auRing, pcm, samples * 2, 0);
        }
      }
      did = true;
    }
    if (did) {
      if (ok) {
        failures = 0;
        if (!audioPull) absorbState(doc, true);
        // The browser is a second controller. Adopt its state on a normal poll,
        // but never overwrite a newer physical-key change still waiting to be sent.
        if (polledState) {
          float remoteFreq = doc["freq"] | wantFreq;
          int remoteVol = constrain((int)(doc["volume"] | wantVolume), 0, 100);
          bool remoteMute = doc["muted"] | false;
          int remoteServer = doc["server_index"] | wantServer;
          int remoteOut = String(doc["output"] | "mac") == "cardputer" ? 1 : 0;
          String remoteMode = doc["mode"] | "am";
          Lock l;
          bool changed = false;
          if (verFreq == sentFreq && strcmp(rem.scan, "off") == 0 && fabsf(wantFreq - remoteFreq) > 0.0005f) {
            wantFreq = remoteFreq; changed = true;
          }
          if (verMode == sentMode) {
            for (int i = 0; i < 3; i++) if (remoteMode == MODES[i] && wantMode != i) {
              wantMode = i; stepIdx = defaultStepForMode(i); changed = true;
            }
          }
          if (verVol == sentVol && wantVolume != remoteVol) { wantVolume = remoteVol; changed = true; }
          if (verMute == sentMute) wantMuted = remoteMute;
          if (verServer == sentServer) wantServer = remoteServer;
          if (verOut == sentOut && wantOutput != remoteOut) { wantOutput = remoteOut; changed = true; }
          if (changed) { radioDirty = true; lastRadioSaveMs = millis(); }
        }
        setNetStatus(usbPcOk ? "PC: OK (CABLE)" : "PC: OK (Wi-Fi)");
      } else {
        failures++;
        Lock l; rem.ok = false; statusRev++;
      }
    }
    vTaskDelay((did && !ok ? 500 : 15) / portTICK_PERIOD_MS);
  }
}

// ------------------------------------------------------------------ settings changes (UI side)
static void changeFreq(float khz) {
  Lock l;
  wantFreq = constrain(khz, FREQ_MIN, FREQ_MAX);
  verFreq++;
  radioDirty = true;
  lastRadioSaveMs = millis();
}
static void changeVolume(int v) {
  Lock l;
  wantVolume = constrain(v, 0, 100);
  if (wantMuted && v > 0) { wantMuted = false; verMute++; }
  verVol++;
  radioDirty = true;
  lastRadioSaveMs = millis();
}
static void toggleMute() {
  Lock l;
  wantMuted = !wantMuted;
  verMute++;
}
static void changeMode(int m) {
  Lock l;
  int next = ((m % 3) + 3) % 3;
  if (next == wantMode) return;
  wantMode = next;
  stepIdx = defaultStepForMode(wantMode);
  verMode++;
  radioDirty = true;
  lastRadioSaveMs = millis();
}

static void clockService() {
  if (WiFi.status() != WL_CONNECTED) {
    clockNtpStarted = false;  // configure SNTP again after the next connection
  } else if (!clockNtpStarted) {
    configTzTime("JST-9", "pool.ntp.org", "time.google.com");
    clockNtpStarted = true;
    clockPollMs = 0;
  }
  uint32_t nowMs = millis();
  if (clockPollMs && nowMs - clockPollMs < 1000) return;
  clockPollMs = nowMs;
  time_t now = time(nullptr);
  if (now < 1700000000) return;  // NTP has not supplied a plausible date yet
  if (!clockSynced) { clockSynced = true; clockPersistPending = true; }
  if (clockPersistPending || !clockSaveMs || nowMs - clockSaveMs >= 10UL * 60UL * 1000UL) {
    if (prefs.begin("rdsdr", false)) {
      prefs.putLong64("epoch", (int64_t)now);
      prefs.end();
    }
    if (M5.Rtc.isEnabled()) {
      struct tm rt;
      localtime_r(&now, &rt);
      M5.Rtc.setDateTime(&rt);
    }
    clockPersistPending = false;
    clockSaveMs = nowMs;
  }
  struct tm local;
  localtime_r(&now, &local);
  static const char* const DAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  snprintf(clockDate, sizeof clockDate, "%s %02d/%02d", DAY[local.tm_wday], local.tm_mon + 1, local.tm_mday);
  snprintf(clockTime, sizeof clockTime, "%02d:%02d JST", local.tm_hour, local.tm_min);
  clockSynced = true;
}
static void changeServer(int delta) {
  Lock l;
  int n = max(1, rem.serverCount);
  wantServer = (((rem.serverIndex + delta) % n) + n) % n;
  rem.serverIndex = wantServer;
  verServer++;
}

static void changeOutput(int o) {
  Lock l;
  wantOutput = o ? 1 : 0;
  verOut++;  // pushed to the Mac by the net task
  radioDirty = true;
  lastRadioSaveMs = millis();
}

// ---- MANUAL / AUTO tuning (first-generation RADIO DAYS).  AUTO: the knob starts a scan in its direction on the Mac,
// which stops at a signal.  Esc or a knob push stops it.
static void scanRequest(int dir) {
  Lock l;
  wantScanDir = dir;
  verScan++;
}
static bool scanRunning() { Lock l; return strcmp(rem.scan, "off") != 0; }
static void setTuneAuto(bool a) {
  if (a == tuneAuto) return;
  tuneAuto = a;
  if (!a) scanRequest(0);                 // back to MANUAL: any scan ends where it is
  else autoHintUntil = millis() + 6000;
  statusRev++;
}
static void autoTurn(int detents) {       // a turn of the knob in AUTO: search in that direction
  int dir = detents > 0 ? 1 : -1;
  bool run = scanRunning();
  int cur; { Lock l; cur = wantScanDir; }
  if (run && cur == dir) return;          // already going that way
  scanRequest(dir);
}

// ------------------------------------------------------------------ Cardputer speaker: the Mac sends decoded audio, we play it
// netTask (core 0): TCP to the Mac -> frames -> ring buffer.   audioPlayTask (core 1): ring buffer -> M5.Speaker.
// Volume and mute are applied here (M5.Speaker master volume), so they act at once whatever is buffered.
static void audioNetTask(void*) {
  WiFiClient cli;
  uint8_t buf[512];
  uint32_t lastData = 0;
  uint32_t backoff = 500;
  bool wasUp = false;
  for (;;) {
    if (!auWanted) {
      if (cli.connected()) cli.stop();
      auConn = false;
      wasUp = false;
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    if (usbPcOk) {
      if (cli.connected()) cli.stop();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    String ip;
    bool macOk, macOut;
    { Lock l; ip = macIP; macOk = rem.ok; macOut = strcmp(rem.output, "cardputer") == 0; }
    if (WiFi.status() != WL_CONNECTED || ip.isEmpty() || !macOk || !macOut) {
      if (cli.connected()) cli.stop();
      auConn = false;
      auNetState = wasUp ? AU_RECONNECT : AU_WAIT;
      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }
    if (!cli.connected()) {
      auConn = false;
      auNetState = wasUp ? AU_RECONNECT : AU_CONNECTING;
      cli.stop();
      if (!cli.connect(ip.c_str(), AUDIO_PORT, 2000)) {
        vTaskDelay(backoff / portTICK_PERIOD_MS);
        backoff = min<uint32_t>(backoff * 2, 4000);
        continue;
      }
      cli.setNoDelay(true);
      cli.setTimeout(1500);
      cli.print("RDSDR-AUDIO 1 ");
      cli.print(TOKEN);
      cli.print("\n");
      String line = cli.readStringUntil('\n');
      line.trim();
      if (line != "OK") {  // BUSY (the Mac is not in Cardputer mode yet) or no answer: try again
        cli.stop();
        vTaskDelay(backoff / portTICK_PERIOD_MS);
        backoff = min<uint32_t>(backoff * 2, 4000);
        continue;
      }
      auAsm->reset();
      auNet.onReconnect();
      xStreamBufferReset(auRing);  // old audio would only add delay
      auPlayout.reset();
      lastData = millis();
      backoff = 500;
      wasUp = true;
      auConn = true;
      Serial.printf("[AUDIO] connected to %s:%u\n", ip.c_str(), AUDIO_PORT);
    }
    int n = cli.available();
    if (n > 0) {
      n = cli.read(buf, min(n, (int)sizeof(buf)));
      if (n <= 0) {
        Serial.println("[AUDIO] socket closed while data was pending, reconnecting");
        auConn = false;
        auNetState = AU_RECONNECT;
        cli.stop();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        continue;
      }
      lastData = millis();
      bool good = auAsm->feed(buf, (size_t)n, [&](const rdaudio::FrameHeader& h, const int16_t* pcm) {
        auNet.onFrame(h.seq, millis());
        auRate = h.rate;
        size_t fill = xStreamBufferBytesAvailable(auRing) / 2;
        if (auPlayout.admit((uint32_t)fill, h.nsamples)) xStreamBufferSend(auRing, pcm, h.nsamples * 2, 0);
      });
      if (!good) {  // protocol error: start again
        Serial.println("[AUDIO] protocol error, reconnecting");
        auConn = false;
        auNetState = AU_RECONNECT;
        cli.stop();
        vTaskDelay(50 / portTICK_PERIOD_MS);
      }
    } else {
      if (millis() - lastData > 2500) {  // the Mac went quiet: Wi-Fi or Mac trouble
        Serial.println("[AUDIO] no data for 2.5 s, reconnecting");
        auConn = false;
        auNetState = AU_RECONNECT;
        cli.stop();
        vTaskDelay(50 / portTICK_PERIOD_MS);
        continue;
      }
      vTaskDelay(3 / portTICK_PERIOD_MS);
    }
  }
}

static float auGain() {  // the same curve as the Mac's: (volume/100)^2; mute = 0
  if (wantMuted) return 0.0f;
  float v = wantVolume / 100.0f;
  return v * v;
}

static void audioPlayTask(void*) {
  int idx = 0;
  uint32_t lastGive = 0;
  for (;;) {
    if (!auSpeakerOn || !auRing) {
      auLevel = 0;
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }
    uint32_t fill = (uint32_t)(xStreamBufferBytesAvailable(auRing) / 2);
    size_t queued = M5.Speaker.isPlaying(AU_CH);  // 0 idle, 1 playing with room, 2 playing and full
    uint32_t give = auPlayout.onFeed(fill, queued < 2, queued == 0);
    if (give) {
      xStreamBufferReceive(auRing, auBuf[idx], give * 2, 0);
      double sum = 0;
      for (uint32_t i = 0; i < give; i++) sum += (double)auBuf[idx][i] * auBuf[idx][i];
      auLevel = sqrtf((float)(sum / give)) / 32768.0f * auGain();
      M5.Speaker.playRaw(auBuf[idx], give, auRate, false, 1, AU_CH, false);
      idx = (idx + 1) % 3;
      lastGive = millis();
    } else if (millis() - lastGive > 250) {
      auLevel = 0;
    }
    vTaskDelay(4 / portTICK_PERIOD_MS);
  }
}

// UI thread, every loop: start / stop the speaker side, follow the volume.
static void audioService() {
  static uint8_t appliedVol = 255;
  if (wantOutput == 1 && !auWanted) {
    auNoMem = false;
    if (!auRing) auRing = xStreamBufferCreate(AU_RING_BYTES, 1);
    if (!auAsm) auAsm = new (std::nothrow) rdaudio::FrameAssembler();
    if (!auRing || !auAsm) {  // not enough memory: stay on the Mac speaker
      auNoMem = true;
      Serial.printf("[AUDIO] no memory (heap %u, largest %u): staying on the Mac speaker\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      changeOutput(0);
      return;
    }
    xStreamBufferReset(auRing);
    auPlayout.reset();
    auto spk = M5.Speaker.config();
    spk.sample_rate = 48000;
    spk.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk);
    M5.Speaker.begin();
    auSpeakerOn = true;
    auWanted = true;
    appliedVol = 255;
    WiFi.setSleep(false);  // modem sleep adds jitter
    Serial.printf("[AUDIO] speaker on, heap %u, largest %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  } else if (wantOutput == 0 && auWanted) {
    auWanted = false;       // first the speaker, then the Mac is told (netTask) - never both at once
    auSpeakerOn = false;
    M5.Speaker.stop();
    M5.Speaker.end();
    WiFi.setSleep(true);
    auLevel = 0;
    Serial.println("[AUDIO] speaker off");
  }
  // setVolume(0) silences PCM but can leave the small amplifier's own hiss.
  // A real MUTE therefore powers the speaker path down, then starts it cleanly
  // again on unmute while the network stream remains connected.
  if (auWanted && wantMuted && auSpeakerOn) {
    auSpeakerOn = false;
    M5.Speaker.stop();
    M5.Speaker.end();
    auLevel = 0;
    if (auRing) xStreamBufferReset(auRing);
    auPlayout.reset();
    Serial.println("[AUDIO] hard mute (speaker off)");
  } else if (auWanted && !wantMuted && !auSpeakerOn) {
    auto spk = M5.Speaker.config();
    spk.sample_rate = 48000;
    spk.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk);
    M5.Speaker.begin();
    if (auRing) xStreamBufferReset(auRing);
    auPlayout.reset();
    appliedVol = 255;
    auSpeakerOn = true;
    Serial.println("[AUDIO] unmute (speaker on)");
  }
  if (auWanted) {
    uint8_t v = (uint8_t)lroundf(192.0f * auGain());  // 192 as U10's maximum (the small speaker distorts above)
    if (auSpeakerOn && v != appliedVol) { M5.Speaker.setVolume(v); appliedVol = v; }
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      Serial.printf("[AUDIO] %s fill %u ms, frames %u gaps %u maxgap %u ms, under %u over %u, reconn %u, heap %u\n", auStateName(auStateNow()),
                    auRing ? (unsigned)(xStreamBufferBytesAvailable(auRing) / 2 * 1000 / 12000) : 0, (unsigned)auNet.frames, (unsigned)auNet.gaps, (unsigned)auNet.maxGapMs,
                    (unsigned)auPlayout.underruns, (unsigned)auPlayout.overruns, (unsigned)auNet.reconnects, (unsigned)ESP.getFreeHeap());
    }
  }
}

// ------------------------------------------------------------------ drawing helpers
static void txt(int x, int y, int size, uint16_t fg, const char* s, uint16_t bg = COL_BG) {
  lcd.setFont(&fonts::Font0);  // always the built-in font: a GFX font set elsewhere must never leak into this text
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextPadding(0);
  lcd.setTextSize(size);
  lcd.setTextColor(fg, bg);
  lcd.setCursor(x, y);
  lcd.print(s);
}
static void header(const char* title) {
  lcd.fillScreen(COL_BG);
  txt(4, 3, 1, COL_AMBER, title);
  txt(188, 3, 1, COL_DIM, BUILD_TAG);
  lcd.drawFastHLine(0, 14, 240, COL_DIM);
}

static Screen screen = SC_RADIO;
static bool screenDirty = true;
static void setScreen(Screen s) { screen = s; reportedScreen = (uint8_t)s; screenDirty = true; }

// ------------------------------------------------------------------ radio screen
static const char* const MODE_LABEL[] = {"AM", "USB", "LSB"};
static String entry, entryMsg;  // text entry screens

static void drawBar(int x, int y, int w, int h, float frac, uint16_t col) {
  frac = constrain(frac, 0.0f, 1.0f);
  int fw = (int)(w * frac);
  lcd.drawRect(x, y, w, h, COL_DIM);
  lcd.fillRect(x + 1, y + 1, fw > 2 ? fw - 2 : 0, h - 2, col);
  lcd.fillRect(x + 1 + (fw > 2 ? fw - 2 : 0), y + 1, w - 2 - (fw > 2 ? fw - 2 : 0), h - 2, COL_BG);
}

// ==== ANALOG BEGIN (v1.10: a silver metal receiver drawn with M5GFX primitives; drawing only, no bitmaps) ====
// Layout (240 x 135): title beside a top-aligned five-row indicator panel | two low, wide meters | full-width scale | black/green display.
// Only what changed is redrawn.  Two small sprites are reused: the scale glass (224 x 21) and ONE meter face (42 x 32),
// drawn and pushed first for SIGNAL, then for VOL.  The red line on the scale is fixed; the scale slides under it.
static constexpr uint16_t rgb565c(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
static constexpr int DIAL_W = 228, DIAL_H = 22, DIAL_X = 6, DIAL_Y = 57;
static constexpr float DIAL_PX_PER_STEP = 3.0f;
static constexpr int DIAL_LABEL_EVERY = 20;  // a number every 20 ticks (60 px)
static constexpr int METER_W = 45, METER_H = 32, METER_Y = 16, SIG_X = 6, VOL_X = 59;
static constexpr int IND_X = 113, IND_Y = 4, IND_W = 123, IND_H = 46;
static constexpr int GLASS_X = 6, GLASS_Y = 90, GLASS_W = 155, GLASS_H = 34;
static constexpr int CLOCK_X = 167, CLOCK_W = 67;
static constexpr uint16_t PN_INK = rgb565c(34, 36, 34), WELL = rgb565c(23, 25, 24), WELL_LIT = rgb565c(238, 236, 226);
static constexpr uint16_t IV_INK = rgb565c(30, 31, 28), IV_GREY = rgb565c(56, 57, 52);
static constexpr uint16_t DL_RED = rgb565c(214, 40, 32);
// The five indicator rows follow the playful colour coding printed on M5Stack hardware.
// Link lamps keep their green/amber/red status semantics; these colours group the labels only.
static constexpr uint16_t ROW_OUT = rgb565c(35, 82, 176), ROW_LINK = rgb565c(24, 120, 66);
static constexpr uint16_t ROW_TUNE = rgb565c(190, 98, 18), ROW_MODE = rgb565c(184, 48, 42), ROW_STEP = rgb565c(120, 60, 164);
static constexpr uint16_t ROW_RULE = rgb565c(190, 191, 185);
static constexpr uint16_t GL_INK = rgb565c(92, 255, 74);       // fluorescent green on the black display
static constexpr uint16_t GL_BRIGHT = rgb565c(176, 255, 142);  // highlighted receive mode
static constexpr uint16_t GL_DIM = rgb565c(48, 150, 44);       // secondary state text

static uint16_t lerp565(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, float t) {
  return rgb565c((uint8_t)(r0 + (r1 - r0) * t), (uint8_t)(g0 + (g1 - g0) * t), (uint8_t)(b0 + (b1 - b0) * t));
}

// the silver panel: a warm light grey, darker towards the bottom, with faint brushed lines (every other row)
static uint16_t metalColor(int y) {
  float t = constrain(y, 0, 134) / 134.0f;
  int r, g, b;
  if (t < 0.45f) { float u = t / 0.45f; r = 224 + (int)((185 - 224) * u); g = 221 + (int)((183 - 221) * u); b = 211 + (int)((175 - 211) * u); }
  else { float u = (t - 0.45f) / 0.55f; r = 185 + (int)((140 - 185) * u); g = 183 + (int)((141 - 183) * u); b = 175 + (int)((137 - 175) * u); }
  if ((y & 1) == 0) { r = min(r + 5, 255); g = min(g + 5, 255); b = min(b + 5, 255); }
  return rgb565c((uint8_t)r, (uint8_t)g, (uint8_t)b);
}
static void metalFill(int x, int y, int w, int h) {
  for (int j = 0; j < h; j++) lcd.drawFastHLine(x, y + j, w, metalColor(y + j));
}

// black glass with a faint green cast and a very small reflection along the top
static uint16_t glassColor(int y) {
  int yy = constrain(y - GLASS_Y, 0, GLASS_H - 1);
  float t = (float)yy / (GLASS_H - 1);
  uint16_t c = lerp565(7, 18, 9, 1, 7, 3, t);
  if (yy < 2) c = yy == 0 ? rgb565c(22, 42, 24) : rgb565c(12, 28, 15);
  return c;
}
static constexpr uint16_t GLASS_EDGE = rgb565c(28, 54, 30);  // a thin green-black line inside the metal cut-out
static void glassFill(int y0, int y1) {  // rows y0 .. y1-1 of the radio-information glass
  for (int y = max(y0, GLASS_Y + 1); y < min(y1, GLASS_Y + GLASS_H - 1); y++) lcd.drawFastHLine(GLASS_X + 1, y, GLASS_W - 2, glassColor(y));
}

// a dark opening in the panel (rounded corners, a light lip along the lower and right edge)
static void drawWell(int x, int y, int w, int h) {
  lcd.fillRect(x, y, w, h, WELL);
  lcd.drawFastHLine(x + 1, y + h, w - 1, WELL_LIT);
  lcd.drawFastVLine(x + w, y + 1, h, WELL_LIT);
  lcd.drawPixel(x, y, metalColor(y)); lcd.drawPixel(x + w - 1, y, metalColor(y));
  lcd.drawPixel(x, y + h - 1, metalColor(y + h - 1)); lcd.drawPixel(x + w - 1, y + h - 1, metalColor(y + h - 1));
}

// A white, slightly rounded insert sunk into the metal (shared by the meters' companion panel and TUNE knob).
static void drawWhitePanel(int x, int y, int w, int h) {
  const uint16_t face = rgb565c(247, 247, 241), edge = rgb565c(58, 61, 58);
  lcd.fillRect(x + 1, y + 2, w - 2, h - 4, face);
  lcd.fillRect(x + 2, y + 1, w - 4, h - 2, face);
  lcd.drawRoundRect(x, y, w, h, 3, edge);
  lcd.drawFastHLine(x + 3, y + 2, w - 6, rgb565c(132, 134, 128));
  lcd.drawFastVLine(x + 2, y + 3, h - 6, rgb565c(132, 134, 128));
  lcd.drawFastHLine(x + 3, y + h - 2, w - 6, rgb565c(255, 255, 250));
  lcd.drawFastVLine(x + w - 2, y + 3, h - 6, rgb565c(255, 255, 250));
}

// Tiny slotted screw head for the clock module's separate metal bezel.
static void drawScrew(int cx, int cy, bool slash) {
  const uint16_t rim = rgb565c(68, 70, 67), head = rgb565c(204, 204, 197), slot = rgb565c(82, 84, 81);
  lcd.fillCircle(cx, cy, 2, rim);
  lcd.fillCircle(cx, cy, 1, head);
  if (slash) lcd.drawLine(cx - 1, cy + 1, cx + 1, cy - 1, slot);
  else lcd.drawLine(cx - 1, cy - 1, cx + 1, cy + 1, slot);
}

// text without a background box (it sits on the metal or the glass)
static void txtT(int x, int y, uint16_t fg, const char* s) {
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextPadding(0);
  lcd.setTextSize(1);
  lcd.setTextColor(fg);
  lcd.setCursor(x, y);
  lcd.print(s);
}

// a small lamp.  col is one of COL_OK / COL_WARN / COL_BAD, anything else = off.
static void drawJewel(int cx, int cy, int r, uint16_t col) {
  uint16_t base, hi;
  if (col == COL_OK) { base = rgb565c(70, 205, 60); hi = rgb565c(225, 255, 205); }
  else if (col == COL_WARN) { base = rgb565c(238, 196, 34); hi = rgb565c(255, 250, 190); }
  else if (col == COL_BAD) { base = rgb565c(214, 44, 36); hi = rgb565c(255, 190, 170); }
  else { base = rgb565c(52, 55, 53); hi = rgb565c(112, 116, 112); }
  lcd.fillCircle(cx, cy, r + 1, rgb565c(40, 44, 42));
  lcd.fillCircle(cx, cy, r, base);
  lcd.drawPixel(cx - 1, cy - 1, hi);
}

// posKHz: the frequency under the red line (eases towards the set frequency); step: kHz per tick.
static void drawDialSprite(M5Canvas& sp, float posKHz, float step) {
  for (int y = 0; y < DIAL_H; y++) {  // pale glass, a little darker at the bottom
    float t = (float)y / (DIAL_H - 1);
    sp.drawFastHLine(0, y, DIAL_W, t < 0.7f ? lerp565(255, 255, 255, 243, 242, 232, t / 0.7f) : lerp565(243, 242, 232, 212, 212, 201, (t - 0.7f) / 0.3f));
  }
  // A shallow inner shadow on the top and left makes the glass sit behind the metal face.
  sp.fillRectAlpha(0, 0, DIAL_W, 2, 58, rgb565c(70, 72, 68));
  sp.fillRectAlpha(0, 0, 2, DIAL_H, 58, rgb565c(70, 72, 68));
  const int cx = DIAL_W / 2;
  const double stepD = lround(step * 10.0) / 10.0;  // 0.1 / 1 / 5 / 10 exactly (no float noise in the labels)
  const double pxPerKHz = (double)DIAL_PX_PER_STEP / stepD;
  sp.setFont(&fonts::Font0);
  sp.setTextSize(1);
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(IV_GREY);
  long i0 = (long)floor((posKHz - cx / pxPerKHz) / stepD) - 1;
  long i1 = (long)ceil((posKHz + (DIAL_W - cx) / pxPerKHz) / stepD) + 1;
  for (long i = i0; i <= i1; i++) {
    double f = (double)i * stepD;
    if (f < FREQ_MIN - 0.001 || f > FREQ_MAX + 0.001) continue;  // the scale ends where the tuning range ends
    int x = cx + (int)lround((f - posKHz) * pxPerKHz);
    if (x < 0 || x >= DIAL_W) continue;
    long m20 = ((i % DIAL_LABEL_EVERY) + DIAL_LABEL_EVERY) % DIAL_LABEL_EVERY;
    int len = (m20 == 0) ? 7 : (i % 10 == 0 ? 5 : (i % 5 == 0 ? 4 : 3));
    sp.drawFastVLine(x, 1, len, IV_INK);
    if (m20 == 0) {
      char b[12];
      if (fabs(f - round(f)) < 0.0005) snprintf(b, sizeof(b), "%ld", (long)lround(f)); else snprintf(b, sizeof(b), "%.1f", f);
      sp.drawString(b, x, 9);
    }
  }
  // the ends of the glass fall into shadow
  static const uint8_t VA[4] = {80, 52, 28, 12};
  for (int k = 0; k < 4; k++) {
    sp.fillRectAlpha(k * 4, 0, 4, DIAL_H, VA[k], rgb565c(40, 36, 28));
    sp.fillRectAlpha(DIAL_W - 4 - k * 4, 0, 4, DIAL_H, VA[k], rgb565c(40, 36, 28));
  }
  // the fixed red line: two pixels wide beside the ticks, one pixel in the number zone so the digits stay readable
  sp.drawFastHLine(0, 0, DIAL_W, rgb565c(150, 150, 140));
  sp.drawFastHLine(0, DIAL_H - 1, DIAL_W, rgb565c(150, 150, 140));
  sp.fillRect(cx - 1, 0, 2, 9, DL_RED);
  sp.drawFastVLine(cx - 1, 9, DIAL_H - 9, DL_RED);
  sp.setFont(&fonts::Font0);
  sp.setTextSize(1);
  sp.setTextDatum(textdatum_t::top_left);
}

// the needle sweeps 90 degrees, 45 left and right of vertical; v: 0..100.  The pivot sits low in the face.
static constexpr int MT_PX = METER_W / 2, MT_PY = METER_H - 1, MT_R = 25;
static constexpr float MT_SPAN = 55.0f;
static constexpr float MT_K = 1.0f;
static void meterPoint(float v, float r, int& x, int& y) {
  float a = (-MT_SPAN + 2.0f * MT_SPAN * constrain(v, 0.0f, 100.0f) / 100.0f) * (float)M_PI / 180.0f;
  x = MT_PX + (int)lroundf(sinf(a) * r * MT_K);
  y = MT_PY - (int)lroundf(cosf(a) * r);
}
// the scale line from value v0 to v1 (two pixels thick)
static void meterArc(M5Canvas& sp, float v0, float v1, float r, uint16_t col) {
  int px, py;
  meterPoint(v0, r, px, py);
  for (float v = v0 + 1.0f; v <= v1 + 0.01f; v += 1.0f) {
    int x, y;
    meterPoint(fminf(v, v1), r, x, y);
    sp.drawLine(px, py, x, y, col);
    sp.drawLine(px, py + 1, x, y + 1, col);
    px = x; py = y;
  }
}

// isVol: VOLUME meter (needle = volume setting 0..100, never the audio level); otherwise SIGNAL (S1..S9, then over S9).
// value: needle position 0..100 (already eased).  muted: VOLUME shows MUTE instead of its name.
static void drawMeterSprite(M5Canvas& sp, bool isVol, float value, bool muted) {
  for (int y = 0; y < METER_H; y++) {  // white-ish face, shaded towards the bottom
    float t = (float)y / (METER_H - 1);
    sp.drawFastHLine(0, y, METER_W, t < 0.72f ? lerp565(252, 252, 248, 243, 242, 232, t / 0.72f) : lerp565(243, 242, 232, 212, 212, 201, (t - 0.72f) / 0.28f));
  }
  sp.drawRect(0, 0, METER_W, METER_H, rgb565c(104, 105, 96));
  const uint16_t TICK = rgb565c(40, 40, 36), RED = rgb565c(190, 48, 40);
  const int nTicks = isVol ? 11 : 12;
  const int redFrom = isVol ? 8 : 9;  // VOL: the top fifth; SIGNAL: from S9 on
  meterArc(sp, 0.0f, 100.0f, MT_R, TICK);
  meterArc(sp, 100.0f * redFrom / (nTicks - 1), 100.0f, MT_R, RED);
  for (int k = 0; k < nTicks; k++) {
    bool major = isVol ? (k % 5 == 0) : (k % 2 == 1);
    float v = 100.0f * k / (nTicks - 1);
    int x0, y0, x1, y1;
    meterPoint(v, MT_R, x0, y0);
    meterPoint(v, MT_R - (major ? 5 : 3), x1, y1);
    sp.drawLine(x0, y0, x1, y1, k >= redFrom ? RED : TICK);
  }
  sp.setFont(&fonts::Font0);
  sp.setTextSize(1);
  sp.setTextDatum(textdatum_t::top_center);
  sp.setTextColor(muted ? RED : IV_INK);
  const char* name = muted ? "MUTE" : (isVol ? "VOLUME" : "SIGNAL");
  sp.drawString(name, MT_PX, 21);
  // the red needle (two pixels wide) over the label, and its hub
  int nx, ny;
  meterPoint(value, MT_R - 4, nx, ny);  // the needle stops a little short of the scale
  int tx, ty;
  meterPoint(value, 5.0f, tx, ty);
  sp.drawLine(tx, ty, nx, ny, DL_RED);
  sp.drawLine(tx + 1, ty, nx + 1, ny, DL_RED);
  sp.fillCircle(MT_PX, MT_PY + 1, 3, rgb565c(150, 34, 28));  // the hub, half hidden by the bottom edge
  // Inner top/left shadow: the face is recessed, not pasted onto a thick black gasket.
  sp.fillRectAlpha(1, 1, METER_W - 2, 2, 70, rgb565c(82, 84, 80));
  sp.fillRectAlpha(1, 1, 2, METER_H - 2, 70, rgb565c(82, 84, 80));
  sp.setTextDatum(textdatum_t::top_left);
}
// ==== ANALOG END ====

// ---- screen-level state of the analogue display
static M5Canvas* dialSprite = nullptr;
static M5Canvas* meterSprite = nullptr;
static bool analogReady = false;

static void analogBegin() {  // called once at boot, before Wi-Fi, so the blocks are contiguous
  dialSprite = new (std::nothrow) M5Canvas(&lcd);
  meterSprite = new (std::nothrow) M5Canvas(&lcd);
  if (dialSprite && meterSprite) {
    dialSprite->setColorDepth(16);
    meterSprite->setColorDepth(16);
    analogReady = dialSprite->createSprite(DIAL_W, DIAL_H) && meterSprite->createSprite(METER_W, METER_H);
  }
  if (!analogReady) {  // not enough memory: the simple radio screen is used
    if (dialSprite) { dialSprite->deleteSprite(); delete dialSprite; dialSprite = nullptr; }
    if (meterSprite) { meterSprite->deleteSprite(); delete meterSprite; meterSprite = nullptr; }
  }
  Serial.printf("[ANALOG] %s, heap %u, largest %u\n", analogReady ? "ready" : "no memory", (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// The status line: which link is the problem?  W = Wi-Fi, M = Mac, K = KiwiSDR.
static void statusText(char* out, size_t n, uint16_t* col) {
  Lock l;
  if (WiFi.status() != WL_CONNECTED && (!rem.ok || (wantOutput == 1 && !usbPcOk))) {
    if (savedCount == 0) snprintf(out, n, "Wi-Fi: not set. Press W");
    else if (wifiState == W_TRY) snprintf(out, n, "Wi-Fi: connecting %.14s", saved[wifiOrder[wifiOrderPos]].ssid);
    else snprintf(out, n, "Wi-Fi: no connection (retry)");
    *col = COL_BAD;
  } else if (!rem.ok) {
    snprintf(out, n, "%.38s", netStatus);
    *col = strstr(netStatus, "OK") ? COL_OK : COL_WARN;
  } else if (strcmp(rem.link, "connected") == 0) {
    uint8_t as = auStateNow();
    if ((wantOutput == 1 && as != AU_PLAYING) || auNoMem) {  // the Cardputer speaker is chosen but not playing yet: say why
      if (auNoMem) snprintf(out, n, "ADV audio: no memory - Mac speaker");
      else snprintf(out, n, "ADV audio: %s", auStateName(as));
      *col = as == AU_NOMEM ? COL_BAD : COL_WARN;
    } else {
      snprintf(out, n, "Kiwi OK: %.20s", rem.server);
      *col = COL_OK;
    }
  } else if (strcmp(rem.link, "error") == 0) {
    snprintf(out, n, "Kiwi: %.32s", rem.error[0] ? rem.error : "error (retrying)");
    *col = COL_BAD;
  } else {
    snprintf(out, n, "Kiwi: connecting %.16s", rem.server);
    *col = COL_WARN;
  }
}

static void drawRadioSimple(bool full) {
  static float dFreq = -1; static int dMode = -1, dVol = -1, dStep = -1; static bool dMute = false;
  static float dS = -1, dLvl = -1; static int dOver = -1; static char dStatus[44] = ""; static uint16_t dCol = 0;
  static bool dLink = false; static uint32_t dRev = 0;
  float f; int mo, vol; bool mu, linked; float s, lvl; int over; bool pend;
  {
    Lock l;
    f = wantFreq; mo = wantMode; vol = wantVolume; mu = wantMuted;
    linked = rem.ok && strcmp(rem.link, "connected") == 0;
    s = linked ? rem.s : 0; over = linked ? rem.sOver : 0; lvl = linked ? rem.outLevel : 0;
    pend = (verFreq != 0) && !synced;
  }
  if (full) {
    lcd.fillScreen(COL_BG);
    txt(4, 3, 1, COL_AMBER, "RADIO DAYS SDR");
    lcd.drawFastHLine(0, 14, 240, COL_DIM);
    txt(4, 83, 1, COL_DIM, "VOL");
    txt(4, 96, 1, COL_DIM, "S");
    txt(4, 108, 1, COL_DIM, "OUT");
    dFreq = -1; dMode = -1; dVol = -1; dStep = -1; dS = -1; dLvl = -1; dOver = -1; dStatus[0] = 0; dRev = 0; dLink = !linked;
  }
  if (f != dFreq) {
    char b[24];
    snprintf(b, sizeof(b), "%9.1f", f);
    txt(4, 22, 3, COL_AMBER, b);
    txt(190, 36, 2, COL_AMBER, "kHz");
    dFreq = f;
  }
  if (mo != dMode || stepIdx != dStep) {
    char b[32];
    snprintf(b, sizeof(b), "%-3s   STEP %5.1f kHz  ", MODE_LABEL[mo], STEPS[stepIdx]);
    txt(4, 56, 1, COL_TEXT, b);
    dMode = mo; dStep = stepIdx;
  }
  if (vol != dVol || mu != dMute || full) {
    char b[16];
    if (mu) snprintf(b, sizeof(b), "MUTE %3d", vol); else snprintf(b, sizeof(b), "VOL  %3d", vol);
    txt(150, 56, 1, mu ? COL_BAD : COL_TEXT, b);
    drawBar(28, 81, 206, 10, mu ? 0.0f : vol / 100.0f, mu ? COL_BAD : COL_AMBER);
    dVol = vol; dMute = mu;
  }
  if (s != dS || over != dOver || linked != dLink) {
    drawBar(28, 94, 206, 10, linked ? (s + (over ? over / 20.0f : 0)) / 11.0f : 0.0f, linked ? COL_OK : COL_DIM);
    char b[24];
    if (!linked) snprintf(b, sizeof(b), "S --      ");
    else if (over) snprintf(b, sizeof(b), "S9+%d  ", over);
    else snprintf(b, sizeof(b), "S%.0f    ", s);
    txt(4, 70, 1, linked ? COL_OK : COL_DIM, b);
    dS = s; dOver = over; dLink = linked;
  }
  if (fabsf(lvl - dLvl) > 0.004f || full) {
    drawBar(28, 106, 206, 8, sqrtf(constrain(lvl * 2.0f, 0.0f, 1.0f)), COL_OK);
    dLvl = lvl;
  }
  char st[44]; uint16_t col;
  statusText(st, sizeof(st), &col);
  if (strcmp(st, dStatus) != 0 || col != dCol || full) {
    lcd.fillRect(0, 118, 240, 17, COL_BG);
    txt(4, 121, 1, col, st);
    strlcpy(dStatus, st, sizeof(dStatus));
    dCol = col;
  }
  (void)pend; (void)dRev;
}

// ---- the metal receiver (v1.10)
static void drawLamp(int x, int y, char letter, uint16_t col) {  // used by the "PC NOT CONNECTED" notice
  txt(x, y, 1, COL_DIM, String(letter).c_str());
  lcd.fillCircle(x + 10, y + 3, 3, col);
}

// everything that never changes: panel, frame, openings, fixed engraving
static void drawChrome() {
  metalFill(0, 0, 240, 135);
  lcd.drawRect(0, 0, 240, 135, rgb565c(86, 89, 85));
  lcd.drawRect(1, 1, 238, 133, rgb565c(122, 124, 119));
  lcd.drawRect(3, 3, 234, 129, rgb565c(236, 234, 224));
  txtT(10, 5, PN_INK, "RADIO DAYS SDR");
  txtT(11, 5, PN_INK, "RADIO DAYS SDR");  // bold engraved title

  // Two low, wide analogue instruments on the left. Their shorter windows avoid the tall, stretched look.
  drawWell(5, 15, 47, 34);
  drawWell(58, 15, 47, 34);

  // One recessed white indicator module on the right, divided into five colour-coded rows.
  drawWhitePanel(IND_X, IND_Y, IND_W, IND_H);
  // Thin separators make the five horizontal groups readable at a glance.
  for (int y = 13; y <= 40; y += 9) lcd.drawFastHLine(IND_X + 3, y, IND_W - 6, ROW_RULE);
  txtT(125, 6, ROW_OUT, "OUT PC"); txtT(182, 6, ROW_OUT, "OUT ADV");
  txtT(125, 15, ROW_LINK, "WiFi"); txtT(163, 15, ROW_LINK, "PC");    txtT(201, 15, ROW_LINK, "Kiwi");
  txtT(125, 24, ROW_TUNE, "MANUAL"); txtT(182, 24, ROW_TUNE, "AUTO");
  txtT(125, 33, ROW_MODE, "AM");   txtT(163, 33, ROW_MODE, "USB");  txtT(201, 33, ROW_MODE, "LSB");
  txtT(125, 42, ROW_STEP, ".1");   txtT(153, 42, ROW_STEP, "1");    txtT(175, 42, ROW_STEP, "5");
  txtT(197, 42, ROW_STEP, "10");   txtT(215, 42, ROW_STEP, "kHz");

  drawWell(5, 56, 230, 24);   // full-width tuning scale, one-pixel dark edge around the glass
  // Separate radio and clock modules: thin edges, with metal visible between them.
  drawWell(5, 89, 157, 36);
  drawWell(166, 89, 69, 36);
  glassFill(GLASS_Y, GLASS_Y + GLASS_H);
  lcd.drawRect(GLASS_X, GLASS_Y, GLASS_W, GLASS_H, GLASS_EDGE);
  for (int y = GLASS_Y + 1; y < GLASS_Y + GLASS_H - 1; y++) lcd.drawFastHLine(CLOCK_X + 1, y, CLOCK_W - 2, glassColor(y));
  lcd.drawRect(CLOCK_X, GLASS_Y, CLOCK_W, GLASS_H, GLASS_EDGE);
  // The wide radio LCD gets a Cartier-like screwed bezel: four fasteners above and four below.
  lcd.drawRoundRect(3, 82, 161, 50, 3, rgb565c(70, 73, 70));
  static const int MAIN_SCREW_X[4] = {12, 59, 106, 153};
  for (int i = 0; i < 4; i++) {
    drawScrew(MAIN_SCREW_X[i], 85, (i & 1) != 0);
    drawScrew(MAIN_SCREW_X[i], 128, (i & 1) == 0);
  }
  // The clock remains a digital LCD, mounted as a distinct add-on module with a screwed metal bezel.
  lcd.drawRoundRect(163, 82, 75, 50, 3, rgb565c(70, 73, 70));
  drawScrew(165, 85, false); drawScrew(235, 85, true);
  drawScrew(165, 128, true); drawScrew(235, 128, false);
}

// status colours remain luminous on the black glass
static uint16_t lcdInk(uint16_t col) {
  if (col == COL_BAD) return rgb565c(255, 82, 68);
  if (col == COL_WARN) return rgb565c(255, 210, 72);
  if (col == COL_DIM) return GL_DIM;
  return GL_INK;
}
static constexpr int LCD_MAIN_W = GLASS_W;
// One line in the left side of the modern display. The clock has its own two-line module on the right.
static void drawLcdLine(int line, const char* s, uint16_t ink) {
  const int y0 = line == 0 ? GLASS_Y + 1 : GLASS_Y + 18, y1 = line == 0 ? GLASS_Y + 18 : GLASS_Y + GLASS_H;
  for (int y = y0; y < y1; y++) lcd.drawFastHLine(GLASS_X + 1, y, LCD_MAIN_W - 2, glassColor(y));
  int w = 6 * (int)strlen(s);
  txtT(GLASS_X + (LCD_MAIN_W - w) / 2, line == 0 ? GLASS_Y + 6 : GLASS_Y + 22, ink, s);
}

// Compact 3x5 "JST" mark: small enough to share the G-SHOCK-like upper complication row with the full date.
static void drawTinyJst(int x, int y, uint16_t col) {
  static const uint8_t glyph[3][5] = {
    {7, 2, 2, 2, 6},  // J
    {7, 4, 7, 1, 7},  // S
    {7, 2, 2, 2, 2},  // T
  };
  for (int c = 0; c < 3; c++)
    for (int row = 0; row < 5; row++)
      for (int bit = 0; bit < 3; bit++)
        if (glyph[c][row] & (1 << (2 - bit))) lcd.drawPixel(x + c * 4 + bit, y + row, col);
}

static void drawClockPanel(const char* date, const char* tm) {
  for (int y = GLASS_Y + 1; y < GLASS_Y + GLASS_H - 1; y++)
    lcd.drawFastHLine(CLOCK_X + 1, y, CLOCK_W - 2, glassColor(y));
  // Digital-watch hierarchy: compact weekday, date and timezone above a large time readout.
  char dow[4] = {date[0], date[1], date[2], 0};
  txtT(CLOCK_X + 2, GLASS_Y + 3, GL_INK, dow);
  txtT(CLOCK_X + 22, GLASS_Y + 3, GL_INK, date + 4);
  drawTinyJst(CLOCK_X + CLOCK_W - 12, GLASS_Y + 4, GL_BRIGHT);
  lcd.drawFastHLine(CLOCK_X + 2, GLASS_Y + 13, CLOCK_W - 4, GLASS_EDGE);
  char hhmm[6];
  strlcpy(hhmm, tm, sizeof hhmm);  // "07:58" from "07:58 JST"; the tiny mark above carries the timezone
  // Use the same large, bold built-in bitmap style as the frequency.  At
  // text size 2 its strokes are two pixels thick and all five characters fit.
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextPadding(0);
  lcd.setTextSize(2);
  lcd.setTextColor(GL_BRIGHT);
  lcd.setCursor(CLOCK_X + (CLOCK_W - 60) / 2, GLASS_Y + 17);
  lcd.print(hhmm);
  lcd.setTextSize(1);
}

// Line 1 contains only the exact tuned frequency.  All discrete choices live in the indicator module.
static void drawLcdLine1(float freq) {
  for (int y = GLASS_Y + 1; y < GLASS_Y + 18; y++)
    lcd.drawFastHLine(GLASS_X + 1, y, LCD_MAIN_W - 2, glassColor(y));
  char b[16];
  snprintf(b, sizeof b, "%.1f", freq);
  // A doubled built-in bitmap font is deliberately used here instead of the
  // custom seven-segment frequency renderer.  Its two-pixel strokes are bold
  // and readable on the real 240x135 panel, while keeping this code close to
  // the v1.10 image that installed successfully through M5Launcher.
  const int numberW = 12 * (int)strlen(b), unitW = 18, gap = 4;
  int x = GLASS_X + (LCD_MAIN_W - numberW - gap - unitW) / 2;
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextPadding(0);
  lcd.setTextSize(2);
  lcd.setTextColor(GL_BRIGHT);
  lcd.setCursor(x, GLASS_Y + 2);
  lcd.print(b);
  lcd.setTextSize(1);
  txtT(x + numberW + gap, GLASS_Y + 6, GL_INK, "kHz");
}

static void drawRadioAnalog(bool full) {
  static float dialPos = 0, volNeedle = 0, sigNeedle = 0;  // eased display positions
  static float dFreq = -1, dPos = -1, dStep = -1, dVolN = -1, dSigN = -1;
  static int dMode = -1, dLamps = -1, dOut = -1, dTuneMode = -1, dStepLamp = -1;
  static bool dMute = false, started = false;
  static char dLine2[28] = "", dClockDate[12] = "", dClockTime[12] = "";
  static uint16_t dCol = 0;
  static uint32_t lastFrame = 0;
  float f, s; int mo, vol, over; bool mu, linked; int outSel; uint8_t as;
  bool autoMode, scanning, scanUp; char scanMsg[12]; float scanExcess; uint32_t scanDoneMs;
  uint16_t lw, lm, lk;
  {
    Lock l;
    f = wantFreq; mo = wantMode; vol = wantVolume; mu = wantMuted;
    linked = rem.ok && strcmp(rem.link, "connected") == 0;
    s = linked ? rem.s : 0; over = linked ? rem.sOver : 0;
    outSel = wantOutput; as = auStateNow();
    autoMode = tuneAuto;
    scanning = strcmp(rem.scan, "off") != 0; scanUp = strcmp(rem.scan, "down") != 0;
    strlcpy(scanMsg, rem.scanMsg, sizeof scanMsg); scanExcess = rem.scanExcess; scanDoneMs = rem.scanDoneMs;
    bool wifi = WiFi.status() == WL_CONNECTED;
    lw = wifi ? COL_OK : (wifiState == W_TRY ? COL_WARN : COL_BAD);
    lm = rem.ok ? COL_OK : (wifi ? COL_WARN : COL_DIM);
    lk = !rem.ok ? COL_DIM : (strcmp(rem.link, "connected") == 0 ? COL_OK : (strcmp(rem.link, "error") == 0 ? COL_BAD : COL_WARN));
  }
  const float step = STEPS[stepIdx];
  // SIGNAL needle target: the receiver's S value (S9 = 9/11 of the scale), zero while not connected
  const float sigTarget = linked ? constrain(100.0f * (s + (over ? over / 20.0f : 0.0f)) / 11.0f, 0.0f, 100.0f) : 0.0f;
  if (full || !started) {
    started = true;
    dialPos = f;                       // no sweep when the screen opens
    volNeedle = mu ? 0.0f : (float)vol;
    sigNeedle = sigTarget;
    drawChrome();
    dFreq = dPos = dStep = dVolN = dSigN = -1; dMode = dLamps = dOut = dTuneMode = dStepLamp = -1; dLine2[0] = 0;
    dMute = !mu;
    lastFrame = 0;
  }
  // Indicator row 1: selected output is amber; an ADV failure is red.
  int outKey = outSel * 16 + as;
  if (outKey != dOut) {
    drawJewel(119, 9, 2, outSel == 0 ? COL_WARN : COL_DIM);
    drawJewel(176, 9, 2, outSel == 1 ? (as == AU_NOMEM ? COL_BAD : COL_WARN) : COL_DIM);
    dOut = outKey;
  }
  // Indicator row 2: link health keeps the traffic-light meaning.
  int lamps = (int)(((uint32_t)lw * 31u + lm) * 31u + lk);
  if (lamps != dLamps) {
    drawJewel(119, 18, 2, lw); drawJewel(157, 18, 2, lm); drawJewel(195, 18, 2, lk);
    dLamps = lamps;
  }
  // Indicator rows 3-5: amber means a discrete user selection, dark means not selected.
  int tuneMode = autoMode ? 1 : 0;
  if (tuneMode != dTuneMode) {
    drawJewel(119, 27, 2, !autoMode ? COL_WARN : COL_DIM);
    drawJewel(176, 27, 2, autoMode ? COL_WARN : COL_DIM);
    dTuneMode = tuneMode;
  }
  if (mo != dMode) {
    drawJewel(119, 36, 2, mo == 0 ? COL_WARN : COL_DIM);
    drawJewel(157, 36, 2, mo == 1 ? COL_WARN : COL_DIM);
    drawJewel(195, 36, 2, mo == 2 ? COL_WARN : COL_DIM);
    dMode = mo;
  }
  if (stepIdx != dStepLamp) {
    drawJewel(119, 45, 2, stepIdx == 0 ? COL_WARN : COL_DIM);
    drawJewel(147, 45, 2, stepIdx == 1 ? COL_WARN : COL_DIM);
    drawJewel(169, 45, 2, stepIdx == 2 ? COL_WARN : COL_DIM);
    drawJewel(191, 45, 2, stepIdx == 3 ? COL_WARN : COL_DIM);
    dStepLamp = stepIdx;
  }
  // frame pacing: the scale and the needles ease towards their targets, about 30 frames a second.
  // While a scan runs the scale glides slowly: the PC moves one step about every 1.5 s.
  uint32_t now = millis();
  if (now - lastFrame >= 33) {
    lastFrame = now;
    float pxDiff = (f - dialPos) / step * DIAL_PX_PER_STEP;
    if (fabsf(pxDiff) < 0.5f) dialPos = f; else dialPos += (f - dialPos) * (scanning ? 0.07f : 0.35f);
    float nt = mu ? 0.0f : (float)vol;
    if (fabsf(nt - volNeedle) < 0.4f) volNeedle = nt; else volNeedle += (nt - volNeedle) * 0.25f;
    if (fabsf(sigTarget - sigNeedle) < 0.4f) sigNeedle = sigTarget; else sigNeedle += (sigTarget - sigNeedle) * 0.25f;
  }
  if (dialPos != dPos || step != dStep) {
    drawDialSprite(*dialSprite, dialPos, step);
    dialSprite->pushSprite(&lcd, DIAL_X, DIAL_Y);
    dPos = dialPos;
  }
  if (sigNeedle != dSigN) {
    drawMeterSprite(*meterSprite, false, sigNeedle, false);
    meterSprite->pushSprite(&lcd, SIG_X, METER_Y);
    dSigN = sigNeedle;
  }
  if (volNeedle != dVolN || mu != dMute) {
    drawMeterSprite(*meterSprite, true, volNeedle, mu);
    meterSprite->pushSprite(&lcd, VOL_X, METER_Y);
    dVolN = volNeedle; dMute = mu;
  }
  // LCD line 1: the exact set frequency (never the eased dial position).
  if (f != dFreq || full) {
    drawLcdLine1(f);
    dFreq = f;
  }
  if (strcmp(clockDate, dClockDate) != 0 || strcmp(clockTime, dClockTime) != 0 || full) {
    drawClockPanel(clockDate, clockTime);
    strlcpy(dClockDate, clockDate, sizeof dClockDate);
    strlcpy(dClockTime, clockTime, sizeof dClockTime);
  }
  // LCD line 2: a scan has priority, then the AUTO hint, then which link is the problem
  char st[48]; uint16_t col;
  if (scanning) {
    snprintf(st, sizeof st, "SCANNING %s  Esc / push: stop", scanUp ? ">>>" : "<<<");
    col = COL_AMBER;
  } else if (scanDoneMs && millis() - scanDoneMs < 4000 && scanMsg[0] && strcmp(scanMsg, "scanning") != 0 && autoMode) {
    if (strcmp(scanMsg, "signal") == 0) { snprintf(st, sizeof st, "SIGNAL FOUND  +%d dB", (int)lroundf(scanExcess)); col = COL_OK; }
    else if (strcmp(scanMsg, "end") == 0) { snprintf(st, sizeof st, "END OF BAND"); col = COL_WARN; }
    else if (strcmp(scanMsg, "lost") == 0) { snprintf(st, sizeof st, "SCAN ENDED: KIWI LINK LOST"); col = COL_BAD; }
    else { snprintf(st, sizeof st, "SCAN STOPPED"); col = COL_DIM; }
  } else if (autoMode && millis() < autoHintUntil) {
    snprintf(st, sizeof st, "AUTO: turn the knob to search"); col = COL_OK;
  } else {
    statusText(st, sizeof(st), &col);
  }
  char shown[25];
  strlcpy(shown, st, sizeof shown);  // at most 24 characters: stays in the main area beside the clock
  if (strcmp(shown, dLine2) != 0 || col != dCol || full) {
    drawLcdLine(1, shown, lcdInk(col));
    strlcpy(dLine2, shown, sizeof(dLine2));
    dCol = col;
  }
}

// ---- "PC NOT CONNECTED" notice. USB or Wi-Fi may provide the control link.
static bool macNoticeActive() {
  static uint32_t downSince = 0;
  bool ok;
  { Lock l; ok = rem.ok; }
  if (ok) { downSince = 0; return false; }
  if (!downSince) downSince = millis();
  return millis() - downSince >= 3000;
}

static void drawMacNotice(bool full) {
  static char dKey[160] = "";
  char detail[64], ipLine[44], local[44];
  uint16_t lw;
  {
    Lock l;
    const char* d = netStatus;
    if (strncmp(d, "PC: ", 4) == 0) d += 4;
    snprintf(detail, sizeof detail, "Last error: %.26s", d);
    if (macIP.isEmpty()) snprintf(ipLine, sizeof ipLine, "PC IP: not found yet (auto)");
    else snprintf(ipLine, sizeof ipLine, "PC IP: %.15s (%s)", macIP.c_str(), macManual ? "manual" : "auto");
    snprintf(local, sizeof local, "%.1f kHz %s  VOL %d%s", wantFreq, MODE_LABEL[wantMode], wantVolume, wantMuted ? " MUTE" : "");
    lw = WiFi.status() == WL_CONNECTED ? COL_OK : COL_BAD;
  }
  char key[160];
  snprintf(key, sizeof key, "%s|%s|%s", detail, ipLine, local);
  if (!full && strcmp(key, dKey) == 0) return;
  strlcpy(dKey, key, sizeof dKey);
  lcd.fillScreen(COL_BG);
  txt(4, 3, 1, COL_AMBER, "RADIO DAYS SDR");
  lcd.drawFastHLine(0, 14, 240, COL_DIM);
  drawLamp(164, 3, 'W', lw); drawLamp(190, 3, 'P', COL_BAD); drawLamp(216, 3, 'K', COL_DIM);
  txt(24, 22, 2, COL_BAD, "PC NOT CONNECTED");
  txt(4, 50, 1, COL_TEXT, "Start RADIO DAYS on your PC.");
  txt(4, 60, 1, COL_TEXT, "Connect USB, or use the same Wi-Fi.");
  txt(4, 76, 1, COL_DIM, "OUT PC: USB is enough.");
  txt(4, 86, 1, COL_DIM, "OUT ADV: USB or Wi-Fi audio.");
  txt(4, 102, 1, COL_WARN, detail);
  txt(4, 112, 1, COL_DIM, ipLine);
  txt(4, 124, 1, COL_AMBER, local);
}

static void drawRadio(bool full) {
  static bool lastNotice = false;
  bool notice = macNoticeActive();
  if (notice != lastNotice) { lastNotice = notice; full = true; }  // back to the normal screen once the PC answers
  if (notice) { drawMacNotice(full); return; }
  if (analogReady) drawRadioAnalog(full); else drawRadioSimple(full);
}


static void handleRadio(const KeyInput& k, const KnobInput& kn) {
  // ADV's TCA8418 reports one queued edge per update.  Check the live matrix as
  // well so a G held across an update cannot be lost by the extra edge filter.
  if (M5Cardputer.Keyboard.isKeyPressed('g') || M5Cardputer.Keyboard.isKeyPressed('G')) {
    globeOpen();
    return;
  }
  static bool muteKeyWasDown = false;
  bool muteKeyDown = M5Cardputer.Keyboard.isKeyPressed('m') || M5Cardputer.Keyboard.isKeyPressed('M');
  bool muteKeyDirect = muteKeyDown && !muteKeyWasDown;
  muteKeyWasDown = muteKeyDown;
  if (muteKeyDirect) toggleMute();
  if (kn.tuneDetents) {
    if (tuneAuto) autoTurn(kn.tuneDetents);
    else { float f; { Lock l; f = wantFreq; } changeFreq(f + kn.tuneDetents * STEPS[stepIdx]); }
  }
  if (kn.volumeDetents) { int v; { Lock l; v = wantVolume; } changeVolume(v + kn.volumeDetents * VOL_STEP); }
  if (kn.shortPress) { if (scanRunning()) scanRequest(0); else toggleMute(); }  // a push stops a scan; otherwise mute as always
  if (k.tab) setTuneAuto(!tuneAuto);
  for (size_t i = 0; i < k.chars.length(); i++) {
    char c = k.chars[i];
    int v; float f; { Lock l; v = wantVolume; f = wantFreq; }
    switch (c) {
      case ';': case '+': case '=': changeVolume(v + VOL_STEP); break;   // up arrow / + key
      case '.': case '-': case '_': changeVolume(v - VOL_STEP); break;   // down arrow / - key
      case ',': if (tuneAuto) autoTurn(-1); else changeFreq(f - STEPS[stepIdx]); break;   // left arrow
      case '/': if (tuneAuto) autoTurn(+1); else changeFreq(f + STEPS[stepIdx]); break;   // right arrow
      case 't': case 'T': setTuneAuto(!tuneAuto); break;                  // MANUAL <-> AUTO (Tab does the same)
      case '`': if (scanRunning()) scanRequest(0); break;                 // Esc: stop a scan
      case 'm': case 'M': if (!muteKeyDirect) toggleMute(); break;
      case '1': stepIdx = 0; break;
      case '2': stepIdx = 1; break;
      case '3': stepIdx = 2; break;
      case '4': stepIdx = 3; break;
      case 'a': case 'A': changeMode(0); break;
      case 'u': case 'U': changeMode(1); break;
      case 'l': case 'L': changeMode(2); break;
      case 'n': case 'N': changeServer(+1); break;
      case 'p': case 'P': changeServer(-1); break;
      case 'f': case 'F': entry = ""; entryMsg = ""; setScreen(SC_FREQ); return;
      case 'w': case 'W': if (wifiPaused) setScreen(SC_WIFI_LIST); else startScan(); return;
      case 'i': case 'I': { Lock l; entry = macIP; } entryMsg = ""; setScreen(SC_MACIP); return;
      case 'g': case 'G': globeOpen(); return;
      case 'o': case 'O': changeOutput(wantOutput ? 0 : 1); break;   // Mac speaker <-> Cardputer speaker
      case 'd': case 'D': setScreen(SC_DIAG); return;                 // diagnostics
      case 'h': case 'H': setScreen(SC_HELP); return;
    }
  }
}

// ------------------------------------------------------------------ globe: choose the KiwiSDR (from U10's Location globe)
// Orthographic view, GLOBE_TILT_DEG from the north.  For every pixel the map row and the
// longitude relative to the centre meridian are computed once when the screen opens;
// turning the globe is then a table look-up.  Memory (~29 KB in U10, ~22 KB here) is taken
// when the screen opens and freed when it closes.  U10's satellite/UFO is not ported.
static constexpr int GLOBE_R = 42;
static constexpr int GLOBE_D = 2 * GLOBE_R + 1;  // 85 x 85
static constexpr int GLOBE_X = 4;
static constexpr int GLOBE_Y = 20;
static constexpr float GLOBE_TILT_DEG = 25.f;
static constexpr uint32_t GLOBE_FRAME_MS = 33;
static constexpr int PANEL_X = 100;  // 140 px = 23 characters of the small font

static M5Canvas* globeSprite = nullptr;
static uint8_t* globeRow = nullptr;    // per pixel: map row 0..127, 255 = outside the globe
static uint8_t* globeCol = nullptr;    // per pixel: column relative to the centre meridian
static uint8_t* globeDepth = nullptr;  // per pixel: 0..2, shading towards the rim
static bool globeReady = false, globeDirty = true;
static float globeCenterLon = 0.f, globeTargetLon = 0.f;
static uint32_t globeLastMs = 0;
static int gSel = 0;               // receiver under the cursor
static int gPending = -1;          // receiver requested with ENTER, until the Mac's state shows the result
static uint32_t gOnAirMs = 0;      // when the requested receiver came on air
static uint32_t gPanelKey = 0;     // what the panel showed last
static uint32_t gOpenMs = 0;
static bool satShown = false;   // the satellite (and its waves) is on the screen

static void globeEnd() {
  satShown = false;
  if (globeSprite) { globeSprite->deleteSprite(); delete globeSprite; globeSprite = nullptr; }
  free(globeRow); free(globeCol); free(globeDepth);
  globeRow = globeCol = globeDepth = nullptr;
  globeReady = false;
}

static bool globeBegin() {
  globeEnd();
  globeRow = (uint8_t*)malloc(GLOBE_D * GLOBE_D);
  globeCol = (uint8_t*)malloc(GLOBE_D * GLOBE_D);
  globeDepth = (uint8_t*)malloc(GLOBE_D * GLOBE_D);
  globeSprite = new (std::nothrow) M5Canvas(&lcd);
  if (!globeRow || !globeCol || !globeDepth || !globeSprite) { globeEnd(); return false; }
  globeSprite->setColorDepth(8);
  if (!globeSprite->createSprite(GLOBE_D, GLOBE_D)) { globeEnd(); return false; }
  const float tr = GLOBE_TILT_DEG * (float)M_PI / 180.f;
  const float ct = cosf(tr), st = sinf(tr);
  for (int py = 0; py < GLOBE_D; py++) {
    for (int px = 0; px < GLOBE_D; px++) {
      int i = py * GLOBE_D + px;
      float x = (float)(px - GLOBE_R) / GLOBE_R, y = (float)(GLOBE_R - py) / GLOBE_R;
      float d2 = x * x + y * y;
      if (d2 > 1.f) { globeRow[i] = 255; continue; }
      float z = sqrtf(1.f - d2);
      float yg = y * ct + z * st;
      float zg = -y * st + z * ct;
      float lat = asinf(fmaxf(-1.f, fminf(1.f, yg))) * 180.f / (float)M_PI;
      float lonRel = atan2f(x, zg) * 180.f / (float)M_PI;
      int row = (int)((90.f - lat) * GLOBE_MAP_H / 180.f);
      globeRow[i] = (uint8_t)constrain(row, 0, GLOBE_MAP_H - 1);
      globeCol[i] = (uint8_t)((int)lroundf(lonRel * GLOBE_MAP_W / 360.f) & (GLOBE_MAP_W - 1));
      globeDepth[i] = z > 0.55f ? 0 : (z > 0.25f ? 1 : 2);
    }
  }
  globeReady = true;
  globeDirty = true;
  Serial.printf("[GLOBE] ready, heap %u, largest %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return true;
}

// Screen position of a place, or false when it is on the far side.
static bool globeProject(float lat, float lon, int& sx, int& sy) {
  const float r = (float)M_PI / 180.f;
  const float tr = GLOBE_TILT_DEG * r;
  float dl = (lon - globeCenterLon) * r, la = lat * r;
  float xg = cosf(la) * sinf(dl), yg = sinf(la), zg = cosf(la) * cosf(dl);
  float y = yg * cosf(tr) - zg * sinf(tr);
  float z = yg * sinf(tr) + zg * cosf(tr);
  if (z <= 0.05f) return false;
  sx = GLOBE_R + (int)lroundf(xg * GLOBE_R);
  sy = GLOBE_R - (int)lroundf(y * GLOBE_R);
  return true;
}

// ---- the satellite of the first RADIO DAYS (U4's picture and behaviour), by the globe's top right.
// It floats gently (the drift, speed swell and pauses of U4), its body lamps chase, and every now and then it talks with
// the Earth, one direction at a time: cyan wave fronts Earth -> satellite, a short blink of its lamps, then yellow wave
// fronts satellite -> Earth (three thin arcs each).  Decoration only: not tied to the real link.
// Drawn straight on the screen, only where it is clear of the globe disc, the header and the panel (x >= PANEL_X), so
// erasing it (the same pixels in black) never touches anything else.  It is redrawn only when its pose changes.
static constexpr int SAT_W = 24, SAT_H = 12;
static constexpr int SAT_HOME_X = 86, SAT_HOME_Y = 24;  // centre on the screen
static const char* const SAT_ART[SAT_H] = {
    "...........y............",
    "...........k............",
    "kkkkkkk..kkkkkk..kkkkkkk",
    "kbbkbbk..kssssk..kbbkbbk",
    "kbbkbbkggkwssskggkbbkbbk",
    "kbbkbbk..kyrysk..kbbkbbk",
    "kkkkkkk..kssssk..kkkkkkk",
    ".........kkkkkk.........",
    "...........k............",
    ".........kdddk..........",
    "..........kkk...........",
    "........................",
};
static constexpr int SAT_LAMP_ROW = 5;
static constexpr uint32_t SAT_LAMP_MS = 400;     // the body lamps chase slowly
static constexpr uint32_t TALK_FIRST_MS = 3000;  // the first talk after the screen opens
static constexpr uint32_t TALK_EVERY_MS = 10000; // then every 10 s + 0..5 s
static constexpr uint32_t TALK_EXTRA_MS = 5000;
static constexpr uint32_t TALK_DOWN_MS = 1300;   // Earth -> satellite, cyan
static constexpr uint32_t TALK_BLINK_MS = 600;   // the lamps blink
static constexpr uint32_t TALK_UP_MS = 1300;     // satellite -> Earth, yellow
static constexpr uint32_t TALK_FRAME_MS = 40;    // wave fronts move in steps of this

static float satPhase = 0.f;       // drift time (ms), advanced at a varying speed
static uint32_t satLastMs = 0;
static uint32_t satTalkAt = 0;     // next talk starts
static uint32_t satTalkStart = 0;
static bool satTalking = false;
static uint32_t satRng = 0x1234567u;
static SatPose satDrawn;

static void satReset(uint32_t now) {
  satLastMs = now;
  satTalking = false;
  satTalkAt = now + TALK_FIRST_MS;
  satDrawn = SatPose();
  satShown = false;
}

static SatPose satPoseAt(uint32_t now) {
  const float TAU = 2.f * (float)M_PI;
  uint32_t dt = now - satLastMs;
  satLastMs = now;
  if (dt > 100) dt = 100;
  // speed: a slow swell (19 s), and a gentle stop for a moment every 23 s
  float rate = 0.35f + 0.65f * (0.5f + 0.5f * sinf(TAU * (float)(now % 19000) / 19000.f));
  uint32_t pm = now % 23000;
  if (pm < 3000) rate *= 0.5f + 0.5f * cosf(TAU * (float)pm / 3000.f);
  satPhase += (float)dt * rate;
  if (satPhase > 1.0e7f) satPhase -= 1.0e7f;
  SatPose p;
  p.shown = true;
  float dx = 4.2f * sinf(TAU * satPhase / 6100.f) + 1.2f * sinf(TAU * satPhase / 2300.f + 0.7f);
  float dy = 2.0f * sinf(TAU * satPhase / 3700.f + 1.3f) + 0.7f * sinf(TAU * satPhase / 1900.f);
  p.x = (int)lroundf(SAT_HOME_X + dx) - SAT_W / 2;
  p.y = (int)lroundf(SAT_HOME_Y + dy) - SAT_H / 2;
  p.lamp = (uint8_t)((now / SAT_LAMP_MS) % 3);
  if (!satTalking && (int32_t)(now - satTalkAt) >= 0) { satTalking = true; satTalkStart = now; }
  if (satTalking) {
    uint32_t t = now - satTalkStart;
    if (t < TALK_DOWN_MS) {
      p.wave = 1;
      p.waveStep = (uint8_t)(t / TALK_FRAME_MS);
    } else if (t < TALK_DOWN_MS + TALK_BLINK_MS) {
      p.lampMode = ((t - TALK_DOWN_MS) / 100) % 2 ? 2 : 1;  // all on / all dim
    } else if (t < TALK_DOWN_MS + TALK_BLINK_MS + TALK_UP_MS) {
      p.wave = 2;
      p.waveStep = (uint8_t)((t - TALK_DOWN_MS - TALK_BLINK_MS) / TALK_FRAME_MS);
    } else {
      satTalking = false;
      satRng = satRng * 1664525u + 1013904223u;
      satTalkAt = now + TALK_EVERY_MS + (satRng >> 8) % TALK_EXTRA_MS;
    }
  }
  return p;
}

static bool satSame(const SatPose& a, const SatPose& b) {
  return a.shown == b.shown && a.x == b.x && a.y == b.y && a.lamp == b.lamp && a.lampMode == b.lampMode && a.wave == b.wave && a.waveStep == b.waveStep;
}

// one pixel, only where nothing else lives: not on the globe disc (with its thin atmosphere), not on the header, not on the panel
static void satPlot(int x, int y, uint16_t col) {
  const int ecx = GLOBE_X + GLOBE_R, ecy = GLOBE_Y + GLOBE_R;
  if (x < 0 || x >= PANEL_X || y < 16 || y >= 120) return;
  int dx = x - ecx, dy = y - ecy;
  if (dx * dx + dy * dy <= (GLOBE_R + 1) * (GLOBE_R + 1)) return;
  lcd.drawPixel(x, y, col);
}

// wave fronts: up to three arcs of circles round the sender, moving towards the receiver.
// wave 1: from the Earth's edge to the satellite (cyan); wave 2: from the satellite to the Earth (yellow).
static void satWaves(const SatPose& p, bool erase) {
  if (!p.wave) return;
  const int ecx = GLOBE_X + GLOBE_R, ecy = GLOBE_Y + GLOBE_R;
  float ux = p.x + SAT_W / 2.f, uy = p.y + SAT_H / 2.f;
  float cx = ux - ecx, cy = uy - ecy;
  float d = sqrtf(cx * cx + cy * cy);
  // the Earth's waves spread from a point a little inside the disc (flat ripples; the part over the disc is not drawn)
  float ex = ecx + cx / d * (GLOBE_R - 10), ey = ecy + cy / d * (GLOBE_R - 10);
  float gap = sqrtf((ux - ex) * (ux - ex) + (uy - ey) * (uy - ey));
  float sx = p.wave == 1 ? ex : ux, sy = p.wave == 1 ? ey : uy;  // the sender
  float base = p.wave == 1 ? atan2f(uy - ey, ux - ex) : atan2f(ey - uy, ex - ux);
  float r0 = p.wave == 1 ? 12.f : 6.f;                       // the Earth's edge / the satellite's own half size
  float r1 = p.wave == 1 ? gap - 3.f : gap + 2.f;            // up to the receiver (the part over the Earth is not drawn)
  uint32_t dur = p.wave == 1 ? TALK_DOWN_MS : TALK_UP_MS;
  float progress = (float)p.waveStep * TALK_FRAME_MS / dur;
  for (int k = 0; k < 3; k++) {
    float r = r0 + progress * (r1 - r0 + 10.f) - 5.f * k;
    if (r < r0 || r > r1) continue;
    uint16_t col = erase ? COL_BG
                         : (p.wave == 1 ? (k == 0 ? rgb565c(90, 230, 255) : rgb565c(40, 150, 190))
                                        : (k == 0 ? rgb565c(255, 225, 60) : rgb565c(170, 140, 30)));
    float half = p.wave == 1 ? 0.5f : 0.62f;  // +-29 / +-36 degrees (wider than U4: the Earth is closer here)
    int steps = (int)(r * 2.f * half) + 2;
    for (int i = 0; i <= steps; i++) {
      float a = base - half + 2.f * half * i / steps;
      satPlot((int)lroundf(sx + r * cosf(a)), (int)lroundf(sy + r * sinf(a)), col);
    }
  }
}

// the picture; erase = the same pixels in black
static void satPaint(const SatPose& p, bool erase) {
  if (!p.shown) return;
  satWaves(p, erase);
  int lampIndex = 0;
  for (int ry = 0; ry < SAT_H; ry++) {
    for (int rx = 0; rx < SAT_W; rx++) {
      char c = SAT_ART[ry][rx];
      if (c == '.') continue;
      uint16_t col;
      switch (c) {
        case 'k': col = rgb565c(60, 60, 80); break;      // outline, panel grid
        case 'b': col = rgb565c(50, 90, 210); break;     // solar cells
        case 'd': col = rgb565c(200, 200, 210); break;   // dish
        case 'w': col = rgb565c(255, 255, 255); break;
        case 's': col = rgb565c(220, 180, 70); break;    // gold foil body
        case 'g': col = rgb565c(150, 150, 165); break;   // struts
        default: {  // 'y' / 'r' lamps; on the body they chase, or all blink while it talks
          bool lit;
          if (ry != SAT_LAMP_ROW) lit = true;
          else if (p.lampMode) lit = p.lampMode == 1;
          else lit = (lampIndex % 3) == p.lamp;
          if (ry == SAT_LAMP_ROW) lampIndex++;
          if (c == 'y') col = lit ? rgb565c(255, 230, 0) : rgb565c(110, 90, 0);
          else col = lit ? rgb565c(255, 60, 60) : rgb565c(110, 30, 30);
        }
      }
      satPlot(p.x + rx, p.y + ry, erase ? COL_BG : col);
    }
  }
}

// every pass of the globe screen: move it when its pose has changed
static void satService() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  if (satShown && now - lastTick < TALK_FRAME_MS) return;
  lastTick = now;
  SatPose p = satPoseAt(now);
  if (satShown && satSame(p, satDrawn)) return;
  lcd.startWrite();
  if (satShown) satPaint(satDrawn, true);
  satPaint(p, false);
  lcd.endWrite();
  satDrawn = p;
  satShown = true;
}

static void globeRender() {
  static const uint8_t SEA[3] = {(uint8_t)lgfx::color332(30, 110, 230), (uint8_t)lgfx::color332(20, 70, 180), (uint8_t)lgfx::color332(10, 40, 110)};
  static const uint8_t LAND[3] = {(uint8_t)lgfx::color332(60, 200, 60), (uint8_t)lgfx::color332(40, 150, 40), (uint8_t)lgfx::color332(20, 90, 20)};
  uint8_t* buf = (uint8_t*)globeSprite->getBuffer();
  int centerCol = (int)lroundf((globeCenterLon + 180.f) * GLOBE_MAP_W / 360.f);
  memset(buf, 0, GLOBE_D * GLOBE_D);
  for (int i = 0; i < GLOBE_D * GLOBE_D; i++) {
    uint8_t row = globeRow[i];
    if (row == 255) continue;
    int col = (centerCol + globeCol[i]) & (GLOBE_MAP_W - 1);
    bool land = (GLOBE_LAND[row][col >> 3] >> (7 - (col & 7))) & 1;
    buf[i] = land ? LAND[globeDepth[i]] : SEA[globeDepth[i]];
  }
  globeSprite->drawCircle(GLOBE_R, GLOBE_R, GLOBE_R, lgfx::color332(90, 140, 220));  // thin atmosphere
  int cur, n, sel = gSel;
  uint16_t ringCol;
  {
    Lock l;
    cur = rem.ok ? rem.serverIndex : -1;
    n = srvCount;
    bool up = strcmp(rem.link, "connected") == 0, bad = strcmp(rem.link, "error") == 0;
    ringCol = up ? lgfx::color332(0, 255, 0) : (bad ? lgfx::color332(255, 0, 0) : lgfx::color332(255, 160, 0));
    // receivers: yellow dots (several receivers on one spot share the dot)
    for (int i = 0; i < n; i++) {
      int sx, sy;
      if (!srvs[i].hasPos || !globeProject(srvs[i].lat, srvs[i].lon, sx, sy)) continue;
      globeSprite->fillRect(sx - 1, sy - 1, 2, 2, lgfx::color332(255, 220, 0));
    }
    // the receiver in use: a ring coloured by the Mac's real link state
    int sx, sy;
    if (cur >= 0 && cur < n && srvs[cur].hasPos && globeProject(srvs[cur].lat, srvs[cur].lon, sx, sy))
      globeSprite->drawCircle(sx, sy, 4, ringCol);
    // the selection: U10's white/red marker
    if (sel >= 0 && sel < n && srvs[sel].hasPos && globeProject(srvs[sel].lat, srvs[sel].lon, sx, sy)) {
      globeSprite->fillCircle(sx, sy, 3, lgfx::color332(255, 255, 255));
      globeSprite->fillCircle(sx, sy, 2, lgfx::color332(255, 0, 0));
    }
  }
  globeSprite->pushSprite(&lcd, GLOBE_X, GLOBE_Y);
  if (satShown) satPaint(satDrawn, false);  // the sprite push covers part of it
}

static void globeAim(bool immediately) {
  Lock l;
  if (gSel >= 0 && gSel < srvCount && srvs[gSel].hasPos) {
    globeTargetLon = srvs[gSel].lon;
    if (immediately) globeCenterLon = srvs[gSel].lon;
  }
  globeDirty = true;
}

// Turns the globe towards the target (shortest way, easing) - U10's globeService.
static void globeService() {
  if (!globeReady) return;
  uint32_t now = millis();
  if (now - globeLastMs < GLOBE_FRAME_MS) return;
  globeLastMs = now;
  bool moved = false;
  float diff = fmodf(globeTargetLon - globeCenterLon + 540.f, 360.f) - 180.f;
  if (fabsf(diff) > 0.3f) {
    float step = diff * 0.2f;
    if (fabsf(step) < 0.6f) step = diff > 0 ? fminf(0.6f, diff) : fmaxf(-0.6f, diff);
    globeCenterLon += step;
    moved = true;
  } else if (globeCenterLon != globeTargetLon) {
    globeCenterLon = globeTargetLon;
    moved = true;
  }
  if (globeCenterLon > 180.f) globeCenterLon -= 360.f;
  if (globeCenterLon < -180.f) globeCenterLon += 360.f;
  if (moved || globeDirty) { globeDirty = false; globeRender(); }
}

static void globeOpen() {
  { Lock l; srvWant = true; gSel = (rem.ok && rem.serverIndex < max(1, srvCount)) ? rem.serverIndex : 0; }
  gPending = -1;
  gOpenMs = millis();
  satReset(millis());
  header("KiwiSDR GLOBE");
  txt(54, 59, 2, COL_AMBER, "OPENING...");
  if (!globeBegin()) Serial.println("[GLOBE] no memory: text list instead");
  globeAim(true);
  gPanelKey = 0;
  setScreen(SC_GLOBE);
}
static void globeClose() {
  globeEnd();
  gPending = -1;
  setScreen(SC_RADIO);
}

// What the panel says about receiver i, from the Mac's real state.
static void globeStatus(int i, char* out, size_t n, uint16_t* col) {
  Lock l;
  bool isCur = rem.ok && rem.serverIndex == i;
  if (gPending == i && !isCur) {
    if (rem.ok) { snprintf(out, n, "REQUESTING..."); *col = COL_WARN; }
    else { snprintf(out, n, "%.23s", netStatus); *col = COL_BAD; }
  } else if (isCur) {
    if (strcmp(rem.link, "connected") == 0) { snprintf(out, n, "ON AIR"); *col = COL_OK; }
    else if (strcmp(rem.link, "error") == 0) { snprintf(out, n, "FAILED: %.15s", rem.error[0] ? rem.error : "no answer"); *col = COL_BAD; }
    else { snprintf(out, n, "CONNECTING..."); *col = COL_WARN; }
  } else { snprintf(out, n, "ENTER: connect"); *col = COL_DIM; }
}

static void drawGlobePanel(bool full) {
  char st[48], now[48], l1[40], l2[40], l3[40];
  uint16_t col = COL_DIM, ncol = COL_DIM;
  int sel = gSel, n, cur, here = 0, hereIdx = 0, vrev;
  bool hasPos = false;
  char sname[22], lname[44];
  float lat = 0, lon = 0;
  {
    Lock l;
    n = srvCount; cur = rem.ok ? rem.serverIndex : -1; vrev = (int)statusRev;
    if (n == 0 || sel >= n) { sname[0] = 0; lname[0] = 0; }
    else {
      strlcpy(sname, srvs[sel].shortName, sizeof(sname));
      strlcpy(lname, srvs[sel].name, sizeof(lname));
      hasPos = srvs[sel].hasPos; lat = srvs[sel].lat; lon = srvs[sel].lon;
      for (int i = 0; i < n; i++)
        if (srvs[i].hasPos == hasPos && hasPos && fabsf(srvs[i].lat - lat) < 0.05f && fabsf(srvs[i].lon - lon) < 0.05f) {
          here++;
          if (i <= sel) hereIdx = here;
        }
  }
  }
  if (n > 0) globeStatus(sel, st, sizeof(st), &col);
  // the receiver in use, always visible
  {
    Lock l;
    if (!rem.ok) { snprintf(now, sizeof(now), "NOW: %.18s", netStatus); ncol = COL_BAD; }
    else {
      const char* nm = (rem.serverIndex < n) ? srvs[rem.serverIndex].shortName : rem.server;
      const char* lk = strcmp(rem.link, "connected") == 0 ? "on air" : (strcmp(rem.link, "error") == 0 ? "retrying" : "connecting");
      snprintf(now, sizeof(now), "NOW: %.8s %s", nm, lk);
      ncol = strcmp(rem.link, "connected") == 0 ? COL_OK : (strcmp(rem.link, "error") == 0 ? COL_BAD : COL_WARN);
    }
  }
  uint32_t key = (uint32_t)(sel * 31 + n * 7 + cur * 131 + here * 17 + hereIdx + gPending * 1009) ^ (uint32_t)vrev * 2654435761u ^ srvRev * 40503u;
  if (!full && key == gPanelKey) return;
  gPanelKey = key;
  lcd.fillRect(PANEL_X, 18, 144, 98, COL_BG);
  if (n == 0) {
    bool macOk;
    char det[40];
    { Lock l; macOk = rem.ok; const char* d = netStatus; if (strncmp(d, "PC: ", 4) == 0) d += 4; snprintf(det, sizeof det, "%.22s", d); }
    // Receiver locations live on the PC. USB control is fully sufficient;
    // Cardputer Wi-Fi is relevant only when OUT ADV carries audio.
    if (!macOk) {  // neither USB nor Wi-Fi control has answered
      txt(PANEL_X, 22, 1, COL_BAD, "PC NOT CONNECTED");
      txt(PANEL_X, 36, 1, COL_TEXT, "Check that");
      txt(PANEL_X, 46, 1, COL_TEXT, "Start RADIO DAYS on");
      txt(PANEL_X, 56, 1, COL_TEXT, "your PC and keep it open.");
      txt(PANEL_X, 70, 1, COL_DIM, "Connect USB, or check");
      txt(PANEL_X, 80, 1, COL_DIM, "the PC network / I key.");
      txt(PANEL_X, 94, 1, COL_WARN, det);
    } else {
      bool late = millis() - gOpenMs > 4000;
      txt(PANEL_X, 30, 1, late ? COL_WARN : COL_DIM, late ? "List unavailable" : "Loading list...");
    }
    return;
  }
  bool big = strlen(sname) <= 8;
  txt(PANEL_X, 22, big ? 2 : 1, COL_AMBER, sname);
  int y = big ? 40 : 34;
  if (cur == sel) txt(PANEL_X + (big ? 12 : 6) * (int)strlen(sname) + 4, big ? 26 : 22, 1, COL_DIM, "(now)");
  snprintf(l1, sizeof(l1), "%.23s", lname);
  txt(PANEL_X, y, 1, COL_TEXT, l1);
  if (hasPos) snprintf(l2, sizeof(l2), "%.1f%c %.1f%c", fabsf(lat), lat >= 0 ? 'N' : 'S', fabsf(lon), lon >= 0 ? 'E' : 'W');
  else snprintf(l2, sizeof(l2), "position unverified");
  txt(PANEL_X, y + 11, 1, hasPos ? COL_DIM : COL_WARN, l2);
  if (here > 1) { snprintf(l3, sizeof(l3), "%d of %d at this site", hereIdx, here); txt(PANEL_X, y + 22, 1, COL_DIM, l3); }
  snprintf(l3, sizeof(l3), "%d / %d", sel + 1, n);
  txt(PANEL_X + 100, y + 22, 1, COL_DIM, l3);
  txt(PANEL_X, y + 36, 1, col, st);
  txt(PANEL_X, y + 50, 1, ncol, now);
}

// Text list when the globe cannot be made (low memory): the page of 5 with the cursor.
static void drawGlobeFallbackList() {
  lcd.fillRect(0, 18, PANEL_X - 2, 98, COL_BG);
  Lock l;
  int first = gSel / 5 * 5;
  for (int i = first; i < first + 5 && i < srvCount; i++) {
    char line[24];
    snprintf(line, sizeof(line), "%c%.10s%s", i == gSel ? '>' : ' ', srvs[i].shortName, (rem.ok && rem.serverIndex == i) ? "*" : "");
    txt(2, 22 + (i - first) * 16, 1, i == gSel ? COL_AMBER : COL_TEXT, line);
  }
}

static void drawGlobe(bool full) {
  static uint32_t aimRev = 0;
  if (full) aimRev = ~srvRev;  // aim again after (re)opening
  if (aimRev != srvRev) {      // a new list arrived: keep the cursor valid and turn the globe to it
    aimRev = srvRev;
    if (srvCount > 0 && gSel >= srvCount) gSel = srvCount - 1;
    if (globeReady) globeAim(true);
  }
  if (full) {
    header("KiwiSDR  GLOBE");
    txt(4, 124, 1, COL_DIM, "turn:choose  push/ENTER:go  esc:back");
    globeDirty = true;
    gPanelKey = 0;
    satShown = false;  // the screen was cleared
  }
  if (globeReady) { globeService(); satService(); } else if (full) drawGlobeFallbackList();
  drawGlobePanel(full);
}

static void handleGlobe(const KeyInput& k, const KnobInput& kn) {
  if (kn.volumeDetents) { int v; { Lock l; v = wantVolume; } changeVolume(v + kn.volumeDetents * VOL_STEP); }
  if (hasChar(k, '`')) { globeClose(); return; }  // esc: nothing is changed
  int n; { Lock l; n = srvCount; }
  int move = kn.tuneDetents + (hasChar(k, '/') ? 1 : 0) - (hasChar(k, ',') ? 1 : 0) + (hasChar(k, '.') ? 1 : 0) - (hasChar(k, ';') ? 1 : 0);
  if (move && n > 0) {
    gSel = ((gSel + move) % n + n) % n;
    if (globeReady) globeAim(false); else drawGlobeFallbackList();
  }
  if ((k.enter || kn.shortPress) && n > 0) {
    int cur; bool up; { Lock l; cur = rem.ok ? rem.serverIndex : -1; up = strcmp(rem.link, "connected") == 0; }
    if (gSel == cur && up) { globeClose(); return; }  // already listening to it
    { Lock l; wantServer = gSel; verServer++; }
    gPending = gSel;
    gOnAirMs = 0;
  }
  // after a request: leave the globe once it is really on air; stay if it failed
  if (gPending >= 0) {
    Lock l;
    bool onAir = rem.ok && rem.serverIndex == gPending && strcmp(rem.link, "connected") == 0;
    if (onAir) { if (!gOnAirMs) gOnAirMs = millis(); }
    else gOnAirMs = 0;
    if (onAir && millis() - gOnAirMs > 1000) { gPending = -2; }
  }
  if (gPending == -2) globeClose();
}

// ------------------------------------------------------------------ simple text-entry screens

static void drawEntry(const char* title, const char* prompt, const char* hint) {
  header(title);
  txt(4, 22, 1, COL_TEXT, prompt);
  txt(4, 40, 2, COL_AMBER, (entry + "_").c_str());
  if (entryMsg.length()) txt(4, 76, 1, COL_WARN, entryMsg.c_str());
  txt(4, 112, 1, COL_DIM, hint);
  txt(4, 123, 1, COL_DIM, "ENTER ok  DEL erase  ` cancel");
}

static void handleEntry(const KeyInput& k, bool digitsOnly, size_t maxLen, bool (*accept)(const String&)) {
  bool changed = false;
  if (k.del && entry.length()) { entry.remove(entry.length() - 1); changed = true; }
  for (size_t i = 0; i < k.chars.length(); i++) {
    char c = k.chars[i];
    if (c == '`') { setScreen(SC_RADIO); return; }
    bool ok = (c >= '0' && c <= '9') || c == '.';
    if (ok && entry.length() < maxLen) { entry += c; changed = true; }
  }
  (void)digitsOnly;
  if (k.enter) {
    if (accept(entry)) setScreen(SC_RADIO);
    else screenDirty = true;
    return;
  }
  if (changed) { entryMsg = ""; screenDirty = true; }
}

static bool acceptFreq(const String& s) {
  float f = s.toFloat();
  if (s.length() == 0 || f < FREQ_MIN || f > FREQ_MAX) { entryMsg = "Range 10 - 30000 kHz"; return false; }
  changeFreq(f);
  return true;
}
static bool acceptMacIP(const String& s) {
  IPAddress ip;
  Lock l;
  if (s.length() == 0) { macManual = false; macIP = ""; prefsSaveMac(); return true; }  // blank = auto discover
  if (!ip.fromString(s)) { entryMsg = "Not a valid IPv4 address"; return false; }
  macIP = s; macManual = true; prefsSaveMac();
  return true;
}

// ------------------------------------------------------------------ Wi-Fi screens (ported keys from U10)
static constexpr int NET_MAX = 24;
static NetEntry nets[NET_MAX];
static int netCount = 0, netSel = 0;
static bool scanning = false;
static String joinSsid, joinPass, resultText;
static String wifiListMsg;
static String scanConnectedSsid;
static int scanConnectedRssi = 0;
static bool showPass = false, resultOk = false, askDelete = false;
static uint32_t joinStartMs = 0;
static int savedSel = 0;
static String savedMsg;

static void startScan() {
  scanConnectedSsid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  scanConnectedRssi = scanConnectedSsid.length() ? WiFi.RSSI() : 0;
  WiFi.mode(WIFI_STA);
  netCount = 0; netSel = 0;
  WiFi.scanDelete();
  scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
  setScreen(SC_WIFI_LIST);
}
static void setWifiPaused(bool paused) {
  wifiPaused = paused;
  scanning = false;
  WiFi.scanDelete();
  netCount = 0;
  if (paused) {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    wifiState = W_IDLE;
  } else {
    WiFi.mode(WIFI_STA);
    wifiState = W_IDLE;
    startScan();
  }
  statusRev++;
  screenDirty = true;
}
static void collectScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  netCount = 0;
  for (int i = 0; i < n && netCount < NET_MAX; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j = 0; j < netCount; j++) if (nets[j].ssid == s) { dup = true; if (WiFi.RSSI(i) > nets[j].rssi) nets[j].rssi = WiFi.RSSI(i); }
    if (dup) continue;
    nets[netCount++] = {s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN};
  }
  // Some ESP32 scans omit the AP that is already carrying the connection.
  // Keep the setup screen truthful: a live connection must never be reported
  // as "No networks found" merely because that AP was absent from this scan.
  if (scanConnectedSsid.length() && netCount < NET_MAX) {
    String current = scanConnectedSsid;
    bool listed = false;
    for (int i = 0; i < netCount; i++) if (nets[i].ssid == current) { listed = true; break; }
    if (!listed) nets[netCount++] = {current, scanConnectedRssi, false};
  }
  // strongest first
  for (int i = 0; i < netCount; i++)
    for (int j = i + 1; j < netCount; j++)
      if (nets[j].rssi > nets[i].rssi) { NetEntry t = nets[i]; nets[i] = nets[j]; nets[j] = t; }
  WiFi.scanDelete();
  screenDirty = true;
}
static void leaveWifiSetup() {
  wifiState = WiFi.status() == WL_CONNECTED ? W_UP : W_IDLE;  // otherwise restart the background search
  setScreen(SC_RADIO);
}
static void startJoin() {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(joinSsid.c_str(), joinPass.length() ? joinPass.c_str() : nullptr);
  joinStartMs = millis();
  wifiState = W_UP;  // the background search stays out of the way while joining
  setScreen(SC_WIFI_JOIN);
}

// Remember a network that really connected.  Returns the name of a replaced entry, or "".
static String saveJoined() {
  String replaced = "";
  int idx = savedIndexOf(joinSsid.c_str());
  if (idx < 0) {
    if (savedCount < WIFI_SAVED_MAX) idx = savedCount++;
    else {                                  // full: replace the one that was not used last (the oldest slot)
      idx = (savedLast + 1) % WIFI_SAVED_MAX;
      replaced = saved[idx].ssid;
    }
  }
  strlcpy(saved[idx].ssid, joinSsid.c_str(), sizeof(saved[idx].ssid));
  strlcpy(saved[idx].pass, joinPass.c_str(), sizeof(saved[idx].pass));
  savedLast = idx;
  return replaced;
}

static void drawWifiList() {
  header("WI-FI SETUP");
  if (wifiPaused) {
    txt(4, 30, 1, COL_BAD, "Wi-Fi OFF (PC CABLE only)");
    txt(4, 112, 1, COL_DIM, "X: resume Wi-Fi   `: back");
    return;
  }
  if (scanning) { txt(4, 30, 1, COL_WARN, "Scanning..."); return; }
  // A safety warning used to be painted over the seventh network row, making
  // both lines look like a yellow display fault. Give it a dedicated panel.
  if (wifiListMsg.length()) {
    lcd.fillRect(0, 16, 240, 82, COL_BG);
    txt(4, 30, 1, COL_WARN, wifiListMsg.c_str());
    txt(4, 48, 1, COL_TEXT, "The cable may supply power");
    txt(4, 59, 1, COL_TEXT, "without an active PC data link.");
    txt(4, 78, 1, COL_DIM, "Press any key to return.");
    txt(4, 111, 1, COL_DIM, "Wi-Fi remains connected.");
    return;
  }
  if (netCount == 0) {
    txt(4, 30, 1, COL_WARN, "No networks found.");
    txt(4, 112, 1, COL_DIM, "R rescan S saved X off ` back");
    return;
  }
  int first = max(0, min(netSel - 3, netCount - 7));
  for (int row = 0; row < 7 && first + row < netCount; row++) {
    int i = first + row;
    char line[56], name[27];
    strlcpy(name, nets[i].ssid.c_str(), sizeof(name));
    for (char* p = name; *p; p++) if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7F) *p = '?';
    snprintf(line, sizeof(line), "%c%c%-26.26s %4d%s", i == netSel ? '>' : ' ', savedIndexOf(nets[i].ssid.c_str()) >= 0 ? '*' : ' ', name, nets[i].rssi, nets[i].open ? " open" : "");
    txt(4, 19 + row * 11, 1, i == netSel ? COL_AMBER : COL_TEXT, line);
  }
  txt(4, 100, 1, COL_DIM, "knob / ; . move   ENTER select");
  txt(4, 111, 1, COL_DIM, "R rescan S saved X off ` back");
}
static void drawWifiPass() {
  header("WI-FI SETUP");
  char line[64];
  snprintf(line, sizeof(line), "Network: %.30s", joinSsid.c_str());
  txt(4, 20, 1, COL_TEXT, line);
  txt(4, 36, 1, COL_DIM, "Password:");
  String shown = showPass ? joinPass : String();
  if (!showPass) for (size_t i = 0; i < joinPass.length(); i++) shown += '*';
  if (shown.length() > 36) shown = shown.substring(shown.length() - 36);
  txt(4, 48, 1, COL_AMBER, (shown + "_").c_str());
  snprintf(line, sizeof(line), "%u characters", (unsigned)joinPass.length());
  txt(4, 62, 1, COL_DIM, line);
  txt(4, 89, 1, COL_DIM, "ENTER connect   DEL delete");
  txt(4, 100, 1, COL_DIM, "TAB show/hide password");
  txt(4, 111, 1, COL_DIM, "Fn+` back to the list");
}
static void drawWifiJoin() {
  header("WI-FI SETUP");
  char line[64];
  snprintf(line, sizeof(line), "Connecting to %.24s", joinSsid.c_str());
  txt(4, 30, 1, COL_WARN, line);
  txt(4, 112, 1, COL_DIM, "` cancel");
}
static void drawWifiResult() {
  header("WI-FI SETUP");
  txt(4, 30, 1, resultOk ? COL_OK : COL_BAD, resultText.c_str());
  if (resultOk) {
    char line[40];
    snprintf(line, sizeof(line), "IP %s", WiFi.localIP().toString().c_str());
    txt(4, 46, 1, COL_TEXT, line);
    txt(4, 112, 1, COL_DIM, "any key: radio");
  } else {
    txt(4, 100, 1, COL_DIM, "ENTER: retype password");
    txt(4, 111, 1, COL_DIM, "`: back to the list");
  }
}
static void drawWifiSaved() {
  header("WI-FI SETUP");
  txt(4, 20, 1, COL_DIM, "Saved networks (* = tried first)");
  if (savedCount == 0) txt(4, 34, 1, COL_WARN, "No saved networks.");
  for (int i = 0; i < savedCount; i++) {
    char line[48];
    snprintf(line, sizeof(line), "%c%c%.32s", i == savedSel ? '>' : ' ', i == savedLast ? '*' : ' ', saved[i].ssid);
    txt(4, 34 + i * 12, 1, i == savedSel ? COL_AMBER : COL_TEXT, line);
  }
  if (askDelete) {
    char line[48];
    snprintf(line, sizeof(line), "Delete \"%.24s\"? Y / N", saved[savedSel].ssid);
    txt(4, 78, 1, COL_WARN, line);
  } else if (savedMsg.length()) txt(4, 78, 1, COL_DIM, savedMsg.c_str());
  if (savedCount) txt(4, 100, 1, COL_DIM, "ENTER connect   DEL delete");
  txt(4, 111, 1, COL_DIM, "` back to the list");
}
static void handleMove(const KeyInput& k, const KnobInput& kn, int& sel, int count) {
  int move = kn.detents + (hasChar(k, '.') ? 1 : 0) - (hasChar(k, ';') ? 1 : 0);
  if (move && count > 0) { sel = constrain(sel + move, 0, count - 1); screenDirty = true; }
}
static void handleWifiList(const KeyInput& k, const KnobInput& kn) {
  // TCA8418 can occasionally omit a queued key edge while the asynchronous scan is updating.
  // Read the live matrix too, but collapse the live state and queued character into one toggle.
  static bool xWasDown = false;
  bool xDown = M5Cardputer.Keyboard.isKeyPressed('x') || M5Cardputer.Keyboard.isKeyPressed('X');
  static uint32_t lastXMs = 0;
  bool xPressed = hasCharI(k, 'x') || (xDown && !xWasDown);
  xWasDown = xDown;
  if (xPressed && millis() - lastXMs > 750) {
    lastXMs = millis();
    // X is an explicit user command. Never refuse it because a momentary USB request timed out:
    // the same screen remains usable while Wi-Fi is off, so X can always turn Wi-Fi back on.
    wifiListMsg = "";
    setWifiPaused(!wifiPaused);
    return;
  }
  if (wifiListMsg.length()) { wifiListMsg = ""; screenDirty = true; return; }
  if (scanning) { collectScan(); if (!scanning) screenDirty = true; }
  if (hasChar(k, '`')) { leaveWifiSetup(); return; }
  if (hasCharI(k, 'r')) { startScan(); return; }
  if (hasCharI(k, 's')) { savedSel = savedCount ? savedLast : 0; askDelete = false; savedMsg = ""; setScreen(SC_WIFI_SAVED); return; }
  if (netCount == 0) return;
  handleMove(k, kn, netSel, netCount);
  if (k.enter || kn.shortPress) {
    joinSsid = nets[netSel].ssid;
    joinPass = "";
    showPass = false;
    if (nets[netSel].open) startJoin(); else setScreen(SC_WIFI_PASS);
  }
}
static void handleWifiPass(const KeyInput& k) {
  if (k.fn && hasChar(k, '`')) { setScreen(SC_WIFI_LIST); return; }
  bool changed = false;
  if (k.del && joinPass.length()) { joinPass.remove(joinPass.length() - 1); changed = true; }
  if (k.tab) { showPass = !showPass; changed = true; }
  if (!k.fn)
    for (size_t i = 0; i < k.chars.length() && joinPass.length() < 63; i++) {
      char c = k.chars[i];
      if (c >= 0x20 && c < 0x7F) { joinPass += c; changed = true; }
    }
  if (k.enter) { startJoin(); return; }
  if (changed) screenDirty = true;
}
static void handleWifiJoin(const KeyInput& k) {
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    String replaced = saveJoined();
    bool ok = prefsSaveWifi();
    strlcpy(wifiActive, joinSsid.c_str(), sizeof(wifiActive));
    resultText = ok ? (replaced.length() ? "Connected and saved (replaced " + replaced.substring(0, 12) + ")" : String("Connected and saved.")) : String("Connected. Saving failed.");
    resultOk = true;
    wifiState = W_UP;
    statusRev++;
    setScreen(SC_WIFI_RESULT);
    return;
  }
  bool failed = (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) && millis() - joinStartMs > 3000;
  if (failed || millis() - joinStartMs >= WIFI_JOIN_TIMEOUT_MS || hasChar(k, '`')) {
    WiFi.disconnect(false, false);
    resultOk = false;
    resultText = st == WL_NO_SSID_AVAIL ? "Network not found." : "Could not connect. Check the password.";
    setScreen(SC_WIFI_RESULT);
    return;
  }
  static uint32_t lastDot = 0;
  if (millis() - lastDot > 500) {
    lastDot = millis();
    int dots = (int)((millis() - joinStartMs) / 500) % 35;
    lcd.fillRect(4 + dots * 6, 56, 5, 8, COL_WARN);
  }
}
static void handleWifiSaved(const KeyInput& k, const KnobInput& kn) {
  if (askDelete) {
    if (hasCharI(k, 'y')) {
      String name = saved[savedSel].ssid;
      for (int j = savedSel; j + 1 < savedCount; j++) saved[j] = saved[j + 1];
      savedCount--;
      memset(&saved[savedCount], 0, sizeof(saved[0]));
      if (savedLast > savedSel) savedLast--; else if (savedLast == savedSel || savedCount == 0) savedLast = 0;
      savedMsg = prefsSaveWifi() ? "Deleted: " + name.substring(0, 24) : String("Delete failed");
      if (savedSel >= savedCount) savedSel = savedCount ? savedCount - 1 : 0;
      askDelete = false; screenDirty = true;
    } else if (hasCharI(k, 'n') || hasChar(k, '`')) { askDelete = false; screenDirty = true; }
    return;
  }
  if (hasChar(k, '`')) { setScreen(SC_WIFI_LIST); return; }
  if (savedCount == 0) return;
  handleMove(k, kn, savedSel, savedCount);
  if (k.del) { askDelete = true; screenDirty = true; }
  else if (k.enter || kn.shortPress) {
    joinSsid = saved[savedSel].ssid;
    joinPass = saved[savedSel].pass;
    showPass = false;
    startJoin();
  }
}

// ------------------------------------------------------------------ help
static void drawHelp() {
  header("KEYS");
  txt(4, 19, 1, COL_TEXT, "knob turn: tune (AUTO: scan)");
  txt(4, 29, 1, COL_TEXT, "knob hold+turn: volume  press: mute");
  txt(4, 39, 1, COL_TEXT, ";/. or +/-  volume   M mute");
  txt(4, 49, 1, COL_TEXT, ",//  tune -/+   1-4 step");
  txt(4, 59, 1, COL_TEXT, "A/U/L mode   F type freq");
  txt(4, 69, 1, COL_TEXT, "TAB or T: AUTO <-> MANUAL");
  txt(4, 79, 1, COL_TEXT, "Esc or knob press: stop a scan");
  txt(4, 89, 1, COL_TEXT, "G globe   N/P next/prev KiwiSDR");
  txt(4, 99, 1, COL_TEXT, "W Wi-Fi (X off/on) I PC IP H help");
  txt(4, 109, 1, COL_TEXT, "O speaker Mac/ADV   D diagnostics");
  txt(4, 122, 1, COL_DIM, "any key: back   " BUILD_TAG);
}

// ------------------------------------------------------------------ diagnostics (D): where is a drop-out coming from?
static uint32_t diagDrawMs = 0;
static void drawDiag(bool full) {
  if (!full && millis() - diagDrawMs < 500) return;
  diagDrawMs = millis();
  char l[48];
  lcd.fillScreen(COL_BG);
  txt(4, 3, 1, COL_AMBER, "DIAGNOSTICS");
  txt(150, 3, 1, COL_DIM, BUILD_TAG);
  lcd.drawFastHLine(0, 13, 240, COL_DIM);
  int y = 17;
  auto line = [&](uint16_t c, const char* t) { txt(4, y, 1, c, t); y += 10; };
  bool wifi = WiFi.status() == WL_CONNECTED;
  snprintf(l, sizeof l, "WIFI %s %ddBm", wifi ? WiFi.SSID().substring(0, 14).c_str() : "-", wifi ? WiFi.RSSI() : 0);
  line(wifi ? COL_OK : COL_BAD, l);
  { Lock lk; snprintf(l, sizeof l, "MAC  %.15s %s", macIP.c_str(), rem.ok ? (usbPcOk ? "CABLE ok" : "Wi-Fi ok") : "not connected"); line(rem.ok ? COL_OK : COL_WARN, l);
    snprintf(l, sizeof l, "KIWI %s %.12s %.0fdBm", rem.link, rem.server, rem.rssi); line(strcmp(rem.link, "connected") == 0 ? COL_OK : COL_WARN, l); }
  uint8_t as = auStateNow();
  snprintf(l, sizeof l, "OUT  %s %s", wantOutput ? "ADV" : "PC", wantOutput ? auStateName(as) : "(PC plays)");
  line(wantOutput && as != AU_PLAYING ? COL_WARN : COL_TEXT, l);
  if (wantOutput) {
    unsigned fillMs = auRing ? (unsigned)(xStreamBufferBytesAvailable(auRing) / 2 * 1000 / 12000) : 0;
    snprintf(l, sizeof l, "BUF  %ums rx %u gaps %u/%u", fillMs, (unsigned)auNet.frames, (unsigned)auNet.gaps, (unsigned)auNet.lostFrames); line(COL_TEXT, l);
    snprintf(l, sizeof l, "NET  maxgap %ums reconn %u", (unsigned)auNet.maxGapMs, (unsigned)auNet.reconnects); line(auNet.maxGapMs > 150 ? COL_WARN : COL_TEXT, l);
    snprintf(l, sizeof l, "PLAY under %u over %u rebuf %u", (unsigned)auPlayout.underruns, (unsigned)auPlayout.overruns, (unsigned)auPlayout.rebuffers); line(auPlayout.underruns ? COL_WARN : COL_TEXT, l);
    { Lock lk; snprintf(l, sizeof l, "MAC  sent %u dropped %u disc %u", (unsigned)rem.audioSent, (unsigned)rem.audioDropped, (unsigned)rem.audioDisc); line(rem.audioDropped ? COL_WARN : COL_TEXT, l); }
  } else {
    line(COL_DIM, "(audio counters: choose O = ADV)");
  }
  snprintf(l, sizeof l, "HEAP %u min %u blk %u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  line(COL_TEXT, l);
  // SOF+ = the host is sending USB frames to the OTG controller. mount = USB enumeration completed;
  // cdc = the host application currently has the serial port open.
  static uint32_t lastSof = 0;
  uint32_t sof = (USB0.dsts >> 8) & 0x3FFF;
  bool sofMoving = sof != lastSof;
  lastSof = sof;
  bool mounted = (bool)USB;
  bool cdc = (bool)Serial;
  snprintf(l, sizeof l, "USB  SOF%c m%d cdc%d up%lu dn%lu", sofMoving ? '+' : '-', (int)mounted, (int)cdc,
           (unsigned long)usbMounts, (unsigned long)usbUnmounts);
  line(cdc ? COL_OK : mounted ? COL_WARN : COL_BAD, l);
  txt(4, 124, 1, COL_DIM, "any key: back");
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  // PC state replies exceed the default 256-byte CDC queue. Keep the whole JSON frame so USB-only control cannot
  // lose its tail. USB itself is started by the Arduino core before setup() when CDCOnBoot=cdc.
  USB.onEvent(onUsbEvent);
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  Serial.printf("[BOOT] RADIO DAYS SDR %s TinyUSB rtc=0x%06lx\n", BUILD_TAG, (unsigned long)REG_READ(RTC_CNTL_USB_CONF_REG));
  mtx = xSemaphoreCreateMutex();
  lcd.begin();
  auto cfg = M5.config();
  cfg.clear_display = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = true;    // use the battery-backed RTC when this ADV revision provides one
  cfg.internal_spk = true;   // pins and amplifier set-up are known to M5Unified; the speaker is only STARTED when O selects it
  cfg.internal_mic = false;
  cfg.external_imu = false;  // Port A belongs to the U135
  cfg.external_rtc = false;
  cfg.serial_baudrate = 0;
  M5Cardputer.begin(cfg, true);
  setenv("TZ", "JST-9", 1);
  tzset();
  lcd.setRotation(1);
  lcd.setBrightness(128);
  lcd.setFont(&fonts::Font0);
  lcd.fillScreen(COL_BG);
  txt(4, 50, 2, COL_AMBER, "RADIO DAYS SDR");
  txt(4, 74, 1, COL_DIM, BUILD_TAG);

  analogBegin();  // sprites for the analogue dial and meter, taken before Wi-Fi
  prefsLoad();
  Wire.begin(PORT_SDA, PORT_SCL, I2C_FREQ);
  u135Probe();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // wifiService() decides
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, PRO_CPU_NUM);
  // Audio receive must keep draining TCP even while the HTTP state task is busy. A higher
  // priority prevents a full receive window from turning a short stall into a send timeout.
  xTaskCreatePinnedToCore(audioNetTask, "aunet", 6144, nullptr, 2, nullptr, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(audioPlayTask, "auplay", 4096, nullptr, 2, nullptr, APP_CPU_NUM);
  // Start the radio even without saved Wi-Fi: OUT PC can be controlled entirely over USB.
  // W opens optional Wi-Fi setup; USB alone supports both output modes.
  screenDirty = true;
}

void loop() {
  M5Cardputer.update();
  usbLinkService();
  KeyInput k = keyboardPoll();
  KnobInput kn = u135Poll();
  if (screen != SC_WIFI_LIST && screen != SC_WIFI_PASS && screen != SC_WIFI_JOIN && screen != SC_WIFI_SAVED && screen != SC_WIFI_RESULT)
    wifiService();
  clockService();
  prefsSaveRadioIfIdle();
  audioService();

  if (requestWifiScreen) {
    requestWifiScreen = false;
    if (wifiPaused) setScreen(SC_WIFI_LIST); else startScan();
  }
  if (requestWifiToggle) {
    requestWifiToggle = false;
    wifiListMsg = "";
    setWifiPaused(!wifiPaused);
  }
  if (requestMacIpScreen) {
    requestMacIpScreen = false;
    { Lock l; entry = macIP; }
    entryMsg = "";
    setScreen(SC_MACIP);
  }
  if (requestHelpScreen) { requestHelpScreen = false; setScreen(SC_HELP); }
  if (requestGlobeScreen) { requestGlobeScreen = false; globeOpen(); }
  if (requestDiagScreen) { requestDiagScreen = false; setScreen(SC_DIAG); }
  if (requestRadioScreen) { requestRadioScreen = false; if (screen == SC_GLOBE) globeClose(); else setScreen(SC_RADIO); }

  // ==== DISPATCH BEGIN (extracted verbatim by firmware/tools/host_render to test screen changes)
  switch (screen) {
    case SC_RADIO:
      handleRadio(k, kn);
      if (screen == SC_RADIO) { drawRadio(screenDirty); screenDirty = false; }
      else screenDirty = true;
      break;
    case SC_WIFI_LIST: handleWifiList(k, kn); break;
    case SC_WIFI_PASS: handleWifiPass(k); break;
    case SC_WIFI_JOIN: handleWifiJoin(k); break;
    case SC_WIFI_SAVED: handleWifiSaved(k, kn); break;
    case SC_WIFI_RESULT:
      if (resultOk) { if (anyKey(k) || kn.shortPress) leaveWifiSetup(); }
      else if (hasChar(k, '`')) setScreen(SC_WIFI_LIST);
      else if (k.enter || kn.shortPress) setScreen(SC_WIFI_PASS);
      break;
    case SC_FREQ: handleEntry(k, true, 9, acceptFreq); break;
    case SC_MACIP: handleEntry(k, true, 15, acceptMacIP); break;
    case SC_GLOBE:
      handleGlobe(k, kn);
      if (screen == SC_GLOBE) { drawGlobe(screenDirty); screenDirty = false; }
      else screenDirty = true;
      break;
    case SC_DIAG:
      if (anyKey(k) || kn.shortPress) { setScreen(SC_RADIO); break; }
      drawDiag(screenDirty);
      screenDirty = false;
      break;
    case SC_HELP:
      if (anyKey(k) || kn.shortPress) setScreen(SC_RADIO);
      break;
  }
  // Screens that draw themselves (the radio, the globe, diagnostics) get the dirty flag UNTOUCHED: it is their signal
  // to clear the screen and draw everything.  (v1.3 bug: this block cleared the flag right after a key press had
  // switched to the globe, so the globe never cleared the radio screen and drew over it.)
  // The other screens are drawn here, only when something changed.
  if (screenDirty && screen != SC_RADIO && screen != SC_GLOBE && screen != SC_DIAG) {
    switch (screen) {
      case SC_WIFI_LIST: drawWifiList(); break;
      case SC_WIFI_PASS: drawWifiPass(); break;
      case SC_WIFI_JOIN: drawWifiJoin(); break;
      case SC_WIFI_SAVED: drawWifiSaved(); break;
      case SC_WIFI_RESULT: drawWifiResult(); break;
      case SC_FREQ: drawEntry("TYPE FREQUENCY", "kHz (10 - 30000):", "e.g. 9580 or 15770.5"); break;
      case SC_MACIP: drawEntry("PC ADDRESS", "PC IPv4 (blank = auto find):", "see the PC start window"); break;
      case SC_HELP: drawHelp(); break;
      default: break;
    }
    screenDirty = false;
  }
  // ==== DISPATCH END
  delay(10);
}
