// Types that appear in function signatures of RadioDays_SDR.ino.
#pragma once
#include <Arduino.h>

struct KeyInput {
  String chars;
  bool enter = false, del = false, tab = false, fn = false;
};
struct KnobInput {
  int detents = 0, tuneDetents = 0, volumeDetents = 0;
  bool shortPress = false;
};
struct NetEntry {
  String ssid;
  int rssi;
  bool open;
};

enum Screen : uint8_t { SC_RADIO, SC_WIFI_LIST, SC_WIFI_PASS, SC_WIFI_JOIN, SC_WIFI_SAVED, SC_WIFI_RESULT, SC_MACIP, SC_FREQ, SC_HELP, SC_GLOBE, SC_DIAG };

// The satellite on the globe screen (U4's pose: where it floats, which lamp leads, which wave front is out).
struct SatPose {
  bool shown = false;
  int x = 0, y = 0;            // top-left on the screen
  uint8_t lamp = 0;            // the leading body lamp
  uint8_t lampMode = 0;        // 0 chase, 1 all on, 2 all dim (while it talks)
  uint8_t wave = 0;            // 0 none, 1 Earth -> satellite (cyan), 2 satellite -> Earth (yellow)
  uint8_t waveStep = 0;
};
