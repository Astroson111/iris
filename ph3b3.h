#pragma once
#include <Arduino.h>
#include "face.h"

// Discovers Ph3b3 on the network and polls /health periodically.
// Keeps the avatar expression in sync with Ph3b3's reachability.
class IrisPh3b3 {
 public:
    void begin(IrisFace* face, const String& host, int port);

    // Call every loop iteration.  Runs a health check every PH3B3_POLL_MS ms.
    void update();

    // Blocking: POST message to /chat, stream + play TTS audio response.
    void doChat(const String& message);

    // Blocking: base64-encode audio, POST /transcribe, then doChat() with result.
    // Takes ownership of *buf and frees it (sets *buf = nullptr).
    void doPtt(int16_t** buf, int numSamples);

 private:
    IrisFace*     _face         = nullptr;
    String        _host;
    int           _port         = 0;
    String        _sessionId;
    unsigned long _lastMs       = 0;
    bool          _greetedOnce  = false;
    uint32_t      _greetClearMs = 0;

    bool _checkHealth();
};
