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

 private:
    IrisFace* _face      = nullptr;
    String    _ph3b3Host;
    int       _ph3b3Port = 0;

    void _loadPrefs();
    void _savePrefs(const char* host, int port);
    void _loadWifiSlots(String ssids[], String passes[], int& count);
    void _saveWifiSlots(const String ssids[], const String passes[], int count);
    bool _trySlot(const String& ssid, const String& pass, int idx, int total);
    void _runPortal();
};
