// Copy this file to config.h and fill in your values. config.h is not meant to be shared.
#pragma once

// ---- Home Wi-Fi: uploads only happen when this network is in range ----
#define WIFI_SSID "MyHomeWifi"
#define WIFI_PASS "secret"

// ---- Synology WebDAV target (DSM package "WebDAV Server") ----
// Folder URL without trailing slash. HTTP port 5005, HTTPS port 5006 (self-signed cert is accepted).
// The shared folder must exist; the last path segment is created if missing.
#define DAV_URL  "http://192.168.1.10:5005/Recordings/inbox"
#define DAV_USER "cardputer"
#define DAV_PASS "secret"

// ---- Time ----
#define TZ_INFO    "CET-1CEST,M3.5.0,M10.5.0/3"   // Germany / Central Europe
#define NTP_SERVER "pool.ntp.org"                // or your Synology's IP if it runs the NTP service

// ---- Audio ----
#define SAMPLE_RATE 16000   // 16 kHz mono 16-bit = 115 MB/h, exactly what Whisper uses
#define AGC_MAX     64      // maximum automatic gain; lower (16) for loud rooms

// ---- Power behaviour ----
#define WIFI_CHECK_MIN   20    // while recordings are pending: look for home Wi-Fi every N minutes
#define IDLE_SLEEP_S     60    // menu screen without input -> deep sleep after N seconds
#define NAME_TIMEOUT_S   30    // naming prompt after a recording; nothing typed -> timestamp only
#define LOW_BATT_MV      3400  // stop recording cleanly below this battery voltage
#define SCREEN_BRIGHTNESS 60   // 0..255, only while the screen is on
