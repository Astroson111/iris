#include "wifi_mgr.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "config.h"

// ── File-scope state shared with WiFiManager callbacks ────────────────────
static IrisFace* _facePtr   = nullptr;
static bool      _paramsDirty = false;   // set true when portal saves new values

static void onPortalStart(WiFiManager*) {
    if (_facePtr) {
        _facePtr->setState(IrisState::CONFIG_PORTAL);
        _facePtr->setStatusLine("192.168.4.1");
        _facePtr->update();   // push to display before autoConnect blocks the loop
    }
}

static void onSaveParams() {
    _paramsDirty = true;
}

// ── IrisWifi implementation ────────────────────────────────────────────────

void IrisWifi::begin(IrisFace* face) {
    _face    = face;
    _facePtr = face;

    _loadPrefs();  // populate _ph3b3Host / _ph3b3Port from NVS

    _face->setState(IrisState::WIFI_CONNECTING);

    // ── Phase 1: our own NVS credentials ─────────────────────────────────────
    // WiFiManager clears the ESP32 WiFi NVS when it opens the AP, so we keep
    // credentials in our own "iris" namespace which it cannot touch.
    String wSSID, wPass;
    _loadWifiPrefs(wSSID, wPass);

    if (wSSID.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wSSID.c_str(), wPass.c_str());
        uint32_t t0  = millis();
        uint32_t lim = (uint32_t)WIFI_CONNECT_TIMEOUT_S * 1000UL;
        while (millis() - t0 < lim) {
            if (WiFi.status() == WL_CONNECTED) return;
            uint32_t secLeft = (lim - (millis() - t0) + 999) / 1000;
            _face->setStatusLine(("wifi... " + String(secLeft) + "s").c_str());
            _face->update();
            delay(300);
        }
        // Timed out — stop the pending connection attempt so WiFiManager gets a
        // clean WiFi stack and doesn't silently retry the same stale network.
        WiFi.disconnect(true);
        delay(100);
    }

    // ── Phase 2: WiFiManager captive portal ───────────────────────────────────
    // Show portal state on display before autoConnect() blocks the loop.
    _face->setState(IrisState::CONFIG_PORTAL);
    _face->setStatusLine("192.168.4.1");
    _face->update();

    WiFiManager wm;
    wm.setAPCallback(onPortalStart);
    wm.setSaveParamsCallback(onSaveParams);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
    // Phase 1 already spent WIFI_CONNECT_TIMEOUT_S on saved creds; tell WiFiManager
    // to skip its own connect attempt and go straight to the AP portal.
    wm.setConnectTimeout(1);

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", _ph3b3Port);
    WiFiManagerParameter hostParam("ph3b3host", "Ph3b3 host", _ph3b3Host.c_str(), 64);
    WiFiManagerParameter portParam("ph3b3port", "Ph3b3 port", portStr, 6);
    wm.addParameter(&hostParam);
    wm.addParameter(&portParam);

    bool connected = wm.autoConnect(IRIS_AP_SSID);

    if (connected) {
        // Save working credentials to our own NVS so Phase 1 survives future portal runs.
        _saveWifiPrefs(WiFi.SSID(), WiFi.psk());

        if (_paramsDirty) {
            String newHost = String(hostParam.getValue());
            int    newPort = atoi(portParam.getValue());
            if (newHost.length() > 0 && newPort > 0) {
                _ph3b3Host = newHost;
                _ph3b3Port = newPort;
                _savePrefs(newHost.c_str(), newPort);
            }
        }
    }
}

bool IrisWifi::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

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

void IrisWifi::_loadWifiPrefs(String& ssid, String& pass) {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/true);
    ssid = p.getString(NVS_KEY_WIFI_SSID, "");
    pass = p.getString(NVS_KEY_WIFI_PASS, "");
    p.end();
}

void IrisWifi::_saveWifiPrefs(const String& ssid, const String& pass) {
    Preferences p;
    p.begin(NVS_NAMESPACE, /*readOnly=*/false);
    p.putString(NVS_KEY_WIFI_SSID, ssid);
    p.putString(NVS_KEY_WIFI_PASS, pass);
    p.end();
}
