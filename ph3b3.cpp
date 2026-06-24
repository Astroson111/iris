#include "ph3b3.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"

void IrisPh3b3::begin(IrisFace* face, const String& host, int port) {
    _face = face;
    _host = host;
    _port = port;
    // ph3b3.local never resolves (Nyx advertises as Nyx.local); skip mDNS entirely
    // to avoid the 2s+ blocking penalty on every boot.
    _lastMs = millis() - PH3B3_POLL_MS;   // trigger first check immediately
}

void IrisPh3b3::update() {
    if (millis() - _lastMs < PH3B3_POLL_MS) return;
    _lastMs = millis();

    if (_checkHealth()) {
        _face->setState(IrisState::PH3B3_HEALTHY);
        if (!_greetedOnce) {
            _face->setBubble("Hello! Ph3b3 is online and ready.");
            _greetedOnce = true;
        }
    } else {
        _face->setState(IrisState::PH3B3_UNREACHABLE);
        _face->clearBubble();
        _greetedOnce = false;
    }
}

bool IrisPh3b3::_checkHealth() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/health";

    _face->setStatusLine((_host + ":" + String(_port)).c_str());
    _face->update();

    if (!http.begin(client, url)) {
        _face->setStatusLine("http.begin fail");
        _face->update();
        delay(2000);
        return false;
    }

    // setConnectTimeout sets SO_RCVTIMEO on the socket; without it the default
    // 5 s can race the TLS handshake. http.setTimeout() alone is not enough.
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setAuthorization(PH3B3_AUTH_USER, PH3B3_AUTH_PASS);

    int code = http.GET();
    http.end();

    _face->setStatusLine(("HTTP " + String(code)).c_str());
    _face->update();
    delay(2000);

    return (code >= 200 && code < 300);
}
