#include "ph3b3.h"
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

void IrisPh3b3::begin(IrisFace* face, const String& host, int port) {
    _face = face;
    _host = host;
    _port = port;
    _sessionId = "iris-" + String(esp_random(), HEX);
    _lastMs = millis() - PH3B3_POLL_MS;   // trigger first check immediately
}

void IrisPh3b3::update() {
    // Clear greeting bubble after timeout (runs every loop, independent of poll interval)
    if (_greetClearMs > 0 && millis() >= _greetClearMs) {
        _face->clearBubble();
        _greetClearMs = 0;
    }

    if (millis() - _lastMs < PH3B3_POLL_MS) return;
    _lastMs = millis();

    if (_checkHealth()) {
        _face->setState(IrisState::PH3B3_HEALTHY);
        if (!_greetedOnce) {
            _face->setBubble("Hello! Ph3b3 is online and ready.");
            _greetedOnce = true;
            _greetClearMs = millis() + 5000;
        }
    } else {
        _face->setState(IrisState::PH3B3_UNREACHABLE);
        _face->clearBubble();
        _greetedOnce = false;
        _greetClearMs = 0;
    }
}

// ─── doChat ──────────────────────────────────────────────────────────────────
// Blocking: POST /chat, display response text in bubble, stream + play TTS audio.

void IrisPh3b3::doChat(const String& message) {
    _face->setState(IrisState::THINKING);
    _face->update();

    WiFiClientSecure tls;
    tls.setInsecure();
    // WiFiClientSecure.setTimeout() takes SECONDS (not ms) and sets SO_RCVTIMEO.
    // Without this, the default socket timeout is ~5 s — far too short for LLM+TTS.
    tls.setTimeout(90);
    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/chat";
    if (!http.begin(tls, url)) {
        _face->setState(IrisState::PH3B3_UNREACHABLE);
        return;
    }
    http.setConnectTimeout(90000);
    http.setTimeout(120000);
    http.setAuthorization(PH3B3_AUTH_USER, PH3B3_AUTH_PASS);
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

    // Read up to 6KB from the stream, polling so the face stays alive.
    // readBytes() alone can stall if the response is smaller than the buffer.
    auto* raw = http.getStreamPtr();
    raw->setTimeout(30000);
    int peekLen = 0;
    uint32_t peekDeadline = millis() + 60000;
    while (peekLen < (int)(sizeof(s_peek) - 1) && millis() < peekDeadline) {
        int avail = raw->available();
        if (avail > 0) {
            int n = min(avail, (int)(sizeof(s_peek) - 1) - peekLen);
            peekLen += raw->readBytes(s_peek + peekLen, n);
            s_peek[peekLen] = '\0';
            // Stop early once we've captured the audio tag — we have everything we need.
            if (peekLen > 50 && strstr(s_peek, "\"audio\":\"")) break;
        } else if (!raw->connected()) {
            break;
        } else {
            _face->update();
            delay(10);
        }
    }
    s_peek[peekLen] = '\0';

    // Extract response text with ArduinoJson filter (cheap — only parses "response").
    String responseText;
    {
        JsonDocument filter; filter["response"] = true;
        JsonDocument doc;
        deserializeJson(doc, s_peek, peekLen, DeserializationOption::Filter(filter));
        const char* r = doc["response"];
        if (r && *r) responseText = String(r);
    }

    if (responseText.length() > 0) {
        _face->setState(IrisState::SPEAKING);
        _face->setStatusLine("ph3b3 says:");
        _face->setBubble(responseText);
        _face->update();
    }

    // Locate start of "audio" base64 string in peek buffer.
    const char* audioTag = "\"audio\":\"";
    char* foundAudio = strstr(s_peek, audioTag);
    int audioStart = foundAudio ? (int)(foundAudio - s_peek) + (int)strlen(audioTag) : -1;

    if (audioStart >= 0) {
        int   fillIdx       = 0;
        int   chunkPos      = 0;
        int   wavHdrSkipped = 0;
        uint8_t halfLo      = 0;
        bool  halfReady     = false;
        char  b4[4]; int b4pos = 0;
        bool  keepGoing     = true;

        auto flushChunk = [&]() {
            if (chunkPos == 0) return;
            while (M5.Speaker.isPlaying()) delay(1);
            if (!keepGoing) return;
            M5.Speaker.playRaw(s_pcm[fillIdx], chunkPos, 22050, false, 1, 0);
            fillIdx ^= 1;
            chunkPos = 0;
        };

        auto pushByte = [&](uint8_t b) {
            if (wavHdrSkipped++ < 44) return;
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

        for (int i = audioStart; i < peekLen; i++) feedCh(s_peek[i]);

        if (keepGoing) {
            uint32_t deadline  = millis() + 90000;
            uint32_t nextFaceMs = millis() + 33;
            while (keepGoing && millis() < deadline) {
                M5.update();
                if (M5.BtnA.wasPressed()) { M5.Speaker.stop(); keepGoing = false; break; }
                if (millis() >= nextFaceMs) {
                    _face->setSpeakingLevel(0.5f);
                    _face->update();
                    nextFaceMs = millis() + 33;
                }
                int c = raw->read();
                if (c < 0) { delay(1); continue; }
                feedCh((char)c);
            }
        }

        flushChunk();
        http.end();

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
    http.setAuthorization(PH3B3_AUTH_USER, PH3B3_AUTH_PASS);
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
bool IrisPh3b3::_checkHealth() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = "https://" + _host + ":" + String(_port) + "/health";

    if (!http.begin(client, url)) return false;

    // setConnectTimeout sets SO_RCVTIMEO on the socket; without it the default
    // 5 s can race the TLS handshake. http.setTimeout() alone is not enough.
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setAuthorization(PH3B3_AUTH_USER, PH3B3_AUTH_PASS);

    int code = http.GET();
    http.end();
    return (code >= 200 && code < 300);
}
