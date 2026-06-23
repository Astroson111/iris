#pragma once
#include <Arduino.h>
#include "face.h"

// Handles WiFi onboarding (captive portal), credential persistence (NVS),
// and storage of the user-entered Ph3b3 host + port.
class IrisWifi {
 public:
    // Blocks until WiFi is connected or portal times out.
    // On return, call isConnected() to check result.
    void   begin(IrisFace* face);
    bool   isConnected() const;
    String getPh3b3Host() const { return _ph3b3Host; }
    int    getPh3b3Port() const { return _ph3b3Port; }

 private:
    IrisFace* _face      = nullptr;
    String    _ph3b3Host;
    int       _ph3b3Port = 0;

    void _loadPrefs();
    void _savePrefs(const char* host, int port);
};
