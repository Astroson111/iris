#include "face.h"
#include <M5Unified.h>
#include "config.h"

static M5Canvas _cv(&M5.Display);

// ─── Geometry ─────────────────────────────────────────────────────────────────
// SVG viewBox 200×200 → 135×200 face zone: uniform scale 0.675, Y offset +33.
//   screenX(x) = (int)(x * 0.675f)
//   screenY(y) = (int)(y * 0.675f) + 33
static constexpr int FCX   = 67;   // face circle centre X
static constexpr int FCY   = 100;  // face circle centre Y
static constexpr int FCR   = 57;   // face circle radius
static constexpr int GLOWR = 62;   // glow radius
static constexpr int LEX   = 49;   // left  eye cx (viewer's left)
static constexpr int REX   = 86;   // right eye cx
static constexpr int EYY   = 98;   // both eyes cy
static constexpr int ERX   = 14;   // eye rx
static constexpr int ERY   = 16;   // eye ry
static constexpr int MTHY  = 120;  // mouth baseline Y

// ─── Colors ───────────────────────────────────────────────────────────────────
static uint16_t C_FACE, C_GLOW, C_EYE, C_MOUTH, C_WHITE, C_BLK;

// ─── Animation ────────────────────────────────────────────────────────────────
static float    _blink     = 0.0f;
static uint32_t _blinkSt   = 0;
static uint32_t _nextBlink = 0;
static float    _breath    = 0.0f;
static float    _speak     = 0.0f;
static float    _levelVal  = 0.0f;
static uint32_t _levelMs   = 0;
static String   _statusTxt;

// ─── Bubble ───────────────────────────────────────────────────────────────────
static constexpr int _BCOLS = 19;
static constexpr int _BMAX  = 60;
static float    _bProg    = 0.0f;
static bool     _bGrow    = false;
static bool     _bFall    = false;
static uint32_t _bGrowMs  = 0;
static uint32_t _bFallMs  = 0;
static uint8_t  _bStarX[10]   = {};
static uint8_t  _bStarY[10]   = {};
static bool     _bStarBrt[10] = {};
static char     _bLines[_BMAX][20] = {};
static int      _bLnCnt = 0, _bScroll = 0;
static uint32_t _bScrollMs = 0;

// ─── State ────────────────────────────────────────────────────────────────────
static IrisState _fstate = IrisState::BOOT;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static uint16_t dimC(uint16_t col, float k) {
    if (k >= 1.0f) return col;
    if (k <= 0.0f) return 0;
    uint8_t r = (uint8_t)(((col >> 11) & 0x1F) * k);
    uint8_t g = (uint8_t)(((col >> 5)  & 0x3F) * k);
    uint8_t b = (uint8_t)((col         & 0x1F) * k);
    return (r << 11) | (g << 5) | b;
}

// Draws a smile (ry>0) or frown (ry<0) arc as smooth dots along an ellipse.
static void drawMouthArc(int cx, int my, float rx, float ry, uint16_t col, int thick) {
    for (int s = 0; s <= 12; s++) {
        float a  = M_PI * s / 12.0f;
        int   px = cx + (int)(rx * cosf(a));
        int   py = my + (int)(ry * sinf(a));
        _cv.fillSmoothCircle(px, py, thick, col);
    }
}

static void drawBubble() {
    const int TAIL_H  = 8;
    const int tailTY  = MTHY + TAIL_H;
    const int BH_FULL = SCREEN_H - tailTY - 4;
    const int BW_FULL = SCREEN_W - 8;
    const int CR      = 6;

    float p  = _bProg;
    int   bh = max(2, (int)(BH_FULL * p));
    int   bw = max(2, (int)(BW_FULL * p));
    int   bx = FCX - bw / 2;
    int   by = tailTY;

    uint16_t fill = _cv.color565( 14,  6,  38);
    uint16_t rim  = _cv.color565(200, 225, 255);
    uint16_t tc   = _cv.color565(230, 248, 255);
    uint16_t sdim = _cv.color565(100, 120, 200);

    int thw = max(2, (int)(10 * min(1.0f, p * 4.0f)));
    _cv.fillTriangle(FCX, MTHY,  FCX - thw, tailTY,  FCX + thw, tailTY,  fill);
    _cv.drawLine(FCX, MTHY, FCX - thw, tailTY, rim);
    _cv.drawLine(FCX, MTHY, FCX + thw, tailTY, rim);

    int cr = min(CR, bh / 3);
    _cv.fillRoundRect(bx, by, bw, bh, cr, fill);
    _cv.drawRoundRect(bx, by, bw, bh, cr, rim);

    if (p < 0.45f) return;

    for (int i = 0; i < 10; i++) {
        int sx = bx + 3 + (_bStarX[i] * (bw - 6) / 100);
        int sy = by + 3 + (_bStarY[i] * (bh - 6) / 100);
        if (sx < bx+2 || sx > bx+bw-3 || sy < by+2 || sy > by+bh-3) continue;
        if (_bStarBrt[i]) _cv.fillSmoothCircle(sx, sy, 1, 0xFFFF);
        else              _cv.drawPixel(sx, sy, sdim);
    }

    if (p < 0.90f) return;

    const int PAD  = 5;
    const int ROWS = (BH_FULL - PAD * 2) / 8;
    uint32_t  now2 = millis();
    if (_bLnCnt > ROWS && now2 - _bScrollMs >= 1400) {
        if (_bScroll + ROWS < _bLnCnt) { _bScroll++; _bScrollMs = now2; }
    }
    _cv.setTextSize(1);
    _cv.setTextColor(tc, fill);
    _cv.setTextDatum(top_left);
    for (int r = 0; r < ROWS && (_bScroll + r) < _bLnCnt; r++)
        _cv.drawString(_bLines[_bScroll + r], bx + PAD, by + PAD + r * 8);
}

static void render(float t) {
    int oy = (int)_breath;

    // Per-state params
    float faceBright = 1.0f;
    float mouthRY    = 7.0f;
    bool  flatMouth  = false;
    bool  frown      = false;

    switch (_fstate) {
        case IrisState::BOOT:
            flatMouth  = true;
            faceBright = 0.6f;
            break;
        case IrisState::CONFIG_PORTAL:
            flatMouth  = true;
            break;
        case IrisState::WIFI_CONNECTING:
            flatMouth  = true;
            faceBright = 0.75f;
            break;
        case IrisState::PH3B3_SEARCHING:
            faceBright = 0.85f;
            mouthRY    = 5.0f;
            break;
        case IrisState::PH3B3_HEALTHY:
            faceBright = 1.0f;
            mouthRY    = 7.0f + _speak * 9.0f;
            break;
        case IrisState::PH3B3_UNREACHABLE:
            faceBright = 0.5f;
            mouthRY    = 5.0f;
            frown      = true;
            break;
    }

    _cv.fillScreen(C_BLK);

    // 1 — outer glow
    _cv.fillSmoothCircle(FCX, FCY + oy, GLOWR, dimC(C_GLOW, faceBright * 0.7f));

    // 2 — face disc
    uint16_t cFace = dimC(C_FACE, faceBright);
    _cv.fillSmoothCircle(FCX, FCY + oy, FCR, cFace);

    // (circuit traces removed — face-only design)

    // 4 — dark almond eyes
    int ey = EYY + oy;
    _cv.fillEllipse(LEX, ey, ERX, ERY, C_EYE);
    _cv.fillEllipse(REX, ey, ERX, ERY, C_EYE);

    // 5 — brow arcs: face-colour ellipse carved over the top of each eye
    //     gives the almond / sleepy-lid shape from the SVG
    _cv.fillEllipse(LEX, ey - ERY + 3, ERX + 3, 8, cFace);
    _cv.fillEllipse(REX, ey - ERY + 3, ERX + 3, 8, cFace);

    // 6 — blink: face-colour rect drops from eye top
    if (_blink > 0.01f) {
        int bh = max(1, (int)(_blink * (ERY * 2 + 4)));
        _cv.fillRect(LEX - ERX - 1, ey - ERY, ERX * 2 + 2, bh, cFace);
        _cv.fillRect(REX - ERX - 1, ey - ERY, ERX * 2 + 2, bh, cFace);
    }

    // 7 — white highlights (upper-left of each iris)
    if (_blink < 0.65f) {
        _cv.fillSmoothCircle(LEX - 4, ey - 4, 3, C_WHITE);
        _cv.fillSmoothCircle(REX - 4, ey - 4, 3, C_WHITE);
    }

    // 8 — mouth
    uint16_t cMo = dimC(C_MOUTH, faceBright);
    int my = MTHY + oy;
    if (flatMouth) {
        _cv.fillSmoothRoundRect(FCX - 11, my, 22, 3, 1, cMo);
    } else {
        float ry = frown ? -mouthRY : mouthRY;
        drawMouthArc(FCX, my, 15.0f, ry, cMo, 3);
    }

    // 9 — speech bubble
    if (_bProg > 0.0f) drawBubble();

    // 10 — status text (below face sprite, never shifts with breath)
    _cv.setTextDatum(MC_DATUM);
    _cv.setTextColor(0x7BEF, C_BLK);
    _cv.setTextSize(1);
    _cv.drawString(_statusTxt.c_str(), SCREEN_W / 2, STATUS_CENTER_Y);

    _cv.pushSprite(0, 0);
}

// ─── IrisFace ─────────────────────────────────────────────────────────────────

void IrisFace::begin() {
    // StickS3 ST7789 needs RGB order; M5GFX default is BGR.
    auto* panel = M5.Display.panel();
    auto  pcfg  = panel->config();
    pcfg.rgb_order = true;
    panel->config(pcfg);
    M5.Display.setRotation(0);

    C_FACE  = M5.Display.color565(0x6B, 0x3F, 0xA0);
    C_GLOW  = M5.Display.color565(0x3B, 0x1F, 0x60);
    C_EYE   = M5.Display.color565(0x1A, 0x08, 0x30);
    C_MOUTH = M5.Display.color565(0xE0, 0x40, 0x8A);
    C_WHITE = 0xFFFF;
    C_BLK   = 0x0000;

    _cv.deleteSprite();
    _cv.setColorDepth(16);
    _cv.createSprite(SCREEN_W, SCREEN_H);

    _nextBlink = millis() + 1500;
    _statusTxt = "waking up";
}

void IrisFace::update() {
    uint32_t now = millis();
    float    t   = now / 1000.0f;

    _breath = sinf(t * 6.2832f / 3.0f) * 1.5f;

    // Blink
    if (_blinkSt == 0 && now > _nextBlink) _blinkSt = now;
    if (_blinkSt != 0) {
        float k = (float)(now - _blinkSt);
        _blink = (k < 90.f)  ? k / 90.f
               : (k < 160.f) ? 1.0f
               : (k < 250.f) ? 1.0f - (k - 160.f) / 90.f
               : 0.0f;
        if (k >= 250.f) { _blinkSt = 0; _nextBlink = now + 2200 + random(3000); }
    }
    if (_fstate == IrisState::BOOT) _blink = 0.7f;

    // Mouth decay
    float target = (now - _levelMs < 300) ? _levelVal : 0.0f;
    _speak += (target - _speak) * 0.25f;

    // Bubble animate
    if (_bGrow) {
        float raw = (float)(now - _bGrowMs) / 280.0f;
        float pp  = min(1.0f, raw);
        _bProg = pp * pp * (3.0f - 2.0f * pp);
        if (raw >= 1.0f) _bGrow = false;
    } else if (_bFall) {
        float raw = 1.0f - (float)(now - _bFallMs) / 200.0f;
        float pp  = max(0.0f, raw);
        _bProg = pp * pp * (3.0f - 2.0f * pp);
        if (raw <= 0.0f) { _bFall = false; _bProg = 0.0f; }
    }

    render(t);
}

void IrisFace::setState(IrisState s, const char* speechOverride) {
    _fstate = s;
    switch (s) {
        case IrisState::BOOT:              _statusTxt = "waking up";     break;
        case IrisState::CONFIG_PORTAL:     _statusTxt = "Iris-Setup";    break;
        case IrisState::WIFI_CONNECTING:   _statusTxt = "connecting..."; break;
        case IrisState::PH3B3_SEARCHING:   _statusTxt = "finding Ph3b3"; break;
        case IrisState::PH3B3_HEALTHY:     _statusTxt = "Ph3b3 online";  break;
        case IrisState::PH3B3_UNREACHABLE: _statusTxt = "Ph3b3 away";    break;
    }
    if (speechOverride) _statusTxt = speechOverride;
}

void IrisFace::setStatusLine(const char* text) {
    _statusTxt = text;
}

void IrisFace::setSpeakingLevel(float level01) {
    _levelVal = constrain(level01, 0.0f, 1.0f);
    _levelMs  = millis();
}

IrisState IrisFace::getState() const { return _fstate; }

void IrisFace::setBubble(const String& text) {
    _bGrow    = true;
    _bFall    = false;
    _bGrowMs  = millis();
    _bScroll  = 0;
    _bScrollMs = _bGrowMs;
    _bLnCnt   = 0;

    int i = 0, len = (int)text.length();
    while (i < len && _bLnCnt < _BMAX) {
        while (i < len && text[i] == ' ') i++;
        if (i >= len) break;
        int end = min(len, i + _BCOLS);
        if (end < len) {
            int brk = end;
            while (brk > i && text[brk] != ' ') brk--;
            if (brk > i) end = brk;
        }
        int n = end - i;
        if (n > _BCOLS) n = _BCOLS;
        if (n > 0) {
            strncpy(_bLines[_bLnCnt], text.c_str() + i, n);
            while (n > 0 && _bLines[_bLnCnt][n-1] == ' ') n--;
            _bLines[_bLnCnt][n] = '\0';
            if (n > 0) _bLnCnt++;
        }
        i = end;
    }

    for (int j = 0; j < 8;  j++) { _bStarX[j]=random(100); _bStarY[j]=random(100); _bStarBrt[j]=false; }
    for (int j = 8; j < 10; j++) { _bStarX[j]=random(100); _bStarY[j]=random(100); _bStarBrt[j]=true;  }
}

void IrisFace::clearBubble() {
    if (_bGrow || _bProg > 0.0f) {
        _bGrow   = false;
        _bFall   = true;
        _bFallMs = millis();
    }
}
