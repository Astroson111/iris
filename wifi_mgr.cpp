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

    _face->setStatusLine(("SSID:" + (wSSID.length() ? wSSID : "(none)")).c_str());
    _face->update();
    delay(2000);

    if (wSSID.length() > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wSSID.c_str(), wPass.c_str());
        uint32_t t0 = millis();
        while (millis() - t0 < 20000) {
            wl_status_t s = WiFi.status();
            if (s == WL_CONNECTED) break;
            _face->setStatusLine(("WiFi:" + String((int)s)).c_str());
            _face->update();
            delay(300);
        }
        if (WiFi.status() == WL_CONNECTED) {
            _face->setStatusLine(("IP:" + WiFi.localIP().toString()).c_str());
            _face->update();
            delay(2000);
            return;
        }
        _face->setStatusLine(("fail " + String((int)WiFi.status()) + "->portal").c_str());
        _face->update();
        delay(1000);
    }

    // ── Phase 2: WiFiManager portal ───────────────────────────────────────────
    _face->setState(IrisState::CONFIG_PORTAL);
    WiFiManager wm;
    wm.setAPCallback(onPortalStart);
    wm.setSaveParamsCallback(onSaveParams);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);

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
