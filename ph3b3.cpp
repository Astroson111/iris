#include "ph3b3.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include "config.h"

// ─── Base64 helpers (PTT audio encode/decode) ────────────────────────────────

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static const char B64ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void buildWavHeader(uint8_t* h, int samples, int rate) {
    int d = samples * 2, f = 36 + d, br = rate * 2;
    memcpy(h, "RIFF", 4); h[4]=f;h[5]=f>>8;h[6]=f>>16;h[7]=f>>24;
    memcpy(h+8, "WAVE", 4); memcpy(h+12, "fmt ", 4);
    h[16]=16;h[17]=0;h[18]=0;h[19]=0;
    h[20]=1;h[21]=0; h[22]=1;h[23]=0;
    h[24]=rate;h[25]=rate>>8;h[26]=rate>>16;h[27]=rate>>24;
    h[28]=br;h[29]=br>>8;h[30]=br>>16;h[31]=br>>24;
    h[32]=2;h[33]=0; h[34]=16;h[35]=0;
    memcpy(h+36, "data", 4); h[40]=d;h[41]=d>>8;h[42]=d>>16;h[43]=d>>24;
}

// Large static buffers (BSS, not stack) shared by doChat / doPtt.
static char    s_peek[6145];    // first 6KB of /chat response — captures full JSON header
static int16_t s_pcm[2][1024];  // double-buffer for streaming PCM playback

void IrisPh3b3::begin(IrisFace* face, const String& host, int port, const String& authPass) {
    _face = face;
    _host = host;
    _port = port;
    // Per-device auth key from NVS (set in the setup portal). Empty → baked
    // PH3B3_AUTH_PASS, which the server grandfathered as this device's key.
    _authPass = authPass.length() ? authPass : String(PH3B3_AUTH_PASS);
    _sessionId = "iris-" + String(esp_random(), HEX);
    _lastMs = millis() - PH3B3_POLL_MS;   // trigger first check immediately
    // STEP 0 visibility (serial is dead): show the active target on boot so the
    // screen reveals exactly which host:port she is dialing.
    if (_face) _face->setStatusLine((_host + ":" + String(_port)).c_str());
}

void IrisPh3b3::update() {
    // Clear greeting bubble after timeout (runs every loop, independent of poll interval)
    if (_greetClearMs > 0 && millis() >= _greetClearMs) {
        _face->clearBubble();
        _greetClearMs = 0;
    }

    // Argus fleet heartbeat — its own cadence, independent of the health-poll gate.
    if (millis() - _hbLastMs >= ARGUS_HEARTBEAT_MS) {
        _hbLastMs = millis();
        _sendHeartbeat();
    }

    if (millis() - _lastMs < PH3B3_POLL_MS) return;
    _lastMs = millis();

    int code = _checkHealth();
    if (code >= 200 && code < 300) {
        _face->setState(IrisState::PH3B3_HEALTHY);
        if (!_greetedOnce) {
            _face->setBubble("Hello! Ph3b3 is online and ready.");
            _greetedOnce = true;
            _greetClearMs = millis() + 5000;
        }
    } else {
        // Cause-visible status (serial is dead, screen is the instrument):
        //   "denied"   → reached Nyx but auth rejected (401/403)
        //   "no route" → never reached the host (connect/TLS/timeout, code < 0)
        //   "away H:P" → other non-2xx; keep host:port for the generic case
        String line;
        if (code == 401 || code == 403) line = "denied";
        else if (code < 0)              line = "no route";
        else                            line = "away " + _host + ":" + String(_port);
        _face->setState(IrisState::PH3B3_UNREACHABLE, line.c_str());
        _face->clearBubble();
        _greetedOnce = false;
        _greetClearMs = 0;
    }
}

// ─── Argus heartbeat ──────────────────────────────────────────────────────────
// Tiny status POST to /argus/heartbeat, riding the verified check-in path (Basic
// auth + X-Ph3b3-Device: iris — the server IP-pins only Dio, and trusts Iris via
// the shared cred). Fire-and-forget: any failure is silently ignored so a flaky
// network never stalls the UI. Payload stays well under 200 B.
void IrisPh3b3::_sendHeartbeat() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/argus/heartbeat";
    if (!http.begin(tls, url)) return;
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    http.setAuthorization(PH3B3_AUTH_USER, _authPass.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ph3b3-Device", "iris");
    char body[192];
    int n = snprintf(body, sizeof(body),
        "{\"battery\":%d,\"rssi\":%d,\"uptime\":%lu,\"firmware_hash\":\"%s\",\"free_heap\":%u}",
        (int)M5.Power.getBatteryLevel(), (int)WiFi.RSSI(),
        (unsigned long)(millis() / 1000UL), ARGUS_FW_HASH, (unsigned)ESP.getFreeHeap());
    http.POST((uint8_t*)body, n);
    http.end();
}

// ─── doChat ──────────────────────────────────────────────────────────────────
// Blocking: POST /chat, display response text in bubble, stream + play TTS audio.

void IrisPh3b3::doChat(const String& message) {
    _face->setState(IrisState::THINKING);
    _face->update();

    // Chunked TTS consumer (ported from Dio). POST /chat/stream for a manifest
    // {stream_id, chunk_count, text, audio(chunk0), response}, then lazily GET
    // /tts/chunk/{sid}/{n} for the rest — each a SHORT request the server
    // synthesises on demand (with read-ahead). Replaces the old monolithic /chat:
    // its single multi-MB WAV got torn down mid-send when Iris read it at playback
    // speed, so long stories cut off. Bounded per-chunk requests never do.
    WiFiClientSecure tls;
    tls.setInsecure();
    tls.setTimeout(30);              // per-recv floor (s); the watchdog is the real control
    HTTPClient http;
    http.setReuse(true);             // keep-alive socket reused across manifest + chunk GETs
    String base = "https://" + _host + ":" + String(_port);

    // ── Shared decode/playback state (persists across chunks → gapless queue) ──
    int   fillIdx       = 0;
    int   chunkPos      = 0;
    int   wavHdrSkipped = 0;
    uint8_t halfLo      = 0;
    bool  halfReady     = false;
    char  b4[4]; int b4pos = 0;
    bool  keepGoing     = true;      // false at end of ONE chunk's audio field
    bool  streamAbort   = false;     // barge-in / watchdog → stop the whole reply

    auto flushChunk = [&]() {
        if (chunkPos == 0) return;
        while (M5.Speaker.isPlaying()) delay(1);
        if (streamAbort) return;     // barge-in/watchdog only; gating on !keepGoing
        M5.Speaker.playRaw(s_pcm[fillIdx], chunkPos, 22050, false, 1, 0);
        fillIdx ^= 1;
        chunkPos = 0;
    };

    auto pushByte = [&](uint8_t b) {
        if (wavHdrSkipped++ < 44) return;   // skip THIS chunk's 44-byte WAV header
        if (!halfReady) { halfLo = b; halfReady = true; return; }
        s_pcm[fillIdx][chunkPos++] = (int16_t)((b << 8) | halfLo);
        halfReady = false;
        if (chunkPos == 1024) flushChunk();
    };

    auto feedCh = [&](char ch) {
        if (!keepGoing) return;
        if (ch == '"') { keepGoing = false; return; }
        int v = b64val(ch);
        if (v < 0) return;
        b4[b4pos++] = ch;
        if (b4pos == 4) {
            int v0=b64val(b4[0]), v1=b64val(b4[1]), v2=b64val(b4[2]), v3=b64val(b4[3]);
            if (v0 >= 0 && v1 >= 0) {
                pushByte((uint8_t)((v0<<2)|(v1>>4)));
                if (v2 >= 0 && b4[2] != '=') {
                    pushByte((uint8_t)((v1<<4)|(v2>>2)));
                    if (v3 >= 0 && b4[3] != '=') pushByte((uint8_t)((v2<<6)|v3));
                }
            }
            b4pos = 0;
        }
    };

    // Read a response head into s_peek until "audio":" is buffered. Inter-byte
    // watchdog (reset on bytes, give up after PH3B3_RX_SILENCE_FIRST_MS of dead
    // air) — never a wall-clock cap. Returns bytes buffered.
    auto fillPeek = [&]() -> int {
        auto* raw = http.getStreamPtr();
        raw->setTimeout(30000);
        int bodyTotal = http.getSize();
        int pl = 0;
        uint32_t lastRxMs = millis();
        while (pl < (int)(sizeof(s_peek) - 1)) {
            int avail = raw->available();
            if (avail > 0) {
                int n = min(avail, (int)(sizeof(s_peek) - 1) - pl);
                pl += raw->readBytes(s_peek + pl, n);
                s_peek[pl] = '\0';
                lastRxMs = millis();
                if (pl > 20 && strstr(s_peek, "\"audio\":\"")) break;
                if (bodyTotal > 0 && pl >= bodyTotal) break;
            } else if (!raw->connected()) {
                break;
            } else if (bodyTotal > 0 && pl >= bodyTotal) {
                break;
            } else if (millis() - lastRxMs >= PH3B3_RX_SILENCE_FIRST_MS) {
                break;
            } else {
                _face->update();
                delay(10);
            }
        }
        s_peek[pl] = '\0';
        return pl;
    };

    // Decode + play ONE chunk's base64 "audio" field. s_peek[audioStart..peekLen)
    // is the already-read head; then drain the live stream to the field's closing
    // quote / barge-in / no-progress watchdog. Reads one byte per turn so the face
    // ticks every ~33 ms and playback (flushChunk) paces the read. Completion of a
    // chunk = its closing quote; the watchdog only fires on a genuine stall.
    auto playField = [&](int peekLen, int audioStart) {
        wavHdrSkipped = 0; halfReady = false; b4pos = 0; keepGoing = true;
        auto* raw = http.getStreamPtr();
        for (int i = audioStart; i < peekLen && keepGoing; i++) feedCh(s_peek[i]);
        uint32_t lastRxMs = millis(), nextFaceMs = millis() + 33;
        while (keepGoing) {
            M5.update();
            if (M5.BtnA.wasPressed()) { M5.Speaker.stop(); keepGoing = false; streamAbort = true; break; }
            if (millis() >= nextFaceMs) {
                _face->setSpeakingLevel(0.5f);
                _face->update();
                nextFaceMs = millis() + 33;
            }
            int c = raw->read();
            if (c < 0) {
                if (!raw->connected() && raw->available() == 0) break;  // chunk stream closed
                if (M5.Speaker.isPlaying()) { lastRxMs = millis(); }    // audio still draining = progress
                else if (millis() - lastRxMs > PH3B3_RX_SILENCE_STREAM_MS) {
                    _face->setStatusLine("stream stalled");
                    keepGoing = false; streamAbort = true;
                    break;
                }
                delay(1);
                continue;
            }
            lastRxMs = millis();   // byte received → reset the silence watchdog
            feedCh((char)c);
        }
        flushChunk();
    };

    // Drain trailing body bytes (the small tail after a chunk's audio field, or
    // the manifest's "response") so the reused keep-alive socket is clean.
    auto drainTail = [&]() {
        auto* raw = http.getStreamPtr();
        uint32_t idle = millis();
        while (millis() - idle < 60) {
            if (raw->available()) { raw->read(); idle = millis(); }
            else delay(2);
        }
    };

    // ── Manifest: POST /chat/stream ──
    if (!http.begin(tls, base + "/chat/stream")) {
        _face->setState(IrisState::PH3B3_UNREACHABLE);
        return;
    }
    http.setConnectTimeout(20000);
    http.setTimeout(60000);
    http.setAuthorization(PH3B3_AUTH_USER, _authPass.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ph3b3-Device", "iris");

    JsonDocument body;
    body["message"]    = message;
    body["session_id"] = _sessionId;
    String payload;
    serializeJson(body, payload);

    _face->setStatusLine("asking ph3b3...");
    _face->update();
    int code = http.POST(payload);
    if (code != HTTP_CODE_OK) {
        http.end();
        _face->setState(IrisState::PH3B3_HEALTHY);
        _face->setStatusLine(("http " + String(code)).c_str());
        _face->update();
        return;
    }

    // Manifest: stream_id + chunk_count + chunk 0's text & audio. These keys
    // precede "audio"/"response" in the JSON, so they're always inside the 6 KB
    // head regardless of reply length.
    int peekLen = fillPeek();
    JsonDocument filter;
    filter["stream_id"]   = true;
    filter["chunk_count"] = true;
    filter["text"]        = true;
    JsonDocument jdoc;
    deserializeJson(jdoc, s_peek, peekLen, DeserializationOption::Filter(filter));
    const char* sid = jdoc["stream_id"];
    String streamId = (sid && *sid) ? String(sid) : "";
    int chunkCount  = jdoc["chunk_count"] | 0;
    const char* t0  = jdoc["text"];
    String chunk0Text = (t0 && *t0) ? String(t0) : "";

    char* foundTag = strstr(s_peek, "\"audio\":\"");
    int audioStart = foundTag ? (int)(foundTag - s_peek) + 9 : -1;   // 9 = strlen("\"audio\":\"")

    if (chunkCount > 0 && audioStart >= 0) {
        _face->setState(IrisState::SPEAKING);
        _face->setStatusLine("ph3b3 says:");
        if (chunk0Text.length()) _face->setBubble(chunk0Text);
        _face->update();
        playField(peekLen, audioStart);          // chunk 0 (carried in the manifest)
        drainTail();
        http.end();

        // ── Chunks 1..N-1: lazy GET, caption + play each ──
        for (int n = 1; n < chunkCount && !streamAbort; n++) {
            String path = base + "/tts/chunk/" + streamId + "/" + String(n);
            if (!http.begin(tls, path)) break;
            http.setConnectTimeout(20000);
            http.setTimeout(60000);
            http.setAuthorization(PH3B3_AUTH_USER, _authPass.c_str());
            http.addHeader("X-Ph3b3-Device", "iris");
            int gc = http.GET();
            if (gc != HTTP_CODE_OK) { http.end(); break; }
            int cpl = fillPeek();
            JsonDocument cfilter; cfilter["text"] = true;
            JsonDocument cjdoc;
            deserializeJson(cjdoc, s_peek, cpl, DeserializationOption::Filter(cfilter));
            const char* ctx = cjdoc["text"];
            char* ct = strstr(s_peek, "\"audio\":\"");
            int cAudio = ct ? (int)(ct - s_peek) + 9 : -1;
            if (cAudio < 0) { http.end(); break; }
            if (ctx && *ctx) _face->setBubble(String(ctx));   // caption follows the voice
            playField(cpl, cAudio);
            drainTail();
            http.end();
        }

        // Completion = playback buffer drained (NOT stream close). Barge-in honoured.
        while (M5.Speaker.isPlaying()) {
            M5.update();
            if (M5.BtnA.wasPressed()) { M5.Speaker.stop(); break; }
            _face->setSpeakingLevel(0.5f);
            _face->update();
            delay(50);
        }
    } else {
        http.end();
    }

    _face->setSpeakingLevel(0.0f);
    _face->clearBubble();
    _face->setState(IrisState::PH3B3_HEALTHY);
    _face->setStatusLine("Ph3b3 online");
}

// ─── doPtt ───────────────────────────────────────────────────────────────────
// Blocking: encode raw PCM to base64 WAV, POST /transcribe, then doChat().

void IrisPh3b3::doPtt(int16_t** ppAudio, int numSamples) {
    _face->setState(IrisState::THINKING);
    _face->setStatusLine("transcribing...");
    _face->update();

    int16_t* audio = *ppAudio;

    // Build WAV header + base64-encode everything into a PSRAM buffer.
    uint8_t wavHdr[44];
    buildWavHeader(wavHdr, numSamples, 16000);
    int wavBytes = 44 + numSamples * 2;
    int b64Len   = ((wavBytes + 2) / 3) * 4;
    int jLen     = 10 + b64Len + 2;            // {"audio":"..."}
    char* jbuf   = (char*)heap_caps_malloc(jLen + 1, MALLOC_CAP_SPIRAM);
    if (!jbuf) {
        _face->setState(IrisState::PH3B3_HEALTHY);
        _face->setStatusLine("OOM-PTT");
        return;
    }

    memcpy(jbuf, "{\"audio\":\"", 10);
    int pos = 10;

    uint8_t tri[3]; int triPos = 0;
    auto flushTri = [&](int validBytes) {
        while (triPos < 3) tri[triPos++] = 0;
        jbuf[pos++] = B64ENC[(tri[0]>>2)&0x3F];
        jbuf[pos++] = B64ENC[((tri[0]&3)<<4)|((tri[1]>>4)&0xF)];
        jbuf[pos++] = validBytes < 2 ? '=' : B64ENC[((tri[1]&0xF)<<2)|((tri[2]>>6)&0x3)];
        jbuf[pos++] = validBytes < 3 ? '=' : B64ENC[tri[2]&0x3F];
        triPos = 0;
    };
    auto feedB = [&](uint8_t b) {
        tri[triPos++] = b;
        if (triPos == 3) flushTri(3);
    };

    for (int i = 0; i < 44; i++) feedB(wavHdr[i]);
    for (int i = 0; i < numSamples; i++) {
        feedB((uint8_t)(audio[i] & 0xFF));
        feedB((uint8_t)((audio[i] >> 8) & 0xFF));
    }
    if (triPos > 0) flushTri(triPos);

    jbuf[pos++] = '"'; jbuf[pos++] = '}'; jbuf[pos] = '\0';

    // Free the big PTT buffer before opening TLS (saves ~200KB peak).
    free(*ppAudio); *ppAudio = nullptr;

    _face->setStatusLine("sending...");
    _face->update();

    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/transcribe";
    if (!http.begin(tls, url)) {
        free(jbuf);
        _face->setState(IrisState::PH3B3_HEALTHY);
        _face->setStatusLine("transcribe failed");
        return;
    }
    http.setConnectTimeout(20000);
    http.setTimeout(20000);
    http.setAuthorization(PH3B3_AUTH_USER, _authPass.c_str());
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Ph3b3-Device", "iris");

    int code = http.POST((uint8_t*)jbuf, pos);
    free(jbuf); jbuf = nullptr;

    String heard;
    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        JsonDocument doc;
        deserializeJson(doc, body);
        const char* t = doc["text"];
        if (t && *t) heard = String(t);
    }
    http.end();

    if (heard.length() == 0) {
        _face->setState(IrisState::PH3B3_HEALTHY);
        _face->setStatusLine("didn't catch that");
        return;
    }

    // Flash "You: ..." briefly so the user knows what was heard.
    _face->setBubble("You: " + heard);
    _face->update();
    delay(700);
    _face->clearBubble();

    doChat(heard);
}

// ─────────────────────────────────────────────────────────────────────────────
int IrisPh3b3::_checkHealth() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/health";

    if (!http.begin(client, url)) return -1000;   // begin failed → no route

    // setConnectTimeout sets SO_RCVTIMEO on the socket; without it the default
    // 5 s can race the TLS handshake. http.setTimeout() alone is not enough.
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setAuthorization(PH3B3_AUTH_USER, _authPass.c_str());

    int code = http.GET();
    http.end();
    return code;   // 2xx healthy; 401/403 denied; <0 connection failure
}
