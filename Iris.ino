/*
 * Iris — Ph3b3's face on a new body.
 * M5StickS3 (ESP32-S3), Milestone 1: boot → face → WiFi → Ph3b3 health.
 *
 * Build:  arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 ~/Arduino/Iris
 * Flash:  arduino-cli upload  --fqbn m5stack:esp32:m5stack_sticks3 -p /dev/ttyACM0 ~/Arduino/Iris
 *         (Hold side button ~2 s until green LED blinks to enter download mode.)
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

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);
    // M5Unified auto-detects StickS3 and sets portrait 135×240 orientation.

    // ── Boot: sleepy face while we initialise ─────────────────────────────
    irisFace.begin();
    irisFace.setState(IrisState::BOOT);
    delay(600);  // give Avatar task a moment to render the first frame

    // ── WiFi: try saved creds; fall back to captive portal ───────────────
    // Blocks until connected or portal timeout.  Face transitions to
    // CONFIG_PORTAL if the portal opens, WIFI_CONNECTING otherwise.
    irisWifi.begin(&irisFace);

    if (!irisWifi.isConnected()) {
        // Portal timed out without a connection — reboot and try again.
        irisFace.setState(IrisState::WIFI_CONNECTING);
        irisFace.setStatusLine("No WiFi — rebooting");
        delay(3000);
        ESP.restart();
    }

    // ── Ph3b3 discovery + first health check ─────────────────────────────
    irisFace.setState(IrisState::PH3B3_SEARCHING);
    irisPh3b3.begin(&irisFace, irisWifi.getPh3b3Host(), irisWifi.getPh3b3Port());
    // begin() triggers an immediate health check via update() on the next loop.
}

void loop() {
    M5.update();

    if (WiFi.status() != WL_CONNECTED) {
        // WiFi dropped — show doubt face; ESP32 auto-reconnect is active.
        if (irisFace.getState() != IrisState::WIFI_CONNECTING) {
            irisFace.setState(IrisState::WIFI_CONNECTING);
        }
    } else {
        // Healthy WiFi — run Ph3b3 health checks on schedule.
        irisPh3b3.update();
    }

    delay(100);
}
