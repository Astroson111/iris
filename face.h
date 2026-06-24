#pragma once
#include <Arduino.h>

enum class IrisState : uint8_t {
    BOOT,
    CONFIG_PORTAL,
    WIFI_CONNECTING,
    PH3B3_SEARCHING,
    PH3B3_HEALTHY,
    PH3B3_UNREACHABLE,
    LISTENING,
    THINKING,
    SPEAKING,
};

class IrisFace {
 public:
    void      begin();
    void      update();
    void      setState(IrisState s, const char* speechOverride = nullptr);
    void      setStatusLine(const char* text);
    void      setBubble(const String& text);
    void      clearBubble();
    void      setSpeakingLevel(float level01);
    IrisState getState() const;
};
