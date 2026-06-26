#include "wifi_mgr.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "config.h"

// Full WiFi stack teardown + PMF-capable join.
// Fixes WPA2/WPA3 mixed-mode heap leak: WiFi.begin() alone doesn't release
// the WPA supplicant's group-key IE buffers on reason 26/32 rejection.
static void _wifiJoin(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_OFF);
    delay(400);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    wifi_config_t conf = {};
    strlcpy((char*)conf.sta.ssid,     ssid, sizeof(conf.sta.ssid));
    strlcpy((char*)conf.sta.password, pass, sizeof(conf.sta.password));
    conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    conf.sta.pmf_cfg.capable    = true;
    conf.sta.pmf_cfg.required   = false;
#if defined(WPA3_SAE_PWE_BOTH)
    conf.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
    esp_wifi_set_config(WIFI_IF_STA, &conf);
    esp_wifi_connect();
}

static volatile uint8_t _wifiDisconnReason = 0;

// ── IrisWifi::begin ───────────────────────────────────────────────────────────

void IrisWifi::begin(IrisFace* face) {
    _face = face;
    _loadPrefs();

    _face->setState(IrisState::WIFI_CONNECTING);

    // ── Phase 1: try all saved slots in order ─────────────────────────────────
    String ssids[WIFI_SLOT_MAX], passes[WIFI_SLOT_MAX];
    int count = 0;
    _loadWifiSlots(ssids, passes, count);

    if (count > 0) {
        WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
            if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
                _wifiDisconnReason = info.wifi_sta_disconnected.reason;
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

        for (int i = 0; i < count; i++) {
            if (_trySlot(ssids[i], passes[i], i, count)) return;
        }

        // All slots failed — erase ESP32 WiFi NVS so portal can't silently re-connect
        WiFi.disconnect(true, true);
        delay(100);
    }

    // ── Phase 2: DIY captive portal ───────────────────────────────────────────
    _runPortal();
}

// Try one slot; returns true if connected within WIFI_CONNECT_TIMEOUT_S.
bool IrisWifi::_trySlot(const String& ssid, const String& pass, int idx, int total) {
    _wifiDisconnReason = 0;
    _wifiJoin(ssid.c_str(), pass.c_str());

    String base = ">" + ssid.substring(0, 7) + "(" + String(idx + 1) + "/" + String(total) + ")";
    _face->setStatusLine(base.c_str());
    _face->update();

    uint32_t t0  = millis();
    uint32_t lim = (uint32_t)WIFI_CONNECT_TIMEOUT_S * 1000UL;
    while (millis() - t0 < lim) {
        if (WiFi.status() == WL_CONNECTED) return true;
        uint32_t s = (lim - (millis() - t0) + 999) / 1000;
        _face->setStatusLine((base + " " + String(s) + "s").c_str());
        _face->update();
        delay(300);
    }

    // Show fail reason briefly before trying next slot
    String rsn;
    switch (_wifiDisconnReason) {
        case 201: rsn = "NO_AP R:201"; break;
        case 202: rsn = "AUTH  R:202"; break;
        case 15:  rsn = "HSHK  R:15";  break;
        case 204: rsn = "HSHK  R:204"; break;
        case 0:   rsn = "R:timeout";   break;
        default:  rsn = "R:" + String(_wifiDisconnReason); break;
    }
    _face->setStatusLine(rsn.c_str());
    _face->update();
    delay(2000);
    return false;
}

// ── DIY captive portal ────────────────────────────────────────────────────────

void IrisWifi::_runPortal() {
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.softAP(IRIS_AP_SSID);
    delay(200);

    _face->setState(IrisState::CONFIG_PORTAL);
    _face->setStatusLine("192.168.4.1");
    _face->update();

    // Load current slots to pre-fill form
    String ssids[WIFI_SLOT_MAX], passes[WIFI_SLOT_MAX];
    int count = 0;
    _loadWifiSlots(ssids, passes, count);

    DNSServer dns;
    dns.start(53, "*", IPAddress(192, 168, 4, 1));

    WebServer server(80);

    server.on("/", HTTP_GET, [&]() {
        String page =
            F("<!DOCTYPE html><html><head>"
              "<meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Iris Setup</title>"
              "<style>"
              "body{font-family:sans-serif;background:#0a0318;color:#e0d4ff;"
                   "max-width:480px;margin:0 auto;padding:1rem}"
              "h1{color:#a855f7;margin-bottom:.2rem}"
              "p.sub{color:#a09ab8;font-size:.85rem;margin-top:0}"
              ".slot{background:#1a0a38;border:1px solid #3d1f7a;border-radius:8px;"
                    "padding:.75rem;margin:.5rem 0}"
              ".slot h3{margin:0 0 .5rem;font-size:.9rem;color:#c084fc}"
              "label{font-size:.8rem;color:#a09ab8;display:block;margin-top:.4rem}"
              "input{width:100%;box-sizing:border-box;background:#0d0525;"
                    "border:1px solid #4b2c8a;border-radius:4px;color:#e0d4ff;"
                    "padding:.4rem .5rem;font-size:.95rem;margin-top:2px}"
              "input:focus{outline:none;border-color:#a855f7}"
              ".srv{background:#1a0a38;border:1px solid #3d1f7a;border-radius:8px;"
                   "padding:.75rem;margin-top:1rem}"
              ".row2{display:grid;grid-template-columns:3fr 1fr;gap:.5rem}"
              "button{width:100%;margin-top:1rem;padding:.75rem;background:#7e22ce;"
                     "border:none;border-radius:8px;color:#fff;font-size:1rem;"
                     "font-weight:700;cursor:pointer}"
              "button:hover{background:#9333ea}"
              "small{display:block;color:#7c66aa;font-size:.75rem;margin-top:.5rem}"
              "</style></head><body>"
              "<h1>Iris Setup</h1>"
              "<p class='sub'>Iris tries networks in order, 8 s each.</p>"
              "<form method='POST' action='/save'>");

        for (int i = 0; i < WIFI_SLOT_MAX; i++) {
            page += "<div class='slot'><h3>Network ";
            page += String(i + 1);
            page += "</h3><label>SSID</label>"
                    "<input name='ssid";
            page += String(i);
            page += "' value='";
            page += (i < count ? ssids[i] : "");
            page += "'><label>Password</label>"
                    "<input name='pass";
            page += String(i);
            page += "' type='password' placeholder='(blank keeps saved password)'>"
                    "</div>";
        }

        page += F("<div class='srv'><h3>Ph3b3 Server</h3>"
                  "<div class='row2'>"
                  "<div><label>Host / IP</label>"
                  "<input name='ph3b3host' value='");
        page += _ph3b3Host;
        page += F("'></div><div><label>Port</label>"
                  "<input name='ph3b3port' value='");
        page += String(_ph3b3Port);
        page += F("'></div></div></div>"
                  "<button type='submit'>Save &amp; Reboot</button>"
                  "<small>Clear an SSID field to remove that slot. "
                  "Leave password blank to keep the existing saved password.</small>"
                  "</form>"
                  "<div style='position:fixed;bottom:12px;right:12px;width:110px;opacity:0.75'>"
                  "<svg width='100%' viewBox='0 0 680 460' role='img' xmlns='http://www.w3.org/2000/svg'>"
                  "<title>Peach, a golden retriever, sleeping in a little bed</title>"
                  "<desc>A line-art golden retriever curled up asleep in a small cushioned bed, "
                  "with a crescent moon and small stars above, drawn in a soft monochrome style "
                  "to match the Ph3b3 crescent art.</desc>"
                  "<style>"
                  ".ink{fill:none;stroke:#2b2b2b;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}"
                  ".ink-soft{fill:none;stroke:#6b6b6b;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}"
                  ".fur{fill:#e8a85c;opacity:0.22}"
                  ".bed{fill:#6b6b6b;opacity:0.10}"
                  ".cap{fill:#6b6b6b;font-family:Georgia,serif;font-size:15px;font-style:italic}"
                  "</style>"
                  "<circle cx='500' cy='95' r='34' fill='none' stroke='#6b6b6b' stroke-width='1.5'/>"
                  "<circle cx='512' cy='88' r='34' fill='#ffffff' stroke='none'/>"
                  "<circle cx='512' cy='88' r='34' fill='#6b6b6b' opacity='0.10'/>"
                  "<path d='M180 150 l3 7 l7 1 l-5 5 l1 7 l-6 -3 l-6 3 l1 -7 l-5 -5 l7 -1 z' fill='#9b9b9b'/>"
                  "<path d='M250 110 l2 5 l5 1 l-4 3 l1 5 l-4 -2 l-4 2 l1 -5 l-4 -3 l5 -1 z' fill='#9b9b9b'/>"
                  "<path d='M560 175 l2 5 l5 1 l-4 3 l1 5 l-4 -2 l-4 2 l1 -5 l-4 -3 l5 -1 z' fill='#9b9b9b'/>"
                  "<ellipse cx='340' cy='370' rx='210' ry='50' class='bed'/>"
                  "<path class='ink-soft' d='M140 360 q-8 -34 30 -42 q170 -22 340 0 q38 8 30 42 q-6 30 -50 36 q-150 18 -300 0 q-44 -6 -50 -36 z'/>"
                  "<path class='ink-soft' d='M150 358 q180 24 380 0'/>"
                  "<path class='fur' d='M250 290 q-70 10 -78 56 q-6 40 60 50 q120 16 230 -4 q70 -14 56 -64 q-14 -44 -90 -48 q-24 24 -64 22 q-46 -2 -54 -28 q-8 6 -16 8 z'/>"
                  "<path class='ink' d='M250 292 q-66 12 -74 54 q-6 38 58 48 q116 16 224 -4 q66 -14 54 -60 q-14 -42 -86 -46'/>"
                  "<path class='ink' d='M250 292 q12 -22 44 -22 q34 0 42 26'/>"
                  "<path class='ink' d='M336 296 q12 18 52 16 q44 -2 56 -24'/>"
                  "<path class='ink' d='M444 288 q42 -36 72 -20 q22 12 6 40 q-12 22 -42 28'/>"
                  "<path class='ink-soft' d='M470 282 q24 -14 40 -4'/>"
                  "<path class='ink' d='M250 292 q-30 -16 -34 -44 q-2 -20 16 -26 q20 -6 30 14'/>"
                  "<path class='ink' d='M232 222 q-14 -4 -20 8 q-6 14 8 24'/>"
                  "<circle cx='244' cy='258' r='2.5' fill='#2b2b2b'/>"
                  "<path class='ink-soft' d='M236 256 q8 -5 16 0'/>"
                  "<path class='ink' d='M258 272 q8 6 18 2'/>"
                  "<path class='ink' d='M266 268 q2 6 -2 10'/>"
                  "<circle cx='270' cy='276' r='3' fill='#2b2b2b'/>"
                  "<path class='ink-soft' d='M300 330 q40 10 90 4'/>"
                  "<path class='ink-soft' d='M330 360 q30 6 70 0'/>"
                  "<path class='ink-soft' d='M540 250 q10 -10 0 -20 q-10 -10 0 -20' opacity='0.7'/>"
                  "<path class='ink-soft' d='M556 236 q8 -8 0 -16 q-8 -8 0 -16' opacity='0.5'/>"
                  "<text x='340' y='432' text-anchor='middle' class='cap'>Peach &mdash; rest easy, good girl</text>"
                  "</svg></div>"
                  "</body></html>");

        server.send(200, "text/html", page);
    });

    // Captive portal: redirect every unknown path to root
    server.onNotFound([&]() {
        server.sendHeader("Location", "http://192.168.4.1/", true);
        server.send(302, "text/plain", "");
    });

    server.on("/save", HTTP_POST, [&]() {
        String newSSIDs[WIFI_SLOT_MAX], newPasses[WIFI_SLOT_MAX];
        int newCount = 0;

        for (int i = 0; i < WIFI_SLOT_MAX; i++) {
            String s = server.arg("ssid" + String(i));
            s.trim();
            if (s.length() == 0) continue;   // cleared SSID = drop slot

            String p = server.arg("pass" + String(i));
            p.trim();
            // Blank password field = keep whatever was saved at this position
            if (p.length() == 0 && i < count) p = passes[i];

            newSSIDs[newCount]  = s;
            newPasses[newCount] = p;
            newCount++;
        }

        String newHost = server.arg("ph3b3host");
        newHost.trim();
        int newPort = server.arg("ph3b3port").toInt();
        if (newHost.length() > 0 && newPort > 0) {
            _ph3b3Host = newHost;
            _ph3b3Port = newPort;
            _savePrefs(newHost.c_str(), newPort);
        }

        _saveWifiSlots(newSSIDs, newPasses, newCount);

        server.send(200, "text/html",
            "<html><body style='font-family:sans-serif;background:#0a0318;"
            "color:#e0d4ff;text-align:center;padding:2rem'>"
            "<h2 style='color:#a855f7'>Saved!</h2>"
            "<p>Rebooting Iris...</p>"
            "</body></html>");
        delay(1000);
        ESP.restart();
    });

    server.begin();

    uint32_t deadline = millis() + (uint32_t)PORTAL_TIMEOUT_S * 1000UL;
    while (millis() < deadline) {
        dns.processNextRequest();
        server.handleClient();
        _face->update();
        delay(5);
    }

    server.stop();
    dns.stop();
    WiFi.mode(WIFI_OFF);
}

// ── Public methods ────────────────────────────────────────────────────────────

bool IrisWifi::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

// Called by the watchdog in loop(). Tries one slot per call, rotating so each
// saved network gets a chance without blocking 32 s per reconnect cycle.
void IrisWifi::reconnect() {
    String ssids[WIFI_SLOT_MAX], passes[WIFI_SLOT_MAX];
    int count = 0;
    _loadWifiSlots(ssids, passes, count);
    if (count == 0) return;

    static int sSlotIdx = 0;
    int slot = sSlotIdx % count;
    _wifiJoin(ssids[slot].c_str(), passes[slot].c_str());
    sSlotIdx = (slot + 1) % count;
}

void IrisWifi::clearWifiCreds() {
    const String empty[WIFI_SLOT_MAX] = {};
    _saveWifiSlots(empty, empty, 0);
}

// ── NVS helpers ───────────────────────────────────────────────────────────────

void IrisWifi::_loadPrefs() {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/true);
    _ph3b3Host = p.getString(NVS_KEY_HOST, PH3B3_FALLBACK_HOST);
    _ph3b3Port = p.getInt(NVS_KEY_PORT, PH3B3_FALLBACK_PORT);
    p.end();
}

void IrisWifi::_savePrefs(const char* host, int port) {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/false);
    p.putString(NVS_KEY_HOST, host);
    p.putInt(NVS_KEY_PORT, port);
    p.end();
}

void IrisWifi::_loadWifiSlots(String ssids[], String passes[], int& count) {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/true);

    // -1 sentinel = NVS_KEY_WIFI_COUNT not yet written (first boot of this firmware)
    int stored = p.getInt(NVS_KEY_WIFI_COUNT, -1);

    if (stored < 0) {
        // One-time migration from legacy single-slot keys
        String legSSID = p.getString("wifi_ssid", "");
        String legPass = p.getString("wifi_pass", "");
        p.end();
        if (legSSID.length() > 0) {
            ssids[0]  = legSSID;
            passes[0] = legPass;
            count = 1;
            _saveWifiSlots(ssids, passes, 1);
            Preferences rw;
            rw.begin(NVS_NAMESPACE, /*readOnly=*/false);
            rw.remove("wifi_ssid");
            rw.remove("wifi_pass");
            rw.end();
        } else {
            count = 0;
        }
        return;
    }

    count = (stored > WIFI_SLOT_MAX) ? WIFI_SLOT_MAX : stored;
    char key[16];
    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "wifi_ssid_%d", i);
        ssids[i] = p.getString(key, "");
        snprintf(key, sizeof(key), "wifi_pass_%d", i);
        passes[i] = p.getString(key, "");
    }
    p.end();
}

void IrisWifi::_saveWifiSlots(const String ssids[], const String passes[], int count) {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/false);
    p.putInt(NVS_KEY_WIFI_COUNT, count);
    char key[16];
    for (int i = 0; i < WIFI_SLOT_MAX; i++) {
        snprintf(key, sizeof(key), "wifi_ssid_%d", i);
        if (i < count) p.putString(key, ssids[i]);
        else           p.remove(key);
        snprintf(key, sizeof(key), "wifi_pass_%d", i);
        if (i < count) p.putString(key, passes[i]);
        else           p.remove(key);
    }
    p.end();
}
