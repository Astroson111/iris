# Iris — Ph3b3's Face on a New Body

Iris is the embedded companion device for Ph3b3, a local AI assistant built on ESP32-S3.
She sits on a desk, displays an avatar face, and tells you — just by looking at her — whether
Ph3b3 is online and ready.  No app, no dashboard: her expression IS the status readout.

**Ecosystem:** [Ph3b3](https://github.com/astroson111/ph3b3) — the local server (STT · reasoning ·
TTS) · [Dio / Dionysus](https://github.com/astroson111/Dionysus) — the Stack-Chan desktop body ·
[Control Panel](https://github.com/astroson111/ph3b3/tree/main/static) — the local web PWA (served
by Ph3b3) · Iris (this repo). All local, all clients of the same server.

---

## Hardware

| Part | Notes |
|---|---|
| **M5StickS3** | ESP32-S3-PICO, 8 MB flash / 8 MB PSRAM |
| ST7789 LCD | 135 × 240 px, portrait, built-in |
| USB-C | Power + flash (native USB CDC, enumerates as /dev/ttyACM0) |

No external components required for Milestone 1.

---

## How the M5Stack controller is used

- **Display** — M5Unified drives the ST7789 at 135 × 240 portrait.  The top 200 px are owned by
  the avatar sprite (M5Stack_Avatar library, GirlyEye style, custom portrait layout).  The bottom
  40 px form a status band for one-line text that Iris's avatar task cannot overwrite.
- **NVS (Preferences)** — Ph3b3 host + port are persisted across reboots without touching the
  filesystem.  WiFiManager stores WiFi credentials in the ESP32 WiFi NVS partition automatically.
- **WiFi** — Station mode for normal operation; SoftAP for the config portal.
- **ESP32-S3 native USB CDC** — Serial debug at 115200 baud.

---

## Libraries

| Library | Version | Source |
|---|---|---|
| M5Unified | 0.2.17 | arduino-cli lib install |
| M5GFX | 0.2.23 | (M5Unified dependency) |
| M5Stack_Avatar | 0.10.0 | arduino-cli lib install "M5Stack_Avatar" |
| WiFiManager | 2.0.17 | arduino-cli lib install "WiFiManager" |
| ESPmDNS, WiFiClientSecure, HTTPClient, Preferences | — | bundled with m5stack:esp32 3.3.7 |

---

## Build

```bash
# Prereqs (one-time)
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
~/.local/bin/arduino-cli config init
~/.local/bin/arduino-cli config add board_manager.additional_urls \
    https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
~/.local/bin/arduino-cli core update-index
~/.local/bin/arduino-cli core install m5stack:esp32
~/.local/bin/arduino-cli lib install M5Unified M5GFX "M5Stack_Avatar" "WiFiManager" M5UnitAudioPlayer

# Compile
~/.local/bin/arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 ~/Arduino/Iris
```

---

## Flash

Hold the side button for ~2 seconds until the green LED blinks (download mode), then:

```bash
~/.local/bin/arduino-cli upload \
    --fqbn m5stack:esp32:m5stack_sticks3 \
    -p /dev/ttyACM0 \
    ~/Arduino/Iris
```

---

## Boot → Provision → Discover → Connected

**Boot** — Iris wakes with a sleepy expression while M5Unified initialises the display and
the avatar sprite is created.

**WiFi** — She immediately tries to connect using credentials saved from the last session
(stored in ESP32 NVS).  Her face shifts to *doubt* while connecting.  If no credentials
exist, or the connection fails after 15 seconds, she opens a SoftAP called **Iris-Setup**
and starts a captive web portal; her face goes *neutral* and the status band shows the
portal IP (`192.168.4.1`).  Connect your phone or laptop to Iris-Setup, navigate to that
address, pick your WiFi network, enter the password, optionally update the Ph3b3 host
and port, and set the **Volume** and **Microphone** levels (see below).  Hit Save.  Iris
reconnects and the portal closes.  Credentials, Ph3b3 address, and audio presets are saved
to NVS for all future boots.

**Ph3b3 discovery** — Once on WiFi, Iris calls `WiFi.hostByName("ph3b3.local")` to resolve
Ph3b3's address via mDNS.  If mDNS fails, she falls back to the configured host (default
`192.168.0.16:7331`).  She also registers herself on the network as `iris.local`.

**Health loop** — Every 10 seconds Iris makes a HTTPS GET to Ph3b3's `/health` endpoint,
using `WiFiClientSecure` with `setInsecure()` (self-signed cert) and HTTP Basic auth.
A 2xx response makes her face go **happy**.  Any failure flips her to **sad**.  If WiFi
drops between polls she shows *doubt* until the ESP32's auto-reconnect kicks in.

---

## Audio settings

Set in the **Iris-Setup** portal (set-and-forget — no runtime buttons or gestures). Both are
three-way presets stored as a preset index in NVS (namespace `iris`, keys `vol` / `mic`),
read and applied at boot. Re-entering setup shows the currently stored values. Fresh
enrollment defaults to **Medium** for both — she never boots silent, blasting, or deaf.

| Field | Presets | Effect |
|---|---|---|
| **Volume** | Low 40% · **Medium 70%** · High 100% | Scales `M5.Speaker` output. Iris plays Ph3b3's TTS **locally** on-device (the `/chat` audio reply), so volume is enforced on the ESP32 — not on the server. |
| **Microphone** | Low · **Medium** · High | Mic capture magnification (8 / 16 / 32; **Medium = 16**, the tuned default). Applied to the PTT capture before audio is streamed to Whisper on the server. |

Notes:
- **Medium mic = prior behavior exactly** (16) — it's the calibration anchor.
- The server does no input normalization before Whisper (only an rms silence gate), so device gain
  reaches Whisper directly. Keep **Low** loud enough to clear that gate at arm's length.
- Endpoint tuning (`VAD_SILENCE_MS`) was done at Medium — it is **not** retuned per preset. If a
  higher mic level ever holds sessions open on room noise, lower the High multiplier instead.

---

## Face state reference

| Iris looks… | Means |
|---|---|
| Sleepy | Booting |
| Doubt | Connecting to WiFi (or WiFi dropped) |
| Neutral + "Iris-Setup" speech | Config portal open — connect and provision |
| Neutral + "Finding Ph3b3" | WiFi up, first health check in progress |
| **Happy** | Ph3b3 is online and answering |
| **Sad** | Ph3b3 unreachable (down, wrong address, or auth failed) |

---

## Tuning (config.h)

| Constant | Default | Purpose |
|---|---|---|
| `PH3B3_FALLBACK_HOST` | `192.168.0.16` | Ph3b3 IP if mDNS fails and NVS is empty |
| `PH3B3_FALLBACK_PORT` | `7331` | Ph3b3 port |
| `PH3B3_POLL_MS` | `10000` | Health-check interval (ms) |
| `PH3B3_HTTP_TIMEOUT_MS` | `8000` | Per-request HTTP timeout (covers TLS handshake) |
| `PORTAL_TIMEOUT_S` | `180` | Config portal auto-closes after this many seconds |
| `IRIS_AP_SSID` | `Iris-Setup` | SoftAP name shown during provisioning |
