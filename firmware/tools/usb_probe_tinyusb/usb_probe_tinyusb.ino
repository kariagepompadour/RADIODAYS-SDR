/*
  RADIO DAYS USB probe, TinyUSB variant: the same test as tools/usb_probe (Hardware CDC), but with the USB-OTG
  controller and TinyUSB CDC, to compare the two USB paths of the ESP32-S3 on the Cardputer ADV.
  No M5Unified, no Wi-Fi, no tasks. The core starts USB and Serial (= USBSerial, TinyUSB CDC) before setup();
  M5GFX only shows the state on the screen.
  Build: ./build_probe_tinyusb.sh  (USBMode=default = USB-OTG, CDCOnBoot=cdc) -> firmware/bin/USB-PROBE-TINYUSB.ino.bin

  Screen / serial line, refreshed every 500 ms:
    SOF    moving = the host is sending USB frames to the OTG controller
    mount  USB enumerated by the host (ESPUSB::operator bool)
    cdc    a host program has the port open with DTR set (USBCDC::operator bool)
    mnt/umnt/susp  mount, unmount and suspend events since setup()
    rx     bytes received; every received line is echoed back as "ECHO <line>"
    rtc    RTC_CNTL_USB_CONF_REG (non-zero is expected here: TinyUSB routes the PHY to USB-OTG)
  Note: the PHY routing survives a restart. SDR-1.25 (the last Hardware CDC firmware) switched it back at boot;
  SDR-1.26+ use USB-OTG themselves. tools/usb_probe (HWCDC) does not switch it back, so power-cycle the Cardputer
  before running that probe after this one.
*/
#include <M5GFX.h>
#include <USB.h>
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_struct.h"

#define PROBE_TAG "TinyUSB PROBE"

static M5GFX display;
static volatile uint32_t mounts = 0, unmounts = 0, suspends = 0, rxBytes = 0;

static void onUsbEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == ARDUINO_USB_EVENTS) {
    if (id == ARDUINO_USB_STARTED_EVENT) mounts++;
    else if (id == ARDUINO_USB_STOPPED_EVENT) unmounts++;
    else if (id == ARDUINO_USB_SUSPEND_EVENT) suspends++;
  } else if (base == ARDUINO_USB_CDC_EVENTS && id == ARDUINO_USB_CDC_RX_EVENT) {
    rxBytes += ((arduino_usb_cdc_event_data_t*)data)->rx.len;
  }
}

void setup() {
  USB.onEvent(onUsbEvent);
  Serial.onEvent(onUsbEvent);
  Serial.printf("[BOOT] %s\n", PROBE_TAG);
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

  uint32_t sof = (USB0.dsts >> 8) & 0x3FFF;  // DSTS.SOFFN: frame number of the last SOF received
  bool sofMoving = sof != lastSof;
  lastSof = sof;
  bool mounted = (bool)USB;
  bool cdc = (bool)Serial;
  uint32_t rtc = REG_READ(RTC_CNTL_USB_CONF_REG);

  Serial.printf("TPROBE %lu sof=%s mount=%d cdc=%d mnt=%lu umnt=%lu susp=%lu rx=%lu rtc=0x%06lx\n", (unsigned long)++n,
                sofMoving ? "moving" : "stopped", mounted, cdc, (unsigned long)mounts, (unsigned long)unmounts,
                (unsigned long)suspends, (unsigned long)rxBytes, (unsigned long)rtc);

  char l[48];
  int y = 4;
  auto line = [&](uint16_t col, int size, const char* t) {
    display.setTextSize(size);
    display.setTextColor(col, TFT_BLACK);
    display.setCursor(4, y);
    display.printf("%-38s", t);
    y += size * 10;
  };
  line(TFT_CYAN, 2, PROBE_TAG);
  snprintf(l, sizeof l, "SOF %s", sofMoving ? "moving" : "STOPPED");
  line(sofMoving ? TFT_GREEN : TFT_RED, 2, l);
  snprintf(l, sizeof l, "mount %d  cdc %d", mounted, cdc);
  line(mounted ? TFT_GREEN : TFT_YELLOW, 1, l);
  snprintf(l, sizeof l, "mnt %lu umnt %lu susp %lu", (unsigned long)mounts, (unsigned long)unmounts, (unsigned long)suspends);
  line(TFT_WHITE, 1, l);
  snprintf(l, sizeof l, "rx %lu bytes  lines %lu", (unsigned long)rxBytes, (unsigned long)n);
  line(TFT_WHITE, 1, l);
  snprintf(l, sizeof l, "rtc 0x%06lx  (USB-OTG / TinyUSB)", (unsigned long)rtc);
  line(TFT_WHITE, 1, l);
  y += 6;
  line(TFT_DARKGREY, 1, "SOF moving + mount 1 = host sees us.");
  line(TFT_DARKGREY, 1, "SOF stopped = no USB from the host.");
}
