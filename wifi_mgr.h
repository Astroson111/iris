#pragma once
#include <Arduino.h>
#include "face.h"

// Handles WiFi onboarding (DIY captive portal), multi-slot credential persistence
// (NVS), and storage of the user-entered Ph3b3 host + port.
class IrisWifi {
 public:
    // Blocks until WiFi is connected or portal times out.
    // On return, call isConnected() to check result.
    void   begin(IrisFace* face);
    bool   isConnected() const;
    void   reconnect();          // try one saved slot (rotates); called by watchdog
    void   clearWifiCreds();     // wipe all saved networks (sets count=0)
    String getPh3b3Host() const { return _ph3b3Host; }
    int    getPh3b3Port() const { return _ph3b3Port; }
    int    getVolIdx()    const { return _volIdx; }   // 0/1/2 → IRIS_VOL_LEVELS
    int    getMicIdx()    const { return _micIdx; }   // 0/1/2 → IRIS_MIC_LEVELS
    String getServerKey() const { return _svrKey; }   // per-device Ph3b3 auth key (Basic-auth password)

 private:
    IrisFace* _face      = nullptr;
    String    _ph3b3Host;
    int       _ph3b3Port = 0;
    int       _volIdx    = 1;   // Medium (IRIS_AUDIO_DEFAULT) until _loadPrefs runs
    int       _micIdx    = 1;
    String    _svrKey;          // per-device Ph3b3 auth key; _loadPrefs falls back to PH3B3_AUTH_PASS

    void _loadPrefs();
    void _savePrefs(const char* host, int port);
    void _loadWifiSlots(String ssids[], String passes[], int& count);
    void _saveWifiSlots(const String ssids[], const String passes[], int count);
    bool _trySlot(const String& ssid, const String& pass, int idx, int total);
    void _runPortal();
};
