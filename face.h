#pragma once
#include <Arduino.h>

// All states Iris can be in; each maps to a distinct avatar expression + speech.
enum class IrisState : uint8_t {
    BOOT,              // waking up   → Sleepy
    CONFIG_PORTAL,     // AP + portal → Neutral  (shows SSID text)
    WIFI_CONNECTING,   // trying wifi → Doubt
    PH3B3_SEARCHING,   // wifi up, finding Ph3b3 → Neutral
    PH3B3_HEALTHY,     // Ph3b3 answers          → Happy
    PH3B3_UNREACHABLE, // Ph3b3 silent           → Sad
};

// Owns the Avatar instance and drives expression / status text.
class IrisFace {
 public:
    void      begin();
    void      setState(IrisState s, const char* speechOverride = nullptr);
    void      setStatusLine(const char* text);  // writes to status band (y >= FACE_SPRITE_H)
    IrisState getState() const { return _state; }

 private:
    IrisState _state = IrisState::BOOT;
};
