/*
 * Iris — Ph3b3's face on a new body.
 * M5StickS3 (ESP32-S3), Milestone 2: SVG-faithful face, animation, speech bubble.
 *
 * Build:  arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 ~/Arduino/Iris
 * Flash:  arduino-cli upload  --fqbn m5stack:esp32:m5stack_sticks3 -p /dev/ttyACM0 ~/Arduino/Iris
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_sleep.h>

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

// Show the sleeping face for 2 s so the user sees WHY she went dark, then
// arm both the 5-min timer (mandatory auto-retry) and BtnA ext1 wake (bonus
// manual override), cut display backlight + WiFi radio, and enter deep sleep.
// esp_deep_sleep_start() never returns — next execution is setup() on wake.
static void goToSleep(const char* reason) {
    irisFace.setState(IrisState::SLEEPING, reason);
    faceDelay(2000);
    WiFi.mode(WIFI_OFF);
    M5.Speaker.end();
    M5.Display.setBrightness(0);
    esp_sleep_enable_timer_wakeup(SLEEP_RETRY_US);
    esp_sleep_enable_ext1_wakeup(1ULL << SLEEP_WAKE_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

static const int PTT_RATE = 16000;
static const int PTT_MAX  = PTT_RATE * 12;  // 12 s @ 16 kHz

void setup() {
    // Cache reset reason before anything can change it.
    esp_reset_reason_t _lastReset = esp_reset_reason();

    Serial.begin(115200);

    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Speaker.begin();
    M5.Speaker.setVolume(200);

    irisFace.begin();

    // Show why we last rebooted — critical for diagnosing crash/brownout/WDT loops.
    char rrBuf[20];
    switch (_lastReset) {
        case ESP_RST_BROWNOUT: strcpy(rrBuf, "RST:BROWNOUT"); break;
        case ESP_RST_PANIC:    strcpy(rrBuf, "RST:PANIC");    break;
        case ESP_RST_WDT:      strcpy(rrBuf, "RST:WDT");      break;
        case ESP_RST_TASK_WDT: strcpy(rrBuf, "RST:TWDT");     break;
        case ESP_RST_INT_WDT:  strcpy(rrBuf, "RST:IWDT");     break;
        case ESP_RST_SW:       strcpy(rrBuf, "RST:SW");        break;
        case ESP_RST_POWERON:  strcpy(rrBuf, "RST:POWERON");   break;
        case ESP_RST_DEEPSLEEP: {
            esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
            strcpy(rrBuf, wc == ESP_SLEEP_WAKEUP_TIMER ? "WAKE:TIMER" : "WAKE:BTN");
            break;
        }
        default: snprintf(rrBuf, sizeof(rrBuf), "RST:%d", (int)_lastReset); break;
    }
    irisFace.setState(IrisState::BOOT, rrBuf);

    // Boot screen: 6s window. Any BtnA tap → clear WiFi creds and re-run portal.
    // No hold timing: just tap and release. AXP2101 still cuts power at 3s+ hold
    // (hardware, can't override) — a quick tap is nowhere near that threshold.
    // If BtnA was already held at power-on, drain it first; only count fresh presses.
    {
        bool     done     = false;
        uint32_t deadline = millis() + 6000UL;

        M5.update();
        bool bootHeld = M5.BtnA.isPressed();  // absorb power-on button press

        irisFace.setStatusLine("tap A: reset WiFi");
        irisFace.update();

        while (!done && millis() < deadline) {
            M5.update();
            irisFace.update();

            if (bootHeld) {
                if (!M5.BtnA.isPressed()) bootHeld = false;  // wait for power-on release
            } else if (M5.BtnA.wasReleased()) {
                irisWifi.clearWifiCreds();
                irisFace.setStatusLine("WiFi creds cleared");
                irisFace.update();
                delay(1500);
                done = true;
            }
            delay(20);
        }
    }

    irisWifi.begin(&irisFace);

    if (!irisWifi.isConnected()) {
        goToSleep("no WiFi - sleeping");
    }

    irisFace.setState(IrisState::PH3B3_SEARCHING);
    irisPh3b3.begin(&irisFace, irisWifi.getPh3b3Host(), irisWifi.getPh3b3Port());
}

void loop() {
    M5.update();

    static uint32_t sBtnBDownAt = 0;
    if (M5.BtnB.wasPressed())  sBtnBDownAt = millis();
    if (M5.BtnB.isPressed() && sBtnBDownAt && (millis() - sBtnBDownAt) >= 1000)
        M5.Power.powerOff();

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

    // ── WiFi watchdog + reconnect supervisor ──────────────────────────────────
    static uint32_t sLastReconnectMs = 0;
    static int      sReconnectFail   = 0;
    static const int RECONNECT_HARD  = 8;

    if (WiFi.status() != WL_CONNECTED) {
        if (irisFace.getState() != IrisState::WIFI_CONNECTING)
            irisFace.setState(IrisState::WIFI_CONNECTING);

        uint32_t now2    = millis();
        uint32_t interval = (sReconnectFail >= RECONNECT_HARD) ? 60000U : 15000U;
        if (now2 - sLastReconnectMs >= interval) {
            sLastReconnectMs = now2;
            if (ESP.getMaxAllocHeap() >= 80000) {
                irisWifi.reconnect();
                sReconnectFail++;
            } else {
                WiFi.mode(WIFI_OFF);
                delay(100);
                WiFi.mode(WIFI_STA);
            }
        }
    } else {
        sReconnectFail = 0;

        // ── Ph3b3 boot timeout ────────────────────────────────────────────────
        // Armed once on first WiFi connect. If Ph3b3 never responds healthy
        // within WIFI_PH3B3_TIMEOUT_MS, deep-sleep and retry in 5 min.
        // Cancelled permanently by sEverHealthy — never fires after a good
        // connection, even if Ph3b3 later goes down. Portal path is exempt:
        // this block only runs when WL_CONNECTED, which the portal never is.
        static bool     sEverHealthy  = false;
        static uint32_t sPh3bDeadline = 0;
        static bool     sSleepArmed   = false;

        if (!sEverHealthy) {
            if (irisFace.getState() == IrisState::PH3B3_HEALTHY) {
                sEverHealthy = true;
                sSleepArmed  = false;
            } else {
                if (!sSleepArmed) {
                    sPh3bDeadline = millis() + WIFI_PH3B3_TIMEOUT_MS;
                    sSleepArmed   = true;
                } else if (millis() >= sPh3bDeadline) {
                    goToSleep("no Ph3b3 - sleeping");
                }
            }
        }

        irisPh3b3.update();
    }

    irisFace.update();
    delay(20);
}
