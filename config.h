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

// ── Display layout ────────────────────────────────────────────────────────────
// StickS3: ST7789 135×240, portrait.
// Avatar sprite occupies top 200 px; status band lives in the remaining 40 px.
#define SCREEN_W               135
#define SCREEN_H               240
#define FACE_SPRITE_W          135
#define FACE_SPRITE_H          200
#define STATUS_CENTER_Y        220             // y-centre of the status text band
