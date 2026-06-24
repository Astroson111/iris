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

static const int PTT_RATE = 16000;
static const int PTT_MAX  = PTT_RATE * 12;  // 12 s @ 16 kHz

void setup() {
    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Speaker.begin();
    M5.Speaker.setVolume(200);

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

    // ── BtnA: hold ≥200ms = PTT voice │ short tap = canned prompt ─────────
    // wasReleased() can be missed if M5.Mic.record() (blocking 32ms) straddles
    // the M5.update() call, so PTT release uses !isPressed() while sPttActive.
    static uint32_t sBtnADownAt = 0;
    static bool     sPttArmed   = false;  // true once 200ms hold threshold passed
    static bool     sPttActive  = false;  // true while mic is recording
    static int16_t* sPttBuf     = nullptr;
    static int      sPttSamples = 0;

    if (WiFi.status() == WL_CONNECTED) {
        // Track press start
        if (M5.BtnA.wasPressed()) {
            sBtnADownAt = millis();
            sPttArmed   = false;
        }

        // Arm mic after 200ms hold
        if (M5.BtnA.isPressed() && !sPttArmed && (millis() - sBtnADownAt) >= 200) {
            sPttArmed   = true;
            sPttSamples = 0;
            sPttBuf = (int16_t*)heap_caps_malloc(PTT_MAX * 2, MALLOC_CAP_SPIRAM);
            if (sPttBuf) {
                sPttActive = true;
                if (M5.Speaker.isPlaying()) M5.Speaker.stop();
                auto mc = M5.Mic.config();
                mc.sample_rate   = PTT_RATE;
                mc.magnification = 16;
                M5.Mic.config(mc);
                M5.Mic.begin();
                irisFace.setState(IrisState::LISTENING);
                irisFace.update();
            }
        }

        // Accumulate samples while held and buffer not full
        if (sPttActive && M5.BtnA.isPressed() && sPttSamples < PTT_MAX) {
            int chunk = min(512, PTT_MAX - sPttSamples);
            M5.Mic.record(&sPttBuf[sPttSamples], chunk, PTT_RATE);
            sPttSamples += chunk;
            irisFace.update();
        }

        // Dispatch when button released OR buffer full
        if (sPttActive && (!M5.BtnA.isPressed() || sPttSamples >= PTT_MAX)) {
            sPttActive = false;
            M5.Mic.end();
            M5.Speaker.end();
            M5.Speaker.begin();
            M5.Speaker.setVolume(200);
            if (sPttBuf && sPttSamples > 0)
                irisPh3b3.doPtt(&sPttBuf, sPttSamples);
            if (sPttBuf) { free(sPttBuf); sPttBuf = nullptr; }
        }

        // Short tap — released before 200ms arm threshold
        if (M5.BtnA.wasReleased() && !sPttArmed && (millis() - sBtnADownAt) < 600) {
            static const char* PROMPTS[] = {
                "Ph3b3, what's on your mind today?",
                "Tell me something interesting.",
                "Any updates for me?",
                "What time is it?",
            };
            static int promptIdx = 0;
            irisPh3b3.doChat(PROMPTS[promptIdx++ % 4]);
        }
    }

    // ── WiFi watchdog ─────────────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
        if (irisFace.getState() != IrisState::WIFI_CONNECTING)
            irisFace.setState(IrisState::WIFI_CONNECTING);
    } else {
        irisPh3b3.update();
    }

    irisFace.update();
    delay(20);
}
