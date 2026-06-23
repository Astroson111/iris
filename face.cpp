#include "face.h"
#include <M5Unified.h>
#include <Avatar.h>
#include <faces/FaceTemplates.hpp>
#include "config.h"

using namespace m5avatar;

// ─────────────────────────────────────────────────────────────────────────────
// Portrait face layout for the StickS3 (135×240, avatar region 135×200).
//
// BoundingRect coordinates are (top/y, left/x).
// Reference frame: default M5Stack face is 320×240.
//   Default positions (y,x): mouth(148,163), r-eye(93,90), l-eye(96,230),
//                             r-brow(67,96),  l-brow(72,230).
// Scaled to 135×200 (xScale=135/320≈0.422, yScale=200/240≈0.833):
//   mouth(123,68) r-eye(77,38) l-eye(80,97) r-brow(56,40) l-brow(60,97)
//
// GirlyEye chosen for character; size (28,52) gives a tall, expressive iris on
// the narrow portrait canvas without the eyes crowding each other.
// ─────────────────────────────────────────────────────────────────────────────
class IrisPortraitFace : public Face {
 public:
    IrisPortraitFace()
        : Face(
              // mouth (minW, maxW, minH, maxH)
              new UShapeMouth(22, 22, 0, 10), new BoundingRect(123, 68),
              // right eye (character's right = viewer's left)
              new GirlyEye(28, 52, false),    new BoundingRect(77, 38),
              // left eye
              new GirlyEye(28, 52, true),     new BoundingRect(80, 97),
              // eyebrows
              new EllipseEyebrow(16, 5, false), new BoundingRect(56, 40),
              new EllipseEyebrow(16, 5, true),  new BoundingRect(60, 97),
              // face sprite: top-left (0,0), 135 wide × 200 tall
              new BoundingRect(0, 0, FACE_SPRITE_W, FACE_SPRITE_H),
              new M5Canvas(&M5.Display), new M5Canvas(&M5.Display)) {}
};

// ── Module-level singletons ────────────────────────────────────────────────
static Avatar            _avatar;
static IrisPortraitFace* _portraitFace = nullptr;

// ── State → expression + default speech text ──────────────────────────────
struct StateRow { IrisState state; Expression expr; const char* speech; };
static const StateRow STATE_TABLE[] = {
    { IrisState::BOOT,             Expression::Sleepy,  ""               },
    { IrisState::CONFIG_PORTAL,    Expression::Neutral, "Iris-Setup"     },
    { IrisState::WIFI_CONNECTING,  Expression::Doubt,   "Connecting..."  },
    { IrisState::PH3B3_SEARCHING,  Expression::Neutral, "Finding Ph3b3" },
    { IrisState::PH3B3_HEALTHY,    Expression::Happy,   "Ph3b3 online"  },
    { IrisState::PH3B3_UNREACHABLE,Expression::Sad,     "Ph3b3 away"    },
};

// ── IrisFace implementation ────────────────────────────────────────────────

void IrisFace::begin() {
    _portraitFace = new IrisPortraitFace();

    // Ph3b3 brand palette:
    //   PRIMARY   = iris-blue (eyes, outlines)
    //   BACKGROUND= near-black
    //   SECONDARY = soft magenta (cheeks — Ph3b3's accent)
    ColorPalette cp;
    cp.set(COLOR_PRIMARY,    TFT_NAVY);
    cp.set(COLOR_BACKGROUND, TFT_BLACK);
    cp.set(COLOR_SECONDARY,  TFT_MAGENTA);

    _avatar.setFace(_portraitFace);
    _avatar.setColorPalette(cp);
    _avatar.init(8);  // 8-bit colour; balances quality vs RAM on S3

    // Clear the status band; Avatar only ever writes to y < FACE_SPRITE_H.
    M5.Display.fillRect(0, FACE_SPRITE_H, SCREEN_W, SCREEN_H - FACE_SPRITE_H, TFT_BLACK);
}

void IrisFace::setState(IrisState s, const char* speechOverride) {
    _state = s;
    for (const auto& row : STATE_TABLE) {
        if (row.state == s) {
            _avatar.setExpression(row.expr);
            _avatar.setSpeechText(speechOverride ? speechOverride : row.speech);
            return;
        }
    }
}

void IrisFace::setStatusLine(const char* text) {
    // Writes only to the status band below the avatar sprite (y >= FACE_SPRITE_H).
    // Avatar's FreeRTOS task does not touch this region, so no bus conflict.
    M5.Display.fillRect(0, FACE_SPRITE_H, SCREEN_W, SCREEN_H - FACE_SPRITE_H, TFT_BLACK);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.drawString(text, SCREEN_W / 2, STATUS_CENTER_Y);
}
