/*
 * Iris — Ph3b3's face on a new body.
 * M5StickS3 (ESP32-S3), Milestone 2: SVG-faithful face, animation, speech bubble.
 *
 * Build:  arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 ~/Arduino/Iris
 * Flash:  arduino-cli upload  --fqbn m5stack:esp32:m5stack_sticks3 -p /dev/ttyACM0 ~/Arduino/Iris
 */

#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "face.h"
#include "wifi_mgr.h"
#include "ph3b3.h"

IrisFace   irisFace;
IrisWifi   irisWifi;
IrisPh3b3  irisPh3b3;

static void faceDelay(uint32_t ms) {
    uint32_t end = millis() + ms;
    while (millis() < end) { irisFace.update(); delay(20); }
}

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);

    irisFace.begin();
    irisFace.setState(IrisState::BOOT);
    faceDelay(800);

    irisWifi.begin(&irisFace);

    if (!irisWifi.isConnected()) {
        irisFace.setState(IrisState::WIFI_CONNECTING, "No WiFi — rebooting");
        faceDelay(3000);
        ESP.restart();
    }

    irisFace.setState(IrisState::PH3B3_SEARCHING);
    irisPh3b3.begin(&irisFace, irisWifi.getPh3b3Host(), irisWifi.getPh3b3Port());
}

void loop() {
    M5.update();

    if (WiFi.status() != WL_CONNECTED) {
        if (irisFace.getState() != IrisState::WIFI_CONNECTING)
            irisFace.setState(IrisState::WIFI_CONNECTING);
    } else {
        irisPh3b3.update();
    }

    irisFace.update();
    delay(20);
}
