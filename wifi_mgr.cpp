#include "wifi_mgr.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "config.h"

// ── File-scope state shared with WiFiManager callbacks ────────────────────
static IrisFace* _facePtr   = nullptr;
static bool      _paramsDirty = false;   // set true when portal saves new values
static volatile uint8_t _wifiDisconnReason = 0;

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
    // Boot diagnostic: show what SSID is in NVS — visible for 2s so cred-persistence
    // can be verified from the screen without serial.
    String wSSID, wPass;
    _loadWifiPrefs(wSSID, wPass);

    {
        String d = wSSID.length() > 0 ? ("NVS>" + wSSID.substring(0, 12)) : "NVS:empty";
        _face->setStatusLine(d.c_str());
        _face->update();
        delay(2000);
    }

    if (wSSID.length() > 0) {
        _wifiDisconnReason = 0;
        WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
            if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
                _wifiDisconnReason = info.wifi_sta_disconnected.reason;
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

        WiFi.mode(WIFI_STA);
        WiFi.begin(wSSID.c_str(), wPass.c_str());

        // Show the SSID we're actually trying — catches + → space mangling
        String ssidTag = ">" + (wSSID.length() > 11 ? wSSID.substring(0, 11) + "~" : wSSID);
        _face->setStatusLine(ssidTag.c_str());
        _face->update();
        delay(800);

        uint32_t t0  = millis();
        uint32_t lim = (uint32_t)WIFI_CONNECT_TIMEOUT_S * 1000UL;
        while (millis() - t0 < lim) {
            if (WiFi.status() == WL_CONNECTED) return;
            uint32_t secLeft = (lim - (millis() - t0) + 999) / 1000;
            _face->setStatusLine(("wifi... " + String(secLeft) + "s").c_str());
            _face->update();
            delay(300);
        }

        // Hold reason code on screen for 5s before falling to portal
        // 201=NO_AP_FOUND (band/SSID)  202=AUTH_FAIL (password)
        // 15/204=handshake timeout (password)  0=no event fired
        String rsn;
        switch (_wifiDisconnReason) {
            case 201: rsn = "NO_AP R:201";  break;
            case 202: rsn = "AUTH  R:202";  break;
            case 15:  rsn = "HSHK  R:15";   break;
            case 204: rsn = "HSHK  R:204";  break;
            case 0:   rsn = "R:timeout";    break;
            default:  rsn = "R:" + String(_wifiDisconnReason); break;
        }
        _face->setStatusLine(rsn.c_str());
        _face->update();
        delay(5000);

        // Timed out — erase the ESP32 WiFi NVS (eraseap=true) so WiFiManager
        // cannot silently auto-connect to any previously saved network and is
        // forced to open the captive portal for fresh credentials.
        WiFi.disconnect(true, true);
        delay(100);
    }

    // ── Phase 2: WiFiManager captive portal ───────────────────────────────────
    // Erase ESP32 WiFi NVS unconditionally before opening the portal.
    // This covers the Phase-1-skipped case (empty "iris" NVS on first boot)
    // where disconnect(true,true) hasn't run yet, so WiFiManager can't silently
    // auto-connect to a stale network and must always go through the portal.
    WiFi.disconnect(true, true);
    delay(100);

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
        // Show what we saved — on-screen confirmation since serial is dead.
        String saved = "saved:" + WiFi.SSID().substring(0, 10);
        _face->setStatusLine(saved.c_str());
        _face->update();
        delay(2000);

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
