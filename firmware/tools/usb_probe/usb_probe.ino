/*
  RADIO DAYS USB probe: the smallest Hardware CDC (USB Serial/JTAG) app, to tell whether the Mac can enumerate the
  Cardputer ADV at all when an app is started from the Launcher.
  No M5Unified, no Wi-Fi, no tasks: Serial.begin() first, then M5GFX only to show the USB state on the screen.
  Build: ./build_probe.sh  (same FQBN and USB settings as the radio firmware) -> firmware/bin/USB-PROBE-HWCDC.ino.bin

  Screen / serial line, refreshed every 500 ms:
    SOF   moving = the host is sending USB frames to us (electrically attached and enumerated or enumerating)
    plug  HWCDC::isPlugged()   (IDF SOF watchdog)
    cdc   HWCDC::isConnected() (the host has picked up an IN packet)
    rst   BUS_RESET events seen since boot (each host enumeration starts with one)
    rx    bytes received; every received line is echoed back as "ECHO <line>"
    conf0 / rtc  USB_SERIAL_JTAG_CONF0_REG and RTC_CNTL_USB_CONF_REG
*/
#include <M5GFX.h>
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_serial_jtag_struct.h"

#define PROBE_TAG "HWCDC PROBE"

static M5GFX display;
static volatile uint32_t busResets = 0, rxBytes = 0;

static void onUsbEvent(void*, esp_event_base_t, int32_t id, void* data) {
  if (id == ARDUINO_HW_CDC_BUS_RESET_EVENT) busResets++;
  else if (id == ARDUINO_HW_CDC_RX_EVENT) rxBytes += ((arduino_hw_cdc_event_data_t*)data)->rx.len;
}

void setup() {
  uint32_t rtcAtBoot = REG_READ(RTC_CNTL_USB_CONF_REG);
  Serial.onEvent(onUsbEvent);
  Serial.begin();
  Serial.printf("[BOOT] %s rtc_usb_conf=0x%06lx\n", PROBE_TAG, (unsigned long)rtcAtBoot);
  display.begin();
  display.setRotation(1);
  display.setFont(&fonts::Font0);
  display.fillScreen(TFT_BLACK);
}

void loop() {
  static uint32_t lastDraw = 0, lastSof = 0, n = 0;
  static String rxLine;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') { Serial.printf("ECHO %s\n", rxLine.c_str()); rxLine = ""; }
    else if (c != '\r' && rxLine.length() < 120) rxLine += c;
  }
  if (millis() - lastDraw < 500) { delay(5); return; }
  lastDraw = millis();

  uint32_t sof = USB_SERIAL_JTAG.fram_num.sof_frame_index;
  bool sofMoving = sof != lastSof;
  lastSof = sof;
  bool plugged = HWCDC::isPlugged();
  bool cdc = HWCDC::isConnected();
  uint32_t conf0 = USB_SERIAL_JTAG.conf0.val;
  uint32_t rtc = REG_READ(RTC_CNTL_USB_CONF_REG);
  struct { int usb_pad_enable, dp_pullup, pad_pull_override, phy_sel; } c0 = {
    (int)USB_SERIAL_JTAG.conf0.usb_pad_enable, (int)USB_SERIAL_JTAG.conf0.dp_pullup,
    (int)USB_SERIAL_JTAG.conf0.pad_pull_override, (int)USB_SERIAL_JTAG.conf0.phy_sel};

  Serial.printf("PROBE %lu sof=%s plug=%d cdc=%d rst=%lu rx=%lu conf0=0x%05lx rtc=0x%06lx\n", (unsigned long)++n,
                sofMoving ? "moving" : "stopped", plugged, cdc, (unsigned long)busResets, (unsigned long)rxBytes,
                (unsigned long)conf0, (unsigned long)rtc);

  char l[48];
  int y = 4;
  auto line = [&](uint16_t col, int size, const char* t) {
    display.setTextSize(size);
    display.setTextColor(col, TFT_BLACK);
    display.setCursor(4, y);
    display.printf("%-38s", t);
    y += size * 10;
  };
  line(TFT_ORANGE, 2, PROBE_TAG);
  snprintf(l, sizeof l, "SOF %s", sofMoving ? "moving" : "STOPPED");
  line(sofMoving ? TFT_GREEN : TFT_RED, 2, l);
  snprintf(l, sizeof l, "plug %d  cdc %d  rst %lu", plugged, cdc, (unsigned long)busResets);
  line(cdc ? TFT_GREEN : TFT_YELLOW, 1, l);
  snprintf(l, sizeof l, "rx %lu bytes  lines %lu", (unsigned long)rxBytes, (unsigned long)n);
  line(TFT_WHITE, 1, l);
  snprintf(l, sizeof l, "conf0 0x%05lx  rtc 0x%06lx", (unsigned long)conf0, (unsigned long)rtc);
  line(TFT_WHITE, 1, l);
  snprintf(l, sizeof l, "pad %d dp_pu %d ovr %d phy %d", c0.usb_pad_enable, c0.dp_pullup, c0.pad_pull_override, c0.phy_sel);
  line(c0.usb_pad_enable && c0.dp_pullup && !c0.pad_pull_override && !c0.phy_sel ? TFT_WHITE : TFT_RED, 1, l);
  y += 6;
  line(TFT_DARKGREY, 1, "SOF moving + rst>0 = host sees us.");
  line(TFT_DARKGREY, 1, "SOF stopped = no USB from the host.");
}
