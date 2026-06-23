#include "ph3b3.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"

void IrisPh3b3::begin(IrisFace* face, const String& host, int port) {
    _face = face;
    _host = host;
    _port = port;

    // Register Iris on the network as "iris.local" so Ph3b3 can find us too.
    MDNS.begin("iris");

    // Try to resolve Ph3b3 via mDNS.  ESP32's lwIP stack resolves .local names
    // natively through WiFi.hostByName(); ESPmDNS must be started first.
    IPAddress resolved;
    if (WiFi.hostByName(PH3B3_MDNS_HOST, resolved)) {
        String s = resolved.toString();
        if (s != "0.0.0.0") {
            _host = s;
        }
    }
    // If mDNS failed, _host retains the NVS / fallback value.

    // Run the first health check immediately so the face updates without waiting
    // a full poll interval.
    _lastMs = millis() - PH3B3_POLL_MS;
}

void IrisPh3b3::update() {
    if (millis() - _lastMs < PH3B3_POLL_MS) return;
    _lastMs = millis();

    if (_checkHealth()) {
        _face->setState(IrisState::PH3B3_HEALTHY);
    } else {
        _face->setState(IrisState::PH3B3_UNREACHABLE);
    }
}

bool IrisPh3b3::_checkHealth() {
    WiFiClientSecure client;
    client.setInsecure();   // Ph3b3 uses a self-signed cert; skip verification

    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/health";

    if (!http.begin(client, url)) return false;

    http.setTimeout(PH3B3_HTTP_TIMEOUT_MS);
    http.setAuthorization(PH3B3_AUTH_USER, PH3B3_AUTH_PASS);

    int code = http.GET();
    http.end();

    return (code >= 200 && code < 300);
}
