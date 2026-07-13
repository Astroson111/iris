#pragma once

#include "secrets.h"

// ── Ph3b3 connection ──────────────────────────────────────────────────────────
#define PH3B3_MDNS_HOST        "ph3b3.local"  // resolved via WiFi.hostByName / lwIP mDNS
#define PH3B3_FALLBACK_HOST    "192.168.0.16"
#define PH3B3_FALLBACK_PORT    7331
#define PH3B3_POLL_MS          10000UL         // health-check interval (ms)
#define PH3B3_HTTP_TIMEOUT_MS  8000            // per-request timeout (TLS handshake included)
#define PH3B3_MDNS_TIMEOUT_MS  2000            // mDNS resolution timeout (ms)

// ── WiFi / captive portal ─────────────────────────────────────────────────────
#define IRIS_AP_SSID           "Iris-Setup"    // SoftAP SSID shown in config portal
#define PORTAL_TIMEOUT_S       180             // portal auto-closes after 3 min
#define WIFI_CONNECT_TIMEOUT_S 8               // seconds per saved network slot before trying next
#define WIFI_SLOT_MAX          4               // max stored networks

// ── NVS (Preferences namespace + keys) ───────────────────────────────────────
#define NVS_NAMESPACE          "iris"
#define NVS_KEY_HOST           "ph3b3host"
#define NVS_KEY_PORT           "ph3b3port"
#define NVS_KEY_WIFI_COUNT     "wifi_count"
// Per-slot keys built at runtime: "wifi_ssid_0".."wifi_ssid_3", "wifi_pass_0".."wifi_pass_3"

// ── Audio presets (Volume / Mic gain) — set in the setup portal, NVS-persisted ─
// Reuses the Dio Settings convention: preset INDEX 0/1/2 stored, Medium default.
#define NVS_KEY_VOLUME         "vol"
#define NVS_KEY_MIC            "mic"
static const int   IRIS_VOL_LEVELS[3]   = {102, 178, 255};  // Low 40% / Med 70% / High 100% of M5.Speaker
static const int   IRIS_MIC_LEVELS[3]   = {8, 16, 32};      // Low / Med(=16, current) / High mic magnification
static const char* IRIS_PRESET_NAMES[3] = {"Low", "Medium", "High"};
static const int   IRIS_AUDIO_DEFAULT   = 1;                // Medium for both (fresh enrollment never silent/blasting/deaf)

// ── Display layout ────────────────────────────────────────────────────────────
// StickS3: ST7789 135×240, portrait.
// Avatar sprite occupies top 200 px; status band lives in the remaining 40 px.
#define SCREEN_W               135
#define SCREEN_H               240
#define FACE_SPRITE_W          135
#define FACE_SPRITE_H          200
#define STATUS_CENTER_Y        220             // y-centre of the status text band

// ── Deep-sleep / battery save ─────────────────────────────────────────────────
// If WiFi connects but Ph3b3 is unreachable for WIFI_PH3B3_TIMEOUT_MS after
// boot, or if WiFi never connects at all, Iris deep-sleeps and retries every
// SLEEP_RETRY_US microseconds. BtnA (GPIO39) also triggers an early wake.
// GPIO39 is the only RTC-capable button on ESP32-S3; BtnB/GPIO46 is not.
#define WIFI_PH3B3_TIMEOUT_MS  60000UL                // 60 s patience window
#define SLEEP_RETRY_US         (5ULL * 60 * 1000000)  // 5-min deep-sleep timer
#define SLEEP_WAKE_GPIO        39                     // BtnA — RTC-capable on S3
