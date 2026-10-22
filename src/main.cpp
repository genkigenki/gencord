// gencord - voice recorder for the original M5Stack Cardputer (StampS3)
//
//  - G0 button (from sleep or menu): start recording. Double-click G0: stop.
//  - Recording runs with the screen off; any key shows the status for 3 s.
//  - After stopping you can type a name (Enter / G0 / timeout saves).
//  - Files: /rec/YYYY-MM-DD_HH-MM-SS[_name].wav (16 kHz mono 16-bit).
//  - A USB microphone on USB-C (DJI Mic Mini 2 receiver) is used instead of the
//    built-in mic when one answers at the start of a recording (see usb_mic.h).
//  - When the home Wi-Fi is in range, every recording is uploaded to the
//    Synology via WebDAV, verified by size, and only then deleted locally.
//  - Between uses the device is in deep sleep; with pending files it wakes
//    every WIFI_CHECK_MIN minutes to look for the home network.

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <base64.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <freertos/queue.h>
#include <driver/i2s_pdm.h>
#include <Preferences.h>
#include <sys/time.h>
#include <vector>
#include "usb_mic.h"

// Settings come from /recorder.cfg on the SD card (editable on the device with 'c').
// config.h (optional, gitignored) only provides defaults for a private build.
#if __has_include("config.h")
#include "config.h"
#endif
#define GENCORD_VERSION "v0.11"  // shown on the menu cassette; raise with every flash

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef DAV_URL
#define DAV_URL ""
#endif
#ifndef DAV_USER
#define DAV_USER ""
#endif
#ifndef DAV_PASS
#define DAV_PASS ""
#endif
#ifndef TZ_INFO
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 16000
#endif
#ifndef AGC_MAX
#define AGC_MAX 64  // maximum automatic gain
#endif
#ifndef WIFI_CHECK_MIN
#define WIFI_CHECK_MIN 20
#endif
#ifndef IDLE_SLEEP_S
#define IDLE_SLEEP_S 60
#endif
#ifndef MENU_SCREEN_S
#define MENU_SCREEN_S 20  // menu: screen off after N s without input (device stays awake)
#endif
#ifndef NAME_TIMEOUT_S
#define NAME_TIMEOUT_S 30
#endif
#ifndef LOW_BATT_WARN_MV
#define LOW_BATT_WARN_MV 3550
#endif
#ifndef LOW_BATT_MV
#define LOW_BATT_MV 3400
#endif
#ifndef SCREEN_BRIGHTNESS
#define SCREEN_BRIGHTNESS 60
#endif

struct Settings {
  String ssid = WIFI_SSID, pass = WIFI_PASS;
  String davUrl = DAV_URL, davUser = DAV_USER, davPass = DAV_PASS;
  String tz = TZ_INFO, ntp = NTP_SERVER;
  int agcMax = AGC_MAX, wifiCheckMin = WIFI_CHECK_MIN;
  bool usbMic = true;  // look for a USB microphone (DJI receiver) when recording on battery
} S;
static constexpr const char* CFG_PATH = "/recorder.cfg";

// ---------------------------------------------------------------- hardware
static constexpr int PIN_SD_SCK = 40, PIN_SD_MISO = 39, PIN_SD_MOSI = 14, PIN_SD_CS = 12;
static constexpr gpio_num_t PIN_BTN = GPIO_NUM_0;
// Held low during deep sleep: keyboard 74HC138 address lines, mic clock, backlight.
static constexpr gpio_num_t HOLD_LOW_PINS[] = {GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11, GPIO_NUM_43, GPIO_NUM_38};

static constexpr const char* REC_DIR = "/rec";
static constexpr const char* LOG_PATH = "/log.csv";
static constexpr size_t LOG_MAX_BYTES = 256 * 1024;

// ---------------------------------------------------------------- audio buffers
// 12 x 250 ms: the mic task keeps recording for ~3 s even if the SD card stalls.
static constexpr size_t BUF_SAMPLES = SAMPLE_RATE / 4;
static constexpr int NBUF = 12;
static int16_t bufs[NBUF][BUF_SAMPLES];
static QueueHandle_t filledQ, freeQ;


// ---------------------------------------------------------------- state kept in deep sleep
RTC_DATA_ATTR static uint32_t rtcBoots = 0;
RTC_DATA_ATTR static uint16_t rtcPending = 0;
RTC_DATA_ATTR static time_t rtcLastSync = 0;
RTC_DATA_ATTR static int8_t rtcLastSyncOk = -1;  // -1 never, 0 failed, 1 ok
RTC_DATA_ATTR static uint8_t rtcWifiCh = 0;
RTC_DATA_ATTR static uint8_t rtcWifiBssid[6];
RTC_DATA_ATTR static uint8_t rtcScanMiss = 0;

static bool sdOk = false;
static bool screenIsOn = false;

// ================================================================ small helpers
static bool timeValid() { return time(nullptr) > 1704067200; }  // after 2024-01-01

static String timeStr(const char* fmt) {
  if (!timeValid()) return "--";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char b[40];
  strftime(b, sizeof(b), fmt, &t);
  return b;
}

// The clock survives deep sleep, but not the power switch. The last known time is
// kept in NVS and restored at power-on, so it is at least approximate until the
// next NTP sync (shown as "~" in the menu).
RTC_DATA_ATTR static bool rtcTimeExact = false;  // NTP or manually set since power-on

static void saveClock() {
  if (!timeValid()) return;
  Preferences p;
  p.begin("gencord", false);
  p.putLong64("t", (int64_t)time(nullptr));
  p.end();
}

static bool restoreClock() {
  if (timeValid()) return false;
  Preferences p;
  p.begin("gencord", true);
  int64_t t = p.getLong64("t", 0);
  p.end();
  if (t < 1704067200) return false;
  struct timeval tv = {(time_t)t, 0};
  settimeofday(&tv, nullptr);
  rtcTimeExact = false;
  return true;
}

static int battMv() { return M5.Power.getBatteryVoltage(); }
static int battPct() { return M5.Power.getBatteryLevel(); }

static void logEvent(const char* ev, const String& detail = "") {
  if (!sdOk) return;
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  if (f.size() > LOG_MAX_BYTES) {
    f.close();
    SD.remove(LOG_PATH);
    f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) return;
  }
  if (f.size() == 0) f.println("time,boot,event,batt_mv,detail");
  f.printf("%s,%lu,%s,%d,%s\n", timeStr("%Y-%m-%d %H:%M:%S").c_str(), (unsigned long)rtcBoots, ev, battMv(),
           detail.c_str());
  f.close();
}

// ================================================================ screen
static void screenOn() {
  if (screenIsOn) return;
  M5.Display.wakeup();
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  screenIsOn = true;
}

static void screenOff() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  screenIsOn = false;
}

// Frames are composed off-screen (8-bit, 32 KB) and pushed in one go: no black flash
// between redraws. Identical frames are skipped altogether.
static M5Canvas canvas(&M5.Display);
static String lastFrame;

// Wi-Fi state for the header icon: 0 = radio off (no icon), 1 = searching/connecting
// (grey), 2 = connected (white).
static int wifiState() {
  if (WiFi.getMode() == WIFI_OFF) return 0;
  return WiFi.status() == WL_CONNECTED ? 2 : 1;
}

// Classic Wi-Fi fan: dot plus three arcs, (cx, cy) = the dot.
static void wifiIcon(LovyanGFX& d, int cx, int cy) {
  const int st = wifiState();
  if (!st) return;
  const uint16_t c = st == 2 ? TFT_WHITE : TFT_DARKGREY;
  d.fillCircle(cx, cy, 2, c);
  for (int r = 4; r <= 12; r += 4) d.fillArc(cx, cy, r, r - 2, 225, 315, c);  // 2 px arcs, 2 px gaps
}

static bool micUsb = false;  // the running recording comes from the USB microphone

// Wireless mic (DJI receiver on USB-C): capsule on a stand plus two radio waves.
static void micIcon(LovyanGFX& d, int cx, int cy) {
  if (!micUsb) return;
  const uint16_t c = TFT_CYAN;
  d.fillRoundRect(cx - 3, cy - 9, 7, 11, 3, c);
  d.drawArc(cx, cy - 2, 6, 5, 0, 180, c);
  d.drawFastVLine(cx, cy + 4, 3, c);
  d.drawFastHLine(cx - 3, cy + 7, 7, c);
  d.drawArc(cx, cy - 4, 10, 9, 300, 345, c);
  d.drawArc(cx, cy - 4, 10, 9, 195, 240, c);
}

static void ui(const String& title, uint16_t color, const String& l1 = "", const String& l2 = "",
               const String& l3 = "", const String& l4 = "") {
  String frame = title + '\x1f' + color + '\x1f' + l1 + '\x1f' + l2 + '\x1f' + l3 + '\x1f' + l4 + wifiState() + micUsb;
  if (frame == lastFrame) return;
  lastFrame = frame;
  if (!canvas.getBuffer()) {
    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }
  const bool buffered = canvas.getBuffer() != nullptr;
  LovyanGFX& d = buffered ? (LovyanGFX&)canvas : (LovyanGFX&)M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextColor(color, TFT_BLACK);
  d.setTextSize(title.length() > 13 ? 2 : 3);  // 240 px: 13 chars at size 3, 20 at size 2
  d.setCursor(4, 6);
  d.print(title);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(1.5f);  // 26 chars per line
  const String* lines[] = {&l1, &l2, &l3, &l4};
  for (int i = 0; i < 4; i++) {
    d.setCursor(4, 40 + i * 23);
    d.print(*lines[i]);
  }
  wifiIcon(d, 226, 15);
  micIcon(d, 206, 15);
  d.endWrite();
  if (buffered) canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- cassette screens
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static LovyanGFX& frameBegin() {
  if (!canvas.getBuffer()) {
    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }
  LovyanGFX& d = canvas.getBuffer() ? (LovyanGFX&)canvas : (LovyanGFX&)M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  return d;
}

static void frameEnd(LovyanGFX& d) {
  d.endWrite();
  if (&d == (LovyanGFX*)&canvas) canvas.pushSprite(0, 0);
  lastFrame = "";  // the next ui() call must redraw
}

static void reel(LovyanGFX& d, int cx, int cy, int tapeR, float ang) {
  d.fillCircle(cx, cy, tapeR, rgb(92, 54, 30));       // wound tape
  d.drawCircle(cx, cy, tapeR, rgb(130, 80, 45));
  d.fillCircle(cx, cy, 10, rgb(236, 236, 226));       // white hub
  d.fillCircle(cx, cy, 4, rgb(30, 30, 34));
  for (int i = 0; i < 6; i++) {                       // drive teeth
    float a = ang + i * (float)M_PI / 3;
    d.drawWideLine(cx + cosf(a) * 4, cy + sinf(a) * 4, cx + cosf(a) * 9, cy + sinf(a) * 9, 1.2f, rgb(30, 30, 34));
  }
}

// A compact-cassette: label sticker with text, a window with two reels
// (progress 0..1 moves tape from the left reel to the right), head notch below.
static void cassette(LovyanGFX& d, int x, int y, int w, int h, const String& label, uint16_t stripe, float ang,
                     float progress) {
  const uint16_t body = rgb(48, 48, 56), edge = rgb(120, 120, 132);
  d.fillRoundRect(x, y, w, h, 8, body);
  d.drawRoundRect(x, y, w, h, 8, edge);
  for (int sx : {x + 6, x + w - 7})
    for (int sy : {y + 6, y + h - 7}) d.fillCircle(sx, sy, 2, edge);  // screws

  const int lx = x + 12, ly = y + 5, lw = w - 24, lh = h - 30;
  d.fillRoundRect(lx, ly, lw, lh, 5, rgb(250, 238, 204));
  d.fillRect(lx, ly + 17, lw, 4, stripe);
  d.fillRect(lx, ly + 23, lw, 2, rgb(240, 160, 60));
  // Name in bold, like a felt-tip on a tape label; long names show their end.
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextSize(1);
  d.setTextColor(rgb(25, 20, 20));
  d.setTextDatum(textdatum_t::top_left);
  String shown = label;
  while (shown.length() > 1 && d.textWidth(shown) > lw - 10) shown.remove(0, 1);
  d.drawString(shown, lx + 5, ly + 1);
  d.setFont(&fonts::Font0);

  const int wh = lh - 30, wy = ly + 28, ww = w / 2, wx = x + (w - ww) / 2;
  d.fillRoundRect(wx, wy, ww, wh, wh / 2, rgb(18, 18, 22));
  d.setClipRect(wx, wy, ww, wh);
  const int r0 = 11, r1 = wh / 2 + 8;
  const int cy = wy + wh / 2, cl = wx + wh / 2 + 2, cr = wx + ww - wh / 2 - 2;
  reel(d, cl, cy, r0 + (r1 - r0) * (1 - progress), ang);
  reel(d, cr, cy, r0 + (r1 - r0) * progress, ang);
  d.clearClipRect();

  const int by = y + h - 14, bw = w / 2;  // head area
  d.fillRect(x + (w - bw) / 2, by, bw, 14, rgb(72, 72, 82));
  d.fillTriangle(x + (w - bw) / 2 - 10, y + h - 1, x + (w - bw) / 2, by, x + (w - bw) / 2, y + h - 1, rgb(72, 72, 82));
  d.fillTriangle(x + (w + bw) / 2 + 10, y + h - 1, x + (w + bw) / 2, by, x + (w + bw) / 2, y + h - 1, rgb(72, 72, 82));
  for (int hx : {x + w / 2 - 22, x + w / 2 + 22}) d.fillCircle(hx, by + 7, 3, body);
}

static void footer(LovyanGFX& d, const String& a, const String& b) {
  d.setTextSize(1);
  d.setTextColor(rgb(200, 200, 200));
  d.setCursor(4, 116);
  d.print(a);
  d.setTextColor(rgb(140, 140, 150));
  d.setCursor(4, 126);
  d.print(b);
}

static volatile int32_t levelPeak = 0;  // peak of the latest 125 ms block (set in toMono)
static float agcGain = 4.0f;            // current automatic gain (see toMono)

// 0..1 position of the AGC gain on a log scale x1..x64.
static float gainFrac() { return constrain(log2f(max(agcGain, 1.0f)) / 6.0f, 0.0f, 1.0f); }

// Green (x1) -> yellow -> orange -> red (x64).
static uint16_t gainColor() {
  const float t = gainFrac();
  const uint8_t r = t < 0.5f ? (uint8_t)(510 * t) : 255;
  const uint8_t g = t < 0.5f ? 220 : (uint8_t)(220 * (1.0f - t) * 2.0f);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3);
}

// Square badge with the current gain in black, e.g. "x10".
static void gainBadge(LovyanGFX& d, int x, int y, int w, int h) {
  d.fillRoundRect(x, y, w, h, 3, gainColor());
  d.setTextSize(1);
  d.setTextColor(TFT_BLACK);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString("x" + String((int)lroundf(agcGain)), x + w / 2, y + h / 2 + 1);
  d.setTextDatum(textdatum_t::top_left);
}

// Old-style mono LED bar right of the cassette: 16 segments, -48..0 dBFS, rising
// upwards, with a peak-hold LED that stays 1.5 s and then drops one step per 120 ms.
// The white pointer on the left edge is the AGC gain (bottom x1 .. top x64).
static void levelMeter(LovyanGFX& d, int x, int y, int w, int h) {
  const int py = y + h - 1 - (int)(gainFrac() * (h - 2));
  d.fillTriangle(x - 9, py - 4, x - 9, py + 4, x - 3, py, TFT_WHITE);
  static int hold = 0;
  static uint32_t holdSince = 0, lastDrop = 0;
  const int segs = 16, segH = h / segs;
  const float db = levelPeak > 0 ? 20.0f * log10f(levelPeak / 32768.0f) : -96.0f;
  const int lit = constrain((int)lroundf((db + 48.0f) / 48.0f * segs), 0, segs);
  const uint32_t now = millis();
  if (lit >= hold) {
    hold = lit;
    holdSince = now;
  } else if (now - holdSince > 1500 && now - lastDrop > 120) {
    hold--;
    lastDrop = now;
  }
  d.fillRect(x - 2, y, w + 4, h, rgb(16, 16, 18));  // dark housing
  for (int i = 0; i < segs; i++) {                  // i = 0 is the bottom LED
    const uint16_t on = i >= segs - 2 ? TFT_RED : (i >= segs - 5 ? TFT_YELLOW : TFT_GREEN);
    const uint16_t off = i >= segs - 2 ? rgb(60, 10, 10) : (i >= segs - 5 ? rgb(55, 50, 8) : rgb(10, 45, 14));
    const int sy = y + h - (i + 1) * segH;
    const bool isHold = hold > 0 && i == hold - 1;
    d.fillRect(x, sy + 1, w, segH - 2, (i < lit || isHold) ? on : off);  // isHold = max LED stays lit
  }
  d.drawFastHLine(x - 3, py, w + 3, TFT_WHITE);  // AGC line across the meter
}

static void recScreen(uint32_t elapsedMs, const String& label, float mb, int gaps) {
  LovyanGFX& d = frameBegin();
  uint32_t s = elapsedMs / 1000;
  char el[16];
  snprintf(el, sizeof(el), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  if ((millis() / 500) % 2) d.fillCircle(10, 10, 6, TFT_RED);
  d.setTextSize(2);
  d.setTextColor(TFT_RED);
  d.setCursor(22, 3);
  d.print("REC ");
  d.setTextColor(TFT_WHITE);
  d.print(el);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);
  micIcon(d, 160, 14);
  // one tape side = 60 min; the reels turn about once per 2 s like a real deck
  float progress = (s % 3600) / 3600.0f;
  cassette(d, 4, 22, 186, 92, label, TFT_RED, -(millis() % 100000) * 0.0031f, progress);
  levelMeter(d, 210, 22, 22, 70);
  gainBadge(d, 205, 95, 32, 19);
  footer(d, String(mb, 1) + " MB" + (gaps ? "  gaps " + String(gaps) : "") + "   type = name",
         "double-click G0 = stop");
  frameEnd(d);
}

static void menuScreen(const String& header, uint16_t headerColor, const String& label, const String& info,
                       const String& keys) {
  LovyanGFX& d = frameBegin();
  d.setTextSize(2);
  d.setTextColor(headerColor);
  d.setCursor(4, 3);
  d.print(header);
  d.setTextColor(TFT_WHITE);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);
  cassette(d, 27, 22, 186, 92, label, rgb(60, 140, 220), 0, 0);
  footer(d, info, keys);
  frameEnd(d);
}

// Upload: small tape -> moving dots -> NAS box, and a bar that fills up.
static void uploadScreen(const String& name, size_t sent, size_t total, int idx, int count) {
  LovyanGFX& d = frameBegin();
  const float frac = total ? (float)sent / total : 0;
  d.setTextSize(2);
  d.setTextColor(TFT_YELLOW);
  d.setCursor(4, 3);
  d.print("UPLOAD " + String(idx) + "/" + String(count));
  d.setTextColor(TFT_WHITE);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);

  // tape
  d.fillRoundRect(8, 30, 64, 40, 5, rgb(48, 48, 56));
  d.fillRoundRect(14, 34, 52, 16, 3, rgb(250, 238, 204));
  const float ang = millis() * 0.004f;
  for (int cx : {26, 54}) {
    d.fillCircle(cx, 59, 7, rgb(236, 236, 226));
    for (int i = 0; i < 3; i++) {
      float a = ang + i * 2.094f;
      d.drawLine(cx, 59, cx + cosf(a) * 6, 59 + sinf(a) * 6, rgb(30, 30, 34));
    }
  }
  // moving dots
  const int phase = (millis() / 120) % 6;
  for (int i = 0; i < 6; i++) d.fillCircle(84 + i * 12, 50, i == phase ? 3 : 2, i == phase ? TFT_YELLOW : rgb(90, 90, 90));
  // NAS: two drive bays and a blinking activity LED
  d.fillRoundRect(160, 26, 70, 48, 5, rgb(35, 35, 40));
  d.drawRoundRect(160, 26, 70, 48, 5, rgb(120, 120, 132));
  for (int bay : {32, 50}) d.fillRoundRect(167, bay, 46, 13, 2, rgb(70, 70, 80));
  d.fillCircle(221, 38, 2, (millis() / 200) % 2 ? rgb(40, 200, 255) : rgb(20, 60, 80));
  d.fillCircle(221, 56, 2, TFT_GREEN);

  // progress bar
  d.drawRoundRect(8, 82, 224, 16, 4, rgb(150, 150, 160));
  const int w = (int)(220 * frac);
  if (w > 0) d.fillRoundRect(10, 84, w, 12, 3, frac >= 1 ? TFT_GREEN : rgb(60, 140, 220));
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString(String((int)(frac * 100)) + " %", 120, 90);
  d.setTextDatum(textdatum_t::top_left);

  String n = name;
  n.replace(".wav", "");
  footer(d, n.length() > 40 ? n.substring(0, 40) : n,
         String(sent / 1048576.0f, 1) + " / " + String(total / 1048576.0f, 1) + " MB   G0 = stop & record");
  frameEnd(d);
}

// ================================================================ SD + WAV
static bool sdBegin() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 25000000)) return false;
  if (!SD.exists(REC_DIR)) SD.mkdir(REC_DIR);
  return true;
}

static void sdEnd() {
  SD.end();
  SPI.end();
}

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, 1);  // mono
  put32(h + 24, SAMPLE_RATE);
  put32(h + 28, SAMPLE_RATE * 2);
  put16(h + 32, 2);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put32(h + 40, dataBytes);
  f.seek(0);
  f.write(h, sizeof(h));
}

// Lists finished recordings. With recover=true, files left as .wav.part by a
// power loss get a correct header and become normal recordings.
static std::vector<String> listRecordings(bool recover) {
  std::vector<String> done, parts;
  File dir = SD.open(REC_DIR);
  if (!dir) return done;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    if (!f.isDirectory()) {
      if (n.endsWith(".wav")) done.push_back(n);
      else if (n.endsWith(".wav.part")) parts.push_back(n);
    }
    f.close();
  }
  dir.close();
  if (recover) {
    for (auto& p : parts) {
      const String path = String(REC_DIR) + "/" + p;
      // "x.wav.part" -> "x_recovered.wav": the cut-off recording stays recognisable on the NAS
      const String fin = p.substring(0, p.length() - 9) + "_recovered.wav";
      const String finPath = String(REC_DIR) + "/" + fin;
      File probe = SD.open(path, FILE_READ);
      const size_t sz = probe ? probe.size() : 0;
      if (probe) probe.close();
      if (sz <= 44) {  // cut off before the first save point: nothing to keep
        SD.remove(path);
        logEvent("recover_empty", p + " " + String((unsigned long)sz) + "B");
        continue;
      }
      // Rename first: from then on it is a normal recording and will be uploaded,
      // even if fixing the header below fails (the header sizes are then only
      // those of the last save point; the pipeline's ffmpeg reads to the end anyway).
      if (!SD.rename(path, finPath)) {
        logEvent("recover_failed", p + " rename");
        continue;
      }
      File f = SD.open(finPath, "r+");
      if (f) {
        writeWavHeader(f, f.size() - 44);
        f.close();
      }
      done.push_back(fin);
      logEvent("recovered", fin + " " + String((unsigned long)(sz / 1024)) + "KB" + (f ? "" : " header-not-fixed"));
    }
  }
  return done;
}

// ================================================================ keyboard text input
enum class Input { Digits, Name, Any };

static bool accepts(Input mode, char c) {
  switch (mode) {
    case Input::Digits: return isdigit((unsigned char)c);
    case Input::Name: return isalnum((unsigned char)c) || c == '-' || c == '_';
    default: return c >= 32 && c <= 126;
  }
}

// Line editor. Enter/G0 or timeout accept; Esc (the ` key, top left) cancels and
// sets *escaped. `hint` is shown right below the input, e.g. the expected format.
static String textInput(const String& prompt, uint32_t timeoutMs, Input mode, size_t maxLen,
                        const String& initial = "", const String& hint = "", bool* escaped = nullptr) {
  String s = initial;
  uint32_t last = millis(), lastDraw = 0;
  bool dirty = true;
  if (escaped) *escaped = false;
  for (;;) {
    M5Cardputer.update();
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
      auto& ks = kb.keysState();
      last = millis();
      if (ks.enter) break;
      if (!ks.word.empty() && ks.word[0] == '`') {  // Esc
        if (escaped) *escaped = true;
        return initial;
      }
      if (ks.del && s.length()) s.remove(s.length() - 1);
      for (char c : ks.word) {
        if (c == ' ' && mode == Input::Name) c = '_';
        if (s.length() < maxLen && accepts(mode, c)) s += c;
      }
      dirty = true;
    }
    if (M5.BtnA.wasClicked()) break;
    if (millis() - last > timeoutMs) break;
    if (dirty || millis() - lastDraw > 1000) {
      uint32_t left = (timeoutMs - (millis() - last)) / 1000;
      String shown = s.length() > 25 ? s.substring(s.length() - 25) : s;  // tail of long URLs
      ui(prompt, TFT_CYAN, shown + "_", hint, "Enter=OK Esc=back Del=del", "auto in " + String(left) + "s");
      dirty = false;
      lastDraw = millis();
    }
    delay(10);
  }
  return s;
}

// Date and time are separate fields, prefilled with the current values: Enter
// keeps them, so usually only the clock time needs typing.
static void setTimeManually() {
  time_t now = time(nullptr);
  struct tm cur;
  localtime_r(&now, &cur);
  char d0[12], t0[8];
  strftime(d0, sizeof(d0), "%d%m%Y", &cur);
  strftime(t0, sizeof(t0), "%H%M", &cur);
  bool esc = false;
  String d = textInput("Date", 60000, Input::Digits, 8, timeValid() ? d0 : "", "format DDMMYYYY (17102026)", &esc);
  if (esc) return;
  String hm = textInput("Time", 60000, Input::Digits, 4, timeValid() ? t0 : "", "format HHMM (2200 = 22:00)", &esc);
  if (esc) return;
  if (d.length() != 8 || hm.length() != 4) {
    ui("TIME", TFT_RED, "need DDMMYYYY and HHMM", "unchanged");
    delay(1500);
    return;
  }
  struct tm t = {};
  t.tm_mday = d.substring(0, 2).toInt();
  t.tm_mon = d.substring(2, 4).toInt() - 1;
  t.tm_year = d.substring(4, 8).toInt() - 1900;
  t.tm_hour = hm.substring(0, 2).toInt();
  t.tm_min = hm.substring(2, 4).toInt();
  t.tm_isdst = -1;
  if (t.tm_mday < 1 || t.tm_mday > 31 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_hour > 23 || t.tm_min > 59) {
    ui("TIME", TFT_RED, "invalid date/time", "unchanged");
    delay(1500);
    return;
  }
  struct timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
  rtcTimeExact = true;
  saveClock();
  logEvent("time_set_manual");
}

// ================================================================ recording
// The mic is captured as stereo: depending on core/driver version the Cardputer's
// PDM mic shows up in the left or the right slot, the other one stays constant.
// Each buffer holds BUF_SAMPLES/2 L/R frames and is reduced to mono in place.
static constexpr size_t MONO_SAMPLES = BUF_SAMPLES / 2;
static constexpr size_t MONO_BYTES = MONO_SAMPLES * 2;
static int micChannel = -1;  // 0 = left, 1 = right, -1 = not decided yet
static float hpX = 0, hpY = 0;
static float noiseEst = 100.0f;  // tracked background-noise peak of the input
static float agcGate = 60.0f;    // input peak needed to raise the gain
static float agcPeakIn = 0;      // input peak of the latest block (AGC test screen)
static bool agcSpeech = false;   // latest block was above the gate
static int32_t recPeak = 0;  // loudest output sample of the recording (logged, for tuning agc_max)

static void toMono(int16_t* p) {
  int64_t sum[2] = {0, 0};
  for (size_t i = 0; i < MONO_SAMPLES; i++) { sum[0] += p[2 * i]; sum[1] += p[2 * i + 1]; }
  int32_t mean[2] = {(int32_t)(sum[0] / (int64_t)MONO_SAMPLES), (int32_t)(sum[1] / (int64_t)MONO_SAMPLES)};
  uint64_t act[2] = {0, 0};
  for (size_t i = 0; i < MONO_SAMPLES; i++) {
    act[0] += abs(p[2 * i] - mean[0]);
    act[1] += abs(p[2 * i + 1] - mean[1]);
  }
  int ch = act[1] > act[0] ? 1 : 0;
  if (micUsb) {  // USB mic: mix L and R (two DJI transmitters arrive as left/right)
    for (size_t i = 0; i < MONO_SAMPLES; i++) p[2 * i] = (int16_t)(((int32_t)p[2 * i] + p[2 * i + 1]) / 2);
    ch = 0;
  } else {
  if (micChannel < 0 && (act[ch] > 4 * act[1 - ch] + 200)) micChannel = ch;  // clearly one live side
  if (micChannel >= 0) ch = micChannel;
  }
  // 1) ~80 Hz high-pass: removes the PDM DC offset, rumble and handling noise.
  float peakIn = 0;
  for (size_t i = 0; i < MONO_SAMPLES; i++) {
    float x = p[2 * i + ch];
    hpY = x - hpX + 0.97f * hpY;
    hpX = x;
    p[i] = (int16_t)constrain(hpY, -32768.0f, 32767.0f);
    if (fabsf(hpY) > peakIn) peakIn = fabsf(hpY);
  }
  // 2) Automatic gain like a dictaphone: aim the block peak at ~-9 dBFS, never more
  //    than agc_max times. Loud blocks pull the gain down at once, voices raise it
  //    quickly (~8 dB/s). Background noise is tracked (falls fast, rises slowly);
  //    only blocks clearly above it (3x) may raise the gain, and the gain is capped
  //    so the noise itself never ends up louder than ~-27 dBFS.
  static constexpr float TARGET = 12000.0f, MIN_GATE = 60.0f, NOISE_OUT_MAX = 6000.0f;  // hiss may reach ~-15 dBFS so quiet speech still gets loud
  if (peakIn < noiseEst) noiseEst = max(peakIn, 5.0f);
  else noiseEst *= 1.01f;
  agcGate = max(MIN_GATE, noiseEst * 3.0f);
  const float maxGain = min((float)S.agcMax, NOISE_OUT_MAX / noiseEst);
  float want = peakIn > 1.0f ? TARGET / peakIn : maxGain;
  want = constrain(want, 1.0f, max(1.0f, maxGain));
  const float g0 = agcGain;
  agcSpeech = peakIn > agcGate;
  if (want < agcGain) agcGain = want;                               // attack: loud -> down at once
  else if (agcSpeech) agcGain = min(want, agcGain * 1.12f);         // speech: up ~8 dB/s
  else agcGain = min(want, agcGain * 1.03f);  // quiet: drift up ~2 dB/s until the hiss hits the cap,
                                              // so a click never leaves the gain stuck at x1
  agcPeakIn = peakIn;
  // 3) Apply with a ramp across the block, soft-limit above 24000.
  int32_t blockPeak = 0;
  for (size_t i = 0; i < MONO_SAMPLES; i++) {
    const float g = g0 + (agcGain - g0) * (float)i / MONO_SAMPLES;
    float y = p[i] * g;
    const float a = fabsf(y);
    if (a > 24000.0f) y = copysignf(24000.0f + (a - 24000.0f) * 0.25f, y);
    const int32_t v = (int32_t)constrain(y, -32767.0f, 32767.0f);
    if (abs(v) > blockPeak) blockPeak = abs(v);
    p[i] = (int16_t)v;
  }
  levelPeak = blockPeak;
  if (blockPeak > recPeak) recPeak = blockPeak;
}

// ---- PDM microphone via the ESP-IDF driver (M5Unified's mic path stays silent
// on the Cardputer with this core). A task reads DMA data into free buffers.
static i2s_chan_handle_t micRx = nullptr;
static volatile TaskHandle_t micTask = nullptr;
static volatile bool micRun = false;
static volatile uint32_t micDrops = 0;
static int16_t dropBuf[BUF_SAMPLES];  // read target while every buffer waits for the SD card

static void micTaskFn(void*) {
  while (micRun) {
    int16_t* b = nullptr;
    const bool have = xQueueReceive(freeQ, &b, 0) == pdTRUE;
    size_t got = 0;
    i2s_channel_read(micRx, have ? b : dropBuf, BUF_SAMPLES * 2, &got, pdMS_TO_TICKS(500));
    if (!have) { micDrops++; continue; }
    if (got == BUF_SAMPLES * 2) xQueueSend(filledQ, &b, 0);
    else xQueueSend(freeQ, &b, 0);
  }
  micTask = nullptr;
  vTaskDelete(nullptr);
}

static bool micStart() {
  // Known Cardputer issue: stale pin state on 43/46 leaves the mic silent. Start clean.
  gpio_hold_dis(GPIO_NUM_43);
  gpio_reset_pin(GPIO_NUM_43);
  gpio_reset_pin(GPIO_NUM_46);
  delay(5);
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 8;
  cc.dma_frame_num = 256;
  if (i2s_new_channel(&cc, nullptr, &micRx) != ESP_OK) return false;
  i2s_pdm_rx_config_t pc;
  memset(&pc, 0, sizeof(pc));
  pc.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  pc.slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  pc.gpio_cfg.clk = GPIO_NUM_43;
  pc.gpio_cfg.din = GPIO_NUM_46;
  if (i2s_channel_init_pdm_rx_mode(micRx, &pc) != ESP_OK || i2s_channel_enable(micRx) != ESP_OK) {
    i2s_del_channel(micRx);
    micRx = nullptr;
    return false;
  }
  micDrops = 0;
  micRun = true;
  TaskHandle_t h;
  xTaskCreatePinnedToCore(micTaskFn, "mic", 4096, nullptr, 5, &h, 0);
  micTask = h;
  return true;
}

static void micStop();

// Recording and AGC test: the USB microphone if one answers, otherwise the built-in mic.
// Only probed without a PC on the port (USB serial and flashing need it), ~1 s when nothing
// is plugged in. USB host and resampling run at 160 MHz.
static bool micStartAny() {
  micUsb = false;
  if (S.usbMic && !HWCDC::isPlugged()) {
    setCpuFrequencyMhz(160);
    if (usbMicStart(freeQ, filledQ, BUF_SAMPLES, 1200)) {
      micUsb = true;
      agcGain = 1.0f;  // the receiver already delivers a full-level signal
      return true;
    }
    setCpuFrequencyMhz(80);
  }
  return micStart();
}

static String micSource() {
  if (!micUsb) return "internal";
  const UsbMicInfo& m = usbMicInfo();
  char b[96];
  snprintf(b, sizeof(b), "usb %04x:%04x %s %luHz %uch %ubit", m.vid, m.pid, m.product, (unsigned long)m.rate,
           m.channels, m.bits);
  return b;
}

struct CardFile {
  String name;
  uint32_t bytes;
};

// Everything in /rec (finished, still to upload, and unfinished .part files).
static std::vector<CardFile> cardFiles() {
  std::vector<CardFile> out;
  File dir = SD.open(REC_DIR);
  if (!dir) return out;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) out.push_back({f.name(), (uint32_t)f.size()});
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const CardFile& a, const CardFile& b) { return a.name < b.name; });
  return out;
}

static String durationOf(uint32_t bytes) {
  uint32_t s = bytes > 44 ? (bytes - 44) / (SAMPLE_RATE * 2) : 0;
  char b[12];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return b;
}

// USB serial 'w': checks that a file can be reopened with "r+" and patched at offset 0
// while keeping its length (what the recording save points rely on).
static void serialSdTest() {
  const char* p = "/rec/_sdtest.tmp";
  File f = SD.open(p, FILE_WRITE);
  if (!f) { Serial.println("sdtest: create FAILED"); return; }
  for (int i = 0; i < 1000; i++) f.write((uint8_t)'a');
  f.close();
  f = SD.open(p, "r+");
  if (!f) { Serial.println("sdtest: reopen r+ FAILED"); SD.remove(p); return; }
  f.seek(0);
  f.write((const uint8_t*)"HEAD", 4);
  f.seek(1000);
  f.write((const uint8_t*)"TAIL", 4);
  f.close();
  f = SD.open(p, FILE_READ);
  char head[5] = {0}, tail[5] = {0};
  f.read((uint8_t*)head, 4);
  f.seek(1000);
  f.read((uint8_t*)tail, 4);
  size_t sz = f.size();
  f.close();
  SD.remove(p);
  Serial.printf("sdtest: size %u (want 1004) head %s tail %s -> %s\n", (unsigned)sz, head, tail,
                (sz == 1004 && !strcmp(head, "HEAD") && !strcmp(tail, "TAIL")) ? "OK" : "FAILED");
}

// USB serial 'l': list the card for diagnostics.
static void serialListCard() {
  auto files = cardFiles();
  Serial.printf("%u file(s) in %s (all not yet uploaded):\n", (unsigned)files.size(), REC_DIR);
  for (auto& f : files) Serial.printf("  %-48s %9lu B  %s\n", f.name.c_str(), (unsigned long)f.bytes, durationOf(f.bytes).c_str());
}

// Menu 'l': which recordings are still on the card, i.e. not uploaded yet.
// ; and . scroll (the arrow keys), Esc/Enter/G0 back.
static void listScreen() {
  auto files = cardFiles();
  size_t top = 0;
  uint32_t last = millis();
  bool dirty = true;
  for (;;) {
    if (dirty) {
      String l[4];
      for (int i = 0; i < 4; i++) {
        size_t k = top + i;
        if (k < files.size()) {
          String n = files[k].name;
          n.replace(".wav", "");
          if (n.length() > 19) n = n.substring(0, 19);
          l[i] = n + " " + durationOf(files[k].bytes);
        }
      }
      if (files.empty()) l[0] = "all uploaded";
      ui(String(files.size()) + " on card", files.empty() ? TFT_GREEN : TFT_YELLOW, l[0], l[1], l[2], l[3]);
      dirty = false;
    }
    M5Cardputer.update();
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
      auto& ks = kb.keysState();
      last = millis();
      char c = ks.word.empty() ? 0 : ks.word[0];
      if (ks.enter || c == '`') return;
      if (c == ';' && top > 0) { top--; dirty = true; }
      if (c == '.' && top + 4 < files.size()) { top++; dirty = true; }
    }
    if (M5.BtnA.wasClicked() || millis() - last > 30000) return;
    delay(10);
  }
}

// Two short beeps. The speaker amp shares GPIO43 with the mic clock, so the mic is
// paused for the beep (DMA buffers keep the WAV continuous apart from ~0.3 s).
static void lowBattBeep() {
  const bool wasRunning = micRx != nullptr;
  if (wasRunning) micStop();
  M5.Speaker.begin();
  M5.Speaker.setVolume(160);
  for (int i = 0; i < 2; i++) {
    M5.Speaker.tone(2200, 90);
    delay(150);
  }
  M5.Speaker.end();
  if (wasRunning) micStart();
}

// USB serial self-test (send 'm'): 2 s of raw PDM data, per-slot statistics, plus
// how often the data pin toggles. A dead mic shows constant values / 0 toggles.
static void micSelfTest() {
  xQueueReset(filledQ);
  xQueueReset(freeQ);
  for (int i = 0; i < NBUF; i++) {
    int16_t* p = bufs[i];
    xQueueSend(freeQ, &p, 0);
  }
  const bool ok = micStart();
  Serial.printf("mic start: %s\n", ok ? "ok" : "FAILED");
  if (!ok) return;
  uint32_t toggles = 0;
  int last = gpio_get_level(GPIO_NUM_46);
  for (int i = 0; i < 20000; i++) {
    int v = gpio_get_level(GPIO_NUM_46);
    if (v != last) toggles++;
    last = v;
  }
  Serial.printf("GPIO46 toggles in 20000 reads: %lu\n", (unsigned long)toggles);
  uint32_t t0 = millis();
  int n = 0;
  while (millis() - t0 < 2000) {
    int16_t* p;
    if (xQueueReceive(filledQ, &p, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    int16_t mn[2] = {32767, 32767}, mx[2] = {-32768, -32768};
    for (size_t i = 0; i < BUF_SAMPLES; i++) {
      int c = i & 1;
      if (p[i] < mn[c]) mn[c] = p[i];
      if (p[i] > mx[c]) mx[c] = p[i];
    }
    if (n++ % 2 == 0)
      Serial.printf("blk %2d  L %6d..%6d   R %6d..%6d   first %d %d %d %d\n", n, mn[0], mx[0], mn[1], mx[1], p[0], p[1],
                    p[2], p[3]);
    xQueueSend(freeQ, &p, 0);
  }
  Serial.printf("blocks %d, drops %lu\n", n, (unsigned long)micDrops);
  micStop();
}

static void micStop();

// Horizontal level bar, -60..0 dBFS, optional marker (e.g. the noise gate).
static void dbBar(LovyanGFX& d, int x, int y, int w, int h, float level, uint16_t col, float marker = -1) {
  auto pos = [&](float v) {
    float db = v > 0 ? 20.0f * log10f(v / 32768.0f) : -96.0f;
    return (int)(constrain((db + 60.0f) / 60.0f, 0.0f, 1.0f) * w);
  };
  d.fillRoundRect(x, y, w, h, 3, rgb(40, 40, 46));
  const int p = pos(level);
  if (p > 0) d.fillRoundRect(x, y, p, h, 3, col);
  if (marker > 0) d.fillRect(x + pos(marker) - 1, y - 2, 3, h + 4, TFT_ORANGE);
}

// Menu 'a': live view of the automatic gain, nothing is recorded. Shows the gain,
// the raw microphone level (orange mark = noise gate), the level after the AGC and
// a 12 s history (green = output level, yellow = gain). Any key or G0 = back.
static void agcTest() {
  xQueueReset(filledQ);
  xQueueReset(freeQ);
  for (int i = 0; i < NBUF; i++) {
    int16_t* p = bufs[i];
    xQueueSend(freeQ, &p, 0);
  }
  micChannel = -1;
  hpX = hpY = 0;
  agcGain = 4.0f;
  noiseEst = 100.0f;
  if (!micStartAny()) {
    ui("MIC ERROR", TFT_RED, "could not start mic");
    delay(2000);
    return;
  }
  screenOn();
  static constexpr int H = 96;
  uint8_t histOut[H] = {0}, histGain[H] = {0};
  int head = 0, skip = 1;
  uint32_t lastDraw = 0;
  for (;;) {
    int16_t* p;
    if (xQueueReceive(filledQ, &p, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (skip > 0) skip--;  // power-up click
      else {
        toMono(p);
        float db = levelPeak > 0 ? 20.0f * log10f(levelPeak / 32768.0f) : -60.0f;
        histOut[head] = (uint8_t)constrain((db + 60.0f) / 60.0f * 30.0f, 0.0f, 30.0f);
        histGain[head] = (uint8_t)constrain(log2f(agcGain) / 6.0f * 30.0f, 0.0f, 30.0f);  // x1..x64
        head = (head + 1) % H;
      }
      xQueueSend(freeQ, &p, 0);
    }
    M5Cardputer.update();
    const bool keyDown = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
    if (keyDown || M5.BtnA.wasClicked()) break;
    if (millis() - lastDraw < 100) continue;
    lastDraw = millis();

    LovyanGFX& d = frameBegin();
    d.setTextSize(2);
    d.setTextColor(TFT_CYAN);
    d.setCursor(4, 3);
    d.print("AGC TEST");
    d.setTextColor(TFT_WHITE);
    d.setCursor(186, 3);
    d.print(String(battPct()) + "%");
    micIcon(d, 170, 14);
    d.setTextSize(4);
    d.setTextColor(TFT_YELLOW);
    d.setCursor(4, 24);
    d.print("x" + String(agcGain, agcGain < 10 ? 1 : 0));
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE);
    d.setCursor(124, 26);
    d.print(micUsb ? "IN  (USB mic)" : "IN  (mic)");
    dbBar(d, 124, 36, 112, 8, agcPeakIn, rgb(160, 160, 170), agcGate);
    d.setCursor(124, 48);
    d.print("OUT (file)");
    dbBar(d, 124, 58, 112, 8, levelPeak, TFT_GREEN);
    // history graph: newest on the right
    const int gx = 4, gy = 72, gw = H * 2 + 40, gh = 32;
    d.drawRect(gx, gy, gw, gh, rgb(70, 70, 80));
    for (int i = 0; i < H; i++) {
      const int k = (head + i) % H;
      const int x = gx + 1 + i * (gw - 2) / H;
      d.drawFastVLine(x, gy + gh - 1 - histOut[k], histOut[k], TFT_GREEN);
      d.drawPixel(x, gy + gh - 1 - histGain[k], TFT_YELLOW);
      d.drawPixel(x + 1, gy + gh - 1 - histGain[k], TFT_YELLOW);
    }
    footer(d, agcSpeech ? "speech: gain follows voice" : "quiet: gain drifts up (cap = hiss)",
           "noise " + String((int)noiseEst) + "  gate " + String((int)agcGate) + "   any key = back");
    frameEnd(d);
  }
  micStop();
}

static void micStop() {
  if (micUsb) {
    usbMicStop();
    micUsb = false;
    setCpuFrequencyMhz(80);
    return;
  }
  if (!micRx) return;
  micRun = false;
  while (micTask) vTaskDelay(pdMS_TO_TICKS(5));
  i2s_channel_disable(micRx);
  i2s_del_channel(micRx);
  micRx = nullptr;
}

struct RecResult {
  bool ok = false;
  bool lowBatt = false;
};

static RecResult recordSession() {
  RecResult r;
  setCpuFrequencyMhz(80);
  screenOn();
  if (!sdOk) {
    ui("NO SD", TFT_RED, "insert microSD card", "(FAT32)");
    delay(3000);
    return r;
  }
  if (battMv() > 1000 && battMv() < LOW_BATT_MV) {
    ui("BATTERY", TFT_RED, "too low to record", String(battMv()) + " mV");
    delay(3000);
    r.lowBatt = true;
    return r;
  }

  char base[48];
  if (timeValid()) strncpy(base, timeStr("%Y-%m-%d_%H-%M-%S").c_str(), sizeof(base));
  else snprintf(base, sizeof(base), "notime_%lu_%lu", (unsigned long)rtcBoots, (unsigned long)millis());
  String partPath = String(REC_DIR) + "/" + base + ".wav.part";

  File f = SD.open(partPath, FILE_WRITE);
  if (!f) {
    ui("SD ERROR", TFT_RED, "cannot create file");
    delay(3000);
    return r;
  }
  writeWavHeader(f, 0);
  uint32_t dataBytes = 0;

  xQueueReset(filledQ);
  xQueueReset(freeQ);
  for (int i = 0; i < NBUF; i++) {
    int16_t* p = bufs[i];
    xQueueSend(freeQ, &p, 0);
  }
  micChannel = -1;
  hpX = hpY = 0;
  agcGain = 4.0f;
  noiseEst = 100.0f;
  recPeak = 0;
  if (!micStartAny()) {
    f.close();
    SD.remove(partPath);
    ui("MIC ERROR", TFT_RED, "could not start mic");
    delay(3000);
    return r;
  }
  logEvent("rec_start", String(base) + " " + micSource());

  const uint32_t t0 = millis();
  uint32_t lastHdr = t0, lastBatt = t0, lastBlink = 0, lastDraw = 0, screenUntil = t0 + 2500;
  uint32_t gaps = 0, buffersSeen = 0, lastWarn = 0, lostDrops = 0;
  bool writeErr = false, ledOn = false;
  String recName;
  const uint32_t ignoreBtnUntil = t0 + 800;  // the press that woke us must not count as a click

  for (;;) {
    int16_t* p;
    while (xQueueReceive(filledQ, &p, 0) == pdTRUE) {
      if (buffersSeen++ > 0) {  // the first 125 ms carry the mic's power-up click
        toMono(p);
        size_t w = f.write((const uint8_t*)p, MONO_BYTES);
        dataBytes += w;
        if (w != MONO_BYTES) writeErr = true;
      }
      xQueueSend(freeQ, &p, 0);
    }
    if (micUsb && usbMicLost()) {  // receiver unplugged or its battery died: carry on with the built-in mic
      lostDrops += usbMicDrops();
      micStop();
      logEvent("usb_mic_lost", base);
      micChannel = -1;
      agcGain = 4.0f;
      if (!micStart()) break;
      screenOn();
      screenUntil = millis() + 3000;
    }
    gaps = lostDrops + (micUsb ? usbMicDrops() : micDrops);  // 250 ms blocks lost while the SD card stalled

    M5Cardputer.update();
    const uint32_t now = millis();
    while (Serial.available()) {  // a flash request must not cut the recording
      if (Serial.read() == 'F') Serial.println("BUSY-RECORDING");
    }
    if (M5.BtnA.wasDoubleClicked() && now > ignoreBtnUntil) break;
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      // Typing while recording sets the file name; the mic keeps running meanwhile.
      // With the screen off, the first key only wakes the screen.
      auto& ks = M5Cardputer.Keyboard.keysState();
      if (screenIsOn) {
        if (ks.del && recName.length()) recName.remove(recName.length() - 1);
        for (char c : ks.word) {
          if (c == ' ') c = '_';
          if (recName.length() < 40 && accepts(Input::Name, c)) recName += c;
        }
      }
      screenOn();
      screenUntil = now + 5000;
      lastDraw = 0;
    }

    if (screenIsOn) {
      if (now > screenUntil) {
        screenOff();
      } else if (now - lastDraw > 120) {  // ~8 fps: enough for turning reels
        recScreen(now - t0, recName.length() ? recName + "_" : String(base), dataBytes / 1048576.0f, gaps);
        lastDraw = now;
      }
    }

    // Save point 2 s after the start, then every 10 s, so a reset or empty battery
    // loses at most the last 10 s. flush() alone does not update the FAT directory
    // entry (the file length stays 44 bytes until close), so the file is closed and
    // reopened; the mic keeps filling its buffers meanwhile.
    if (now - lastHdr > (dataBytes < 100000 ? 2000u : 10000u)) {
      writeWavHeader(f, dataBytes);
      f.close();
      f = SD.open(partPath, "r+");
      if (!f) f = SD.open(partPath, FILE_APPEND);
      if (!f) writeErr = true;
      else f.seek(44 + dataBytes);
      lastHdr = now;
    }
    if (now - lastBlink > 5000) {  // short dim red blink = "still recording"
      M5.Led.setAllColor(24, 0, 0);
      ledOn = true;
      lastBlink = now;
    } else if (ledOn && now - lastBlink > 40) {
      M5.Led.setAllColor(0, 0, 0);
      ledOn = false;
    }
    if (now - lastBatt > 10000) {
      int mv = battMv();
      if (mv > 1000 && mv < LOW_BATT_MV) {  // close the file cleanly well before the cell is empty
        r.lowBatt = true;
        break;
      }
      if (mv > 1000 && mv < LOW_BATT_WARN_MV && now - lastWarn > 60000) {
        lowBattBeep();  // ~0.3 s gap in the recording: speaker and mic share GPIO43
        screenOn();
        screenUntil = millis() + 3000;
        lastWarn = now;
      }
      lastBatt = now;
    }
    if (writeErr) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  const char* srcAtStop = micUsb ? "usb" : "internal";
  micStop();
  M5.Led.setAllColor(0, 0, 0);
  int16_t* p;
  while (xQueueReceive(filledQ, &p, 0) == pdTRUE) {
    toMono(p);
    dataBytes += f.write((const uint8_t*)p, MONO_BYTES);
  }
  writeWavHeader(f, dataBytes);
  f.close();

  const uint32_t secs = (millis() - t0) / 1000;
  screenOn();
  if (millis() - t0 < 1000 && !r.lowBatt && !writeErr) {  // accidental tap only; 1 s+ is always kept
    SD.remove(partPath);
    logEvent("rec_discarded", base);
    ui("TOO SHORT", TFT_ORANGE, "under 1 s: not saved");
    delay(1500);
    return r;
  }
  String name;
  if (!r.lowBatt && !writeErr) name = textInput("Name?", NAME_TIMEOUT_S * 1000UL, Input::Name, 40, recName);
  else name = recName;
  String finalName = String(base) + (name.length() ? "_" + name : "") + ".wav";
  if (!SD.rename(partPath, String(REC_DIR) + "/" + finalName)) finalName = String(base) + ".wav.part";
  logEvent(writeErr ? "rec_stop_sd_full" : (r.lowBatt ? "rec_stop_lowbatt" : "rec_stop"),
           finalName + " " + String(secs) + "s gaps=" + String(gaps) + " mic_ch=" + String(micChannel) + " src=" + String(srcAtStop) + " peak=" + String(recPeak));

  rtcPending = listRecordings(false).size();
  ui(writeErr ? "SD FULL" : (r.lowBatt ? "LOW BATT" : "SAVED"), writeErr || r.lowBatt ? TFT_RED : TFT_GREEN,
     finalName.substring(0, 19), finalName.length() > 19 ? finalName.substring(19, 38) : "",
     String(secs / 60) + " min " + String(secs % 60) + " s");
  delay(1500);
  r.ok = true;
  return r;
}

// ================================================================ WebDAV upload
struct DavTarget {
  bool tls = false;
  String host;
  uint16_t port = 80;
  String path;  // without trailing slash
  String auth;
} dav;

static void parseDavUrl() {
  String u = S.davUrl;
  dav.tls = u.startsWith("https://");
  u = u.substring(u.indexOf("://") + 3);
  int slash = u.indexOf('/');
  String hostport = slash < 0 ? u : u.substring(0, slash);
  dav.path = slash < 0 ? "" : u.substring(slash);
  while (dav.path.endsWith("/")) dav.path.remove(dav.path.length() - 1);
  int colon = hostport.indexOf(':');
  dav.host = colon < 0 ? hostport : hostport.substring(0, colon);
  dav.port = colon < 0 ? (dav.tls ? 443 : 80) : hostport.substring(colon + 1).toInt();
  dav.auth = "Basic " + base64::encode(S.davUser + ":" + S.davPass);
}

static String urlEncodePath(const String& p) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < p.length(); i++) {
    char c = p[i];
    if (isalnum((unsigned char)c) || strchr("-_.~/", c)) o += c;
    else { o += '%'; o += hex[(uint8_t)c >> 4]; o += hex[(uint8_t)c & 15]; }
  }
  return o;
}

static String absUrl(const String& path) {
  return String(dav.tls ? "https://" : "http://") + dav.host + ":" + dav.port + urlEncodePath(path);
}

static String readLine(NetworkClient& c, uint32_t timeoutMs) {
  String s;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') return s;
      if (ch != '\r') s += ch;
    }
    if (!c.connected() && !c.available()) break;
    delay(2);
  }
  return s;
}

using Progress = void (*)(size_t sent, size_t total);
static volatile bool uploadAbort = false;  // G0 during an upload: stop it, the user wants to record

// One WebDAV request on a fresh connection. Returns the HTTP status or -1.
static int davRequest(const char* method, const String& path, const String& extraHeaders, File* body,
                      int64_t* contentLength = nullptr, String* respBody = nullptr, Progress prog = nullptr) {
  NetworkClient plain;
  NetworkClientSecure secure;
  NetworkClient& c = dav.tls ? (NetworkClient&)secure : plain;
  if (dav.tls) secure.setInsecure();  // Synology default certs are self-signed
  if (!c.connect(dav.host.c_str(), dav.port, 8000)) return -1;

  size_t len = body ? body->size() : 0;
  String req = String(method) + " " + urlEncodePath(path) + " HTTP/1.1\r\n" + "Host: " + dav.host + ":" + dav.port +
               "\r\n" + "Authorization: " + dav.auth + "\r\n" + "User-Agent: gencord\r\n" +
               "Connection: close\r\n" + "Content-Length: " + String((unsigned long)len) + "\r\n" + extraHeaders +
               "\r\n";
  c.print(req);

  if (body) {
    static constexpr size_t CHUNK = 8192;
    uint8_t* buf = (uint8_t*)malloc(CHUNK);
    if (!buf) return -1;
    body->seek(0);
    size_t sent = 0;
    bool fail = false;
    while (sent < len && !fail) {
      int n = body->read(buf, min(CHUNK, len - sent));
      if (n <= 0) { fail = true; break; }
      size_t off = 0;
      uint32_t tw = millis();
      while (off < (size_t)n) {
        size_t w = c.write(buf + off, n - off);
        if (w > 0) { off += w; tw = millis(); }
        else if (!c.connected() || millis() - tw > 20000) { fail = true; break; }
        else delay(2);
      }
      sent += off;
      if (prog) prog(sent, len);
      if (uploadAbort) fail = true;
    }
    free(buf);
    if (fail) { c.stop(); return -1; }
  }

  String status = readLine(c, 30000);  // the NAS may need a moment after a big upload
  int sp = status.indexOf(' ');
  int code = (status.startsWith("HTTP/") && sp > 0) ? status.substring(sp + 1).toInt() : -1;
  int64_t clen = -1;
  for (;;) {
    String h = readLine(c, 5000);
    if (h.length() == 0) break;
    String hl = h;
    hl.toLowerCase();
    if (hl.startsWith("content-length:")) clen = atoll(h.c_str() + 15);
  }
  if (contentLength) *contentLength = clen;
  if (respBody && strcmp(method, "HEAD") != 0) {
    uint32_t t0 = millis();
    while ((c.connected() || c.available()) && millis() - t0 < 5000 && respBody->length() < 8192) {
      while (c.available() && respBody->length() < 8192) *respBody += (char)c.read();
      delay(2);
    }
  }
  c.stop();
  return code;
}

// Size of a remote file: HEAD first, PROPFIND as fallback. -1 if unknown.
static int64_t remoteSize(const String& path) {
  int64_t len = -1;
  if (davRequest("HEAD", path, "", nullptr, &len) == 200 && len >= 0) return len;
  String body;
  int code = davRequest("PROPFIND", path, "Depth: 0\r\n", nullptr, nullptr, &body);
  if (code != 207) return -1;
  int i = body.indexOf("getcontentlength");
  if (i < 0) return -1;
  i = body.indexOf('>', i);
  if (i < 0) return -1;
  return atoll(body.c_str() + i + 1);
}

static String upName;
static int upIdx = 0, upCount = 0;
static uint32_t upLastDraw = 0;
static void upProgress(size_t sent, size_t total) {
  M5.update();
  if (M5.BtnA.wasPressed()) uploadAbort = true;
  if (!screenIsOn || (millis() - upLastDraw < 150 && sent < total)) return;
  upLastDraw = millis();
  uploadScreen(upName, sent, total, upIdx, upCount);
}

// Upload -> verify size -> rename -> verify again. Only then may the caller delete the local file.
static bool uploadOne(const String& name, String& err) {
  String local = String(REC_DIR) + "/" + name;
  File f = SD.open(local, FILE_READ);
  if (!f) { err = "open"; return false; }
  const int64_t size = f.size();
  const String remote = dav.path + "/" + name;
  const String tmp = remote + ".part";
  upName = name;
  if (screenIsOn) uploadScreen(name, 0, size, upIdx, upCount);
  int code = davRequest("PUT", tmp, "Content-Type: audio/wav\r\n", &f, nullptr, nullptr, upProgress);
  f.close();
  if (code != 200 && code != 201 && code != 204) { err = "PUT " + String(code); return false; }
  if (remoteSize(tmp) != size) { err = "verify"; return false; }
  code = davRequest("MOVE", tmp, "Destination: " + absUrl(remote) + "\r\nOverwrite: T\r\n", nullptr);
  if (code != 201 && code != 204) { err = "MOVE " + String(code); return false; }
  if (remoteSize(remote) != size) { err = "verify2"; return false; }
  return true;
}

static void timeSynced() {
  rtcTimeExact = true;
  saveClock();
  logEvent("ntp_ok");
}

// sntp_get_sync_status() never reports COMPLETED on this core even though the clock
// gets set, so the time server's answer is caught with the notification callback.
static volatile bool ntpAnswered = false;
static void onNtpSync(struct timeval*) { ntpAnswered = true; }

static void startNtp() {
  ntpAnswered = false;
  configTzTime(S.tz.c_str(), S.ntp.c_str(), "pool.ntp.org");
  sntp_set_time_sync_notification_cb(onNtpSync);
}

static void syncTime() {
  startNtp();
  uint32_t t0 = millis();
  while (!ntpAnswered && millis() - t0 < 10000) delay(50);
  if (ntpAnswered) timeSynced();
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Cheap presence check: scan only the last known channel (~0.2 s); every 4th
// miss scan all channels in case the router changed channel.
static uint8_t scanChannel() { return (rtcWifiCh == 0 || rtcScanMiss % 4 == 3) ? 0 : rtcWifiCh; }

// Joins the home network found by the last scan (n = scan result count).
static bool wifiJoin(int n) {
  if (n <= 0) {
    rtcScanMiss++;
    WiFi.scanDelete();
    wifiOff();
    return false;
  }
  rtcScanMiss = 0;
  rtcWifiCh = WiFi.channel(0);
  WiFi.BSSID((uint8_t)0, rtcWifiBssid);
  WiFi.scanDelete();
  WiFi.begin(S.ssid.c_str(), S.pass.c_str(), rtcWifiCh, rtcWifiBssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(50);
  if (WiFi.status() != WL_CONNECTED) {
    rtcWifiCh = 0;  // force a full scan next time
    wifiOff();
    return false;
  }
  WiFi.setSleep(false);
  return true;
}

static bool wifiHome(bool fullScan) {
  WiFi.mode(WIFI_STA);
  return wifiJoin(WiFi.scanNetworks(false, false, false, 150, fullScan ? 0 : scanChannel(), S.ssid.c_str()));
}

// ---- background network job in the menu: scan -> join -> NTP -> upload pending.
// Every step is polled, so G0 starts a recording at any moment (it cancels the job).
// Away from home only the ~0.2 s scan happens, then the radio is switched off.
enum class Bg { Idle, Scanning, Joining, Ntp };
static Bg bg = Bg::Idle;
static uint32_t bgSince = 0;
static String netStatus;

static bool configured();
static void uploadConnected(bool interactive, bool needTime);
static bool bgBusy() { return bg != Bg::Idle; }

static void startBackgroundCheck() {
  if (!configured() || bg != Bg::Idle) return;
  if (rtcPending == 0 && rtcTimeExact) return;  // nothing to upload, clock is fine
  const int mv = battMv();
  if (mv > 1000 && mv < LOW_BATT_WARN_MV) return;  // power saving: the radio stays off
  WiFi.mode(WIFI_STA);
  if (WiFi.scanNetworks(true, false, false, 150, scanChannel(), S.ssid.c_str()) != WIFI_SCAN_RUNNING) {
    wifiOff();
    return;
  }
  bg = Bg::Scanning;
  bgSince = millis();
  netStatus = "looking for home Wi-Fi...";
}

static void cancelBackgroundCheck() {
  if (bg == Bg::Idle) return;
  if (bg == Bg::Scanning) WiFi.scanDelete();
  wifiOff();
  bg = Bg::Idle;
  netStatus = "";
  setCpuFrequencyMhz(80);
}

static void pollBackgroundCheck() {
  switch (bg) {
    case Bg::Idle:
      return;
    case Bg::Scanning: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;
      if (n <= 0) {
        rtcScanMiss++;
        WiFi.scanDelete();
        wifiOff();
        bg = Bg::Idle;
        netStatus = "no home Wi-Fi - Wi-Fi off";
        return;
      }
      rtcScanMiss = 0;
      rtcWifiCh = WiFi.channel(0);
      WiFi.BSSID((uint8_t)0, rtcWifiBssid);
      WiFi.scanDelete();
      setCpuFrequencyMhz(160);
      WiFi.begin(S.ssid.c_str(), S.pass.c_str(), rtcWifiCh, rtcWifiBssid);
      bg = Bg::Joining;
      bgSince = millis();
      netStatus = "connecting to " + S.ssid;
      return;
    }
    case Bg::Joining:
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        startNtp();
        bg = Bg::Ntp;
        bgSince = millis();
        netStatus = "getting time...";
      } else if (millis() - bgSince > 10000) {
        rtcWifiCh = 0;  // full scan next time
        cancelBackgroundCheck();
        netStatus = "Wi-Fi connect failed";
      }
      return;
    case Bg::Ntp:
      if (ntpAnswered || millis() - bgSince > 10000) {
        if (ntpAnswered) timeSynced();
        bg = Bg::Idle;
        netStatus = "";
        if (sdOk && rtcPending) uploadConnected(true, false);  // G0 aborts it (uploadAbort)
        else wifiOff();
        setCpuFrequencyMhz(80);
      }
      return;
  }
}

static void uploadAll(bool interactive) {
  if (!sdOk) return;
  if (!configured()) {
    if (interactive) {
      ui("NOT SET UP", TFT_ORANGE, "press c to enter", "Wi-Fi + Synology");
      delay(2000);
    }
    return;
  }
  rtcPending = listRecordings(false).size();
  if (rtcPending == 0 && rtcTimeExact) {  // nothing to send and the clock is fine: stay offline
    if (interactive) {
      ui("UPLOAD", TFT_GREEN, "nothing to upload", "all on the Synology");
      delay(1500);
    }
    return;
  }
  if (interactive) ui("WI-FI", TFT_YELLOW, "looking for", S.ssid);
  setCpuFrequencyMhz(160);
  if (!wifiHome(interactive)) {  // the user pressed 'u': scan all channels
    setCpuFrequencyMhz(80);
    if (interactive) {
      ui("WI-FI", TFT_DARKGREY, "home network", "not in range", String(rtcPending) + " pending");
      delay(1500);
    }
    return;
  }
  uploadConnected(interactive, true);
  setCpuFrequencyMhz(80);
}

static void uploadConnected(bool interactive, bool needTime) {
  std::vector<String> files = listRecordings(false);
  uploadAbort = false;
  if (needTime) syncTime();
  davRequest("MKCOL", dav.path, "", nullptr);  // 201 created / 405 exists: both fine

  int done = 0, failed = 0;
  String err;
  upCount = files.size();
  upIdx = 0;
  for (auto& name : files) {
    int mv = battMv();
    if (mv > 1000 && mv < LOW_BATT_MV + 100) { err = "low battery"; failed++; break; }
    upIdx++;
    upLastDraw = 0;
    if (uploadOne(name, err)) {
      SD.remove(String(REC_DIR) + "/" + name);
      logEvent("upload_ok", name);
      done++;
    } else if (uploadAbort) {
      logEvent("upload_aborted", name);  // stays on the card, next sync retries
      break;
    } else {
      logEvent("upload_fail", name + " " + err);
      failed++;
      break;  // most likely a network problem: try again next time
    }
    if (uploadAbort) break;
  }
  // Device log next to the recordings (overwritten each sync, never deleted locally).
  File lf = uploadAbort ? File() : SD.open(LOG_PATH, FILE_READ);
  if (lf) {
    davRequest("PUT", dav.path + "/_gencord_log.csv", "Content-Type: text/csv\r\n", &lf);
    lf.close();
  }
  wifiOff();
  setCpuFrequencyMhz(80);

  rtcPending = listRecordings(false).size();
  if (uploadAbort) return;  // the caller starts the recording
  rtcLastSync = time(nullptr);
  rtcLastSyncOk = failed == 0 ? 1 : 0;
  if (interactive) {
    ui(failed ? "UPLOAD ERR" : "UPLOADED", failed ? TFT_RED : TFT_GREEN, String(done) + " uploaded",
       failed ? err : "", String(rtcPending) + " pending");
    delay(2000);
  }
}

// ================================================================ settings file
static void loadSettings() {
  if (!sdOk) return;
  File f = SD.open(CFG_PATH, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (line.length() == 0 || line[0] == '#' || eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    k.trim();
    v.trim();
    if (v.length() == 0) continue;  // empty entries keep the built-in default
#ifdef CFG_MIGRATE_DAV_URLS
    if (k == "dav_url") {  // outdated URLs from an earlier build keep the built-in default
      bool outdated = false;
      for (const char* old : {CFG_MIGRATE_DAV_URLS}) outdated |= v == old;
      if (outdated) continue;
    }
#endif
    if (k == "wifi_ssid") S.ssid = v;
    else if (k == "wifi_pass") S.pass = v;
    else if (k == "dav_url") S.davUrl = v;
    else if (k == "dav_user") S.davUser = v;
    else if (k == "dav_pass") S.davPass = v;
    else if (k == "tz") S.tz = v;
    else if (k == "ntp") S.ntp = v;
    else if (k == "agc_max") S.agcMax = constrain(v.toInt(), 1, 255);  // old "mic_gain" is ignored on purpose
    else if (k == "wifi_check_min") S.wifiCheckMin = constrain(v.toInt(), 1, 1440);
    else if (k == "usb_mic") S.usbMic = v.toInt() != 0;
  }
  f.close();
}

static bool saveSettings() {
  if (!sdOk) return false;
  File f = SD.open(CFG_PATH, FILE_WRITE);
  if (!f) return false;
  f.println("# gencord settings (also editable on the device: menu key 'c')");
  f.println("wifi_ssid=" + S.ssid);
  f.println("wifi_pass=" + S.pass);
  f.println("# Synology: http://<nas-ip>:5005/<SharedFolder>/<subfolder>  (https -> port 5006)");
  f.println("dav_url=" + S.davUrl);
  f.println("dav_user=" + S.davUser);
  f.println("dav_pass=" + S.davPass);
  f.println("tz=" + S.tz);
  f.println("ntp=" + S.ntp);
  f.println("agc_max=" + String(S.agcMax));
  f.println("wifi_check_min=" + String(S.wifiCheckMin));
  f.println("# 1 = record from a USB mic (DJI receiver) on USB-C when one is plugged in");
  f.println("usb_mic=" + String(S.usbMic ? 1 : 0));
  f.close();
  return true;
}

static bool configured() { return S.ssid.length() && S.davUrl.length(); }

static void setupWizard() {
  struct Field {
    const char* label;
    String* value;
  } fields[] = {{"Wi-Fi name", &S.ssid},   {"Wi-Fi password", &S.pass},  {"WebDAV URL", &S.davUrl},
                {"WebDAV user", &S.davUser}, {"WebDAV password", &S.davPass}};
  for (auto& fl : fields) *fl.value = textInput(fl.label, 180000, Input::Any, 120, *fl.value);
  bool ok = saveSettings();
  parseDavUrl();
  rtcWifiCh = 0;
  ui(ok ? "SAVED" : "NOT SAVED", ok ? TFT_GREEN : TFT_RED, ok ? CFG_PATH : "no SD card", "", "u = test upload now");
  delay(2000);
}

// ================================================================ sleep
// Light sleep, not deep sleep: G0 is the ESP32-S3 boot strap pin, and waking from
// deep sleep with it held low restarts the chip into the ROM download mode (black
// screen). Light sleep just pauses the CPU, so G0 starts a recording instantly.
enum class Wake { Button, Timer };

static Wake lightSleep() {
  saveClock();
  cancelBackgroundCheck();
  screenOff();
  M5.Led.setAllColor(0, 0, 0);
  wifiOff();
  if (sdOk) sdEnd();
  while (gpio_get_level(PIN_BTN) == 0) delay(10);  // a held button would wake us at once
  delay(50);
  gpio_wakeup_enable(PIN_BTN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (rtcPending) esp_sleep_enable_timer_wakeup((uint64_t)S.wifiCheckMin * 60ULL * 1000000ULL);
  esp_light_sleep_start();
  const bool timer = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  gpio_wakeup_disable(PIN_BTN);
  sdOk = sdBegin();
  return timer ? Wake::Timer : Wake::Button;
}

static void recordNow();

// Sleeps until G0 (-> record) ; timer wakes only run the background upload check.
static void sleepUntilButton() {
  for (;;) {
    if (lightSleep() == Wake::Button) {
      logEvent("wake_button");
      recordNow();
      return;
    }
    logEvent("wake_timer", String(rtcPending) + " pending");
    uploadAll(false);
    if (uploadAbort) {  // G0 pressed during that upload
      uploadAbort = false;
      recordNow();
      return;
    }
  }
}

[[noreturn]] static void deepSleep() {
  saveClock();  // the power switch would otherwise lose the time
  screenOff();
  M5.Led.setAllColor(0, 0, 0);

  if (sdOk) sdEnd();
  wifiOff();
  while (gpio_get_level(PIN_BTN) == 0) delay(10);  // a held button would wake us immediately
  delay(50);
  for (auto pin : HOLD_LOW_PINS) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
  }
  gpio_deep_sleep_hold_en();
  rtc_gpio_pullup_en(PIN_BTN);
  rtc_gpio_pulldown_dis(PIN_BTN);
  esp_sleep_enable_ext0_wakeup(PIN_BTN, 0);
  if (rtcPending) esp_sleep_enable_timer_wakeup((uint64_t)S.wifiCheckMin * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// ================================================================ menu
static void drawMenu() {
  String sync = rtcLastSyncOk < 0 ? "never" : (rtcLastSyncOk ? "ok " : "FAILED ");
  if (rtcLastSyncOk >= 0 && rtcLastSync > 1704067200) {
    struct tm t;
    localtime_r(&rtcLastSync, &t);
    char b[16];
    strftime(b, sizeof(b), "%d.%m %H:%M", &t);
    sync += b;
  }
  String label = !sdOk ? "NO SD CARD" : (!configured() ? "press c to set up" : "gencord " GENCORD_VERSION);
  String info = netStatus.length() ? netStatus
                                   : String(rtcPending) + " on card  |  sync " + (configured() ? sync : "not set up");
  // White = fetched from the internet (or set by hand) since power-on.
  // Red "~" = only the last saved time, may be off; red "TIME NOT SET" = nothing known.
  menuScreen(timeValid() ? (rtcTimeExact ? "" : "~") + timeStr("%H:%M %d.%m.") : "TIME NOT SET",
             timeValid() && rtcTimeExact ? TFT_WHITE : TFT_RED, label,
             info, "(G0)rec (u)p (l)ist (o)ff (t)ime (a)gc");
}

// After a recording: look for home Wi-Fi in the background; the menu stays usable.
static void afterRecording(const RecResult& r) {
  if (r.ok && !r.lowBatt) startBackgroundCheck();
}

static void recordNow() {
  cancelBackgroundCheck();
  afterRecording(recordSession());
}

static void menuLoop() {
  screenOn();
  uint32_t lastInput = millis(), lastDraw = 0, statusSince = 0;
  for (;;) {
    M5Cardputer.update();
    const uint32_t now = millis();
    bool acted = false;
    pollBackgroundCheck();  // may run an upload; G0 there sets uploadAbort
    // isChange() consumes the change, so it must be read exactly once per loop
    const bool keyDown = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
    if (Serial.available()) {  // USB diagnostics: 'm' = microphone self-test
      const int cmd = Serial.read();
      if (cmd == 'm') micSelfTest();
      else if (cmd == 'l') serialListCard();
      else if (cmd == 'w') serialSdTest();
      else if (cmd == 'F') {  // the PC is about to flash: say so on the screen
        screenOn();
        // The PC will reset the chip into download mode within a few seconds. Stay
        // awake and do not start a recording meanwhile; G0 cancels.
        Serial.println("FLASH-OK");
        Serial.flush();
        ui("UPDATING", TFT_YELLOW, "new firmware via USB", "do not unplug", "G0 = cancel");
        const uint32_t until = millis() + 20000;
        while (millis() < until) {
          M5.update();
          if (M5.BtnA.wasClicked()) break;
          delay(10);
        }
      }
      acted = true;
    }
    if (uploadAbort) {
      uploadAbort = false;
      recordNow();
      acted = true;
    } else if (M5.BtnA.wasClicked()) {
      recordNow();
      acted = true;
    } else if (keyDown && !screenIsOn) {
      acted = true;  // screen was off: this key only wakes it
    } else if (keyDown) {
      auto& ks = M5Cardputer.Keyboard.keysState();
      char k = ks.word.empty() ? 0 : tolower(ks.word[0]);
      if (k == 'r' || ks.enter) recordNow();
      else if (k == 'u') { cancelBackgroundCheck(); uploadAll(true); }
      else if (k == 't') setTimeManually();
      else if (k == 'c') setupWizard();
      else if (k == 'l') listScreen();
      else if (k == 'a') agcTest();
      else if (k == 'o' || k == 's') sleepUntilButton();
      acted = true;
    }
    if (acted) {
      screenOn();
      lastInput = millis();
      lastDraw = 0;
    }
    if (netStatus.length() && !bgBusy()) {  // "no home Wi-Fi" stays visible for 5 s
      if (!statusSince) statusSince = now;
      else if (now - statusSince > 5000) { netStatus = ""; statusSince = 0; lastDraw = 0; }
    }
    // 20 s without input: screen off (device stays awake, any key wakes it);
    // IDLE_SLEEP_S without input: light sleep, then only G0 wakes.
    if (screenIsOn && !bgBusy() && millis() - lastInput > MENU_SCREEN_S * 1000UL) screenOff();
    if (screenIsOn && now - lastDraw > (bgBusy() ? 250u : 1000u)) {
      drawMenu();
      lastDraw = now;
    }
    if (!bgBusy() && millis() - lastInput > IDLE_SLEEP_S * 1000UL) {
      sleepUntilButton();
      screenOn();
      lastInput = millis();
      lastDraw = 0;
    }
    delay(10);
  }
}

// ================================================================ entry
void setup() {
  for (auto pin : HOLD_LOW_PINS) gpio_hold_dis(pin);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit(PIN_BTN);

  auto cfg = M5.config();
  cfg.internal_spk = true;   // only for the low-battery beep; shares GPIO43 with the mic (see lowBattBeep)
  cfg.internal_mic = false;  // the mic is driven directly (micStart)
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  M5Cardputer.begin(cfg, true);
  Serial.begin(115200);  // USB CDC: diagnostics only
  M5.Display.setBrightness(0);
  M5.Display.setRotation(1);
  screenIsOn = false;

  sdOk = sdBegin();
  loadSettings();
  setenv("TZ", S.tz.c_str(), 1);
  tzset();
  rtcBoots++;
  setCpuFrequencyMhz(80);

  filledQ = xQueueCreate(NBUF, sizeof(int16_t*));
  freeQ = xQueueCreate(NBUF, sizeof(int16_t*));
  parseDavUrl();
  const bool restoredClock = restoreClock();
  if (sdOk) rtcPending = listRecordings(true).size();

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:  // G0 pressed while asleep -> record right away
      logEvent("wake_button");
      afterRecording(recordSession());
      menuLoop();
      break;
    case ESP_SLEEP_WAKEUP_TIMER:  // background check for home Wi-Fi
      logEvent("wake_timer", String(rtcPending) + " pending");
      if (rtcPending) uploadAll(false);
      deepSleep();
      break;
    default:  // power switch / reset
      logEvent(restoredClock ? "boot_time_restored" : "boot", GENCORD_VERSION " " __DATE__ " " __TIME__);
      startBackgroundCheck();  // NTP (and pending uploads) if the home Wi-Fi is around
      menuLoop();
  }
}

void loop() {}
