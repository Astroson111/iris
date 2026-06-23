#pragma once
#include <Arduino.h>
#include "face.h"

// Discovers Ph3b3 on the network and polls /health periodically.
// Keeps the avatar expression in sync with Ph3b3's reachability.
class IrisPh3b3 {
 public:
    // host / port come from IrisWifi (NVS or portal defaults).
    // Attempts mDNS resolution of PH3B3_MDNS_HOST immediately; if that fails,
    // falls back to the supplied host string.
    void begin(IrisFace* face, const String& host, int port);

    // Call every loop iteration.  Runs a health check every PH3B3_POLL_MS ms.
    void update();

 private:
    IrisFace*     _face   = nullptr;
    String        _host;
    int           _port   = 0;
    unsigned long _lastMs = 0;

    bool _checkHealth();
};
