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
        // Write AP IP to the status band so the user knows where to navigate.
        // Called from the WiFiManager blocking loop — Avatar task does not
        // write to the status band region, so direct draw is safe here.
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

    WiFiManager wm;
    wm.setAPCallback(onPortalStart);
    wm.setSaveParamsCallback(onSaveParams);
    wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
    wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);

    // Custom fields shown in the portal so the user can update Ph3b3's address.
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", _ph3b3Port);
    WiFiManagerParameter hostParam("ph3b3host", "Ph3b3 host", _ph3b3Host.c_str(), 64);
    WiFiManagerParameter portParam("ph3b3port", "Ph3b3 port", portStr, 6);
    wm.addParameter(&hostParam);
    wm.addParameter(&portParam);

    bool connected = wm.autoConnect(IRIS_AP_SSID);

    if (connected && _paramsDirty) {
        String newHost = String(hostParam.getValue());
        int    newPort = atoi(portParam.getValue());
        if (newHost.length() > 0 && newPort > 0) {
            _ph3b3Host = newHost;
            _ph3b3Port = newPort;
            _savePrefs(newHost.c_str(), newPort);
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
