#include "FaceRenderer.h"

#include <initializer_list>

#include "FigoTheme.h"

// All geometry constants are per-mille of the face WIDTH (the init()
// parameter), derived from the face artwork by connected-component
// analysis. Y values already fold in each source image's
// aspect ratio, so a square width x width container fits every expression.
namespace {

// --- Shared open eyes (Neutral / Greeting / Panicked use variations) ---
constexpr int16_t kEyeLeftCx = 225;
constexpr int16_t kEyeRightCx = 780;
constexpr int16_t kEyeCy = 365;
constexpr int16_t kEyeD = 340;
constexpr int16_t kPupilD = 110;

// Panicked: slightly closer-set, marginally smaller eyes, small high pupils.
constexpr int16_t kPanEyeLeftCx = 285;
constexpr int16_t kPanEyeRightCx = 730;
constexpr int16_t kPanEyeD = 318;
constexpr int16_t kPanPupilD = 90;
constexpr int16_t kPanPupilDy = -35;  // pupils ride high - startled

// --- Brows ---
constexpr int16_t kBrowW = 380;
constexpr int16_t kBrowStroke = 55;
constexpr int16_t kBrowCy = 118;         // Neutral's flat bars
// Panicked's steep bars, back at the artwork's own brow centre (its brow box
// is y 2.8%..21.7% of a 800x739 image, i.e. cy 114 per-mille of width).
// Raising cy to dodge the top edge was treating the symptom: the real fault
// was the SLOPE. The artwork's brow box is 18.9% tall = 175 per-mille, and a
// sloped bar occupies 2*slope + stroke, so slope = (175 - 55)/2 = 60, not 85.
// At 85 the ink reached cy - 85 - 27 = 2 per-mille and was clipped; at 60 it
// starts at 27, matching the artwork's 26.
constexpr int16_t kPanBrowCy = 114;
constexpr int16_t kPanBrowSlope = 60;    // +/- pixels-of-width/1000 per end
// Panicked's brows are trimmed at their INNER (nose-side) ends only. At the
// full kBrowW they run to 475 and 540 per-mille, all but meeting over the
// middle of the face. Trimming 30 pulls each inner end back to 445 / 570,
// which is where that eye's own inner edge sits (444 / 571) - the brow gap
// then matches the eye gap (125 vs 127) instead of being half of it.
constexpr int16_t kPanBrowTrim = 30;
constexpr int16_t kPanBrowW = kBrowW - kPanBrowTrim;
// Moving each centre outward by HALF the trim is what keeps the outer end
// pinned to the artwork's outline while the inner end moves in by the full
// trim - a plain narrower brow would have pulled both ends in.
constexpr int16_t kPanBrowCxL = kPanEyeLeftCx - kPanBrowTrim / 2;
constexpr int16_t kPanBrowCxR = kPanEyeRightCx + kPanBrowTrim / 2;
// The inner end stops partway up the original slope instead of at its
// full rise, so the brow keeps its angle rather than getting steeper:
// rise = slope * (width - 2*trim) / width.
constexpr int16_t kPanBrowInnerRise =
    (int16_t)((int32_t)kPanBrowSlope * (kBrowW - 2 * kPanBrowTrim) / kBrowW);
constexpr int16_t kSleepBrowCy = 375;    // Sleeping's bars sit AT eye height
constexpr int16_t kSleepBrowW = 275;
// Curved brows, described by the INK they should cover rather than by a box
// and an angle - curvedBrow() solves for the circle that produces it. Values
// are the artwork's own brow bounding boxes, converted to per-mille of face
// WIDTH (the source measurements normalise x and y to their own axes, so y is
// scaled by the source image's height/width ratio):
//   Greeting (630x552, y*8.76): box x 2.7% y 2.7% w 36.8% h 9.2%
//   Happy    (630x552, y*8.76): box x 1.4% y 9.2% w 37.0% h 11.8%
//   Bullish  (727x739, y*10.2): box x 4.8% y 2.6% w 32.6% h 17.6%
// "top" is the topmost ink, so it doubles as the clipping guarantee: every
// one of these is comfortably inside the container.
constexpr int16_t kGreetBrowTop = 24;
constexpr int16_t kGreetBrowW = 368;
constexpr int16_t kGreetBrowDepth = 81;
constexpr int16_t kHappyBrowTop = 81;
constexpr int16_t kHappyBrowW = 370;
constexpr int16_t kHappyBrowDepth = 103;
// Bullish is the one place the artwork is not followed literally. Its brows
// are drawn 326 wide at cx 205/792 - a dramatic sweep set much wider than the
// "$" eyes at 312/741. Centred on those eyes instead (which is what reads as
// correct on the panel) a 326-wide brow leaves only ~5px between the pair at
// 99px, so they merge into one bar. Scaled to 250 wide, keeping the artwork's
// 326:179 aspect, they stay centred on the eyes with a clear ~13px gap.
constexpr int16_t kBullBrowTop = 26;
constexpr int16_t kBullBrowW = 250;
constexpr int16_t kBullBrowDepth = 137;

// --- Closed-eye chevrons ---
constexpr int16_t kChevronW = 310;
constexpr int16_t kChevronH = 95;
constexpr int16_t kChevronStroke = 55;
constexpr int16_t kHappyEyeCy = 360;     // "^" peak
constexpr int16_t kSleepEyeCy = 588;     // "v" below the bar lids

// --- Bullish "$" eyes ---
constexpr int16_t kDollarLeftCx = 312;
constexpr int16_t kDollarRightCx = 741;
constexpr int16_t kDollarCy = 411;

// --- Mouths ---
constexpr int16_t kMouthCx = 505;
constexpr int16_t kNeutralMouthW = 270;
constexpr int16_t kNeutralMouthCy = 820;
constexpr int16_t kNeutralMouthStroke = 52;
constexpr int16_t kGreetMouthD = 320;
constexpr int16_t kGreetMouthCy = 700;
constexpr int16_t kHappyMouthD = 420;
constexpr int16_t kHappyMouthCy = 660;
constexpr int16_t kBullMouthD = 820;
constexpr int16_t kBullMouthCy = 500;
constexpr int16_t kPanMouthW = 200;
constexpr int16_t kPanMouthH = 316;
constexpr int16_t kPanMouthCy = 749;
constexpr int16_t kSleepMouthD = 330;
constexpr int16_t kSleepMouthCy = 1010;  // frown arc's circle center is low;
                                         // its upper crescent lands ~876
constexpr int16_t kMouthStroke = 48;
constexpr int16_t kPanMouthStroke = 42;

constexpr int16_t kZzzCx = 890;
constexpr int16_t kZzzCy = 100;

constexpr uint32_t kBlinkIntervalMs = 3000;
constexpr uint32_t kBlinkCloseDurationMs = 150;

// Per-expression accent color, from the exported carousel frames: Bullish is
// light green, Panicked red, everything else white.
lv_color_t accentFor(Expression e) {
  switch (e) {
    case Expression::Bullish: return lv_color_hex(0x5CCD4D);
    case Expression::Panicked: return lv_color_hex(0xD60307);
    default: return lv_color_white();
  }
}

// Nearest compiled Montserrat for a target pixel height - the "$" and "Z"
// glyph sizes scale with the face like everything else.
const lv_font_t *fontNear(lv_coord_t px) {
  if (px >= 46) return &lv_font_montserrat_48;
  if (px >= 42) return &lv_font_montserrat_44;
  if (px >= 38) return &lv_font_montserrat_40;
  if (px >= 33) return &lv_font_montserrat_36;
  if (px >= 27) return &lv_font_montserrat_30;
  if (px >= 22) return &lv_font_montserrat_24;
  if (px >= 18) return &lv_font_montserrat_20;
  if (px >= 15) return &lv_font_montserrat_16;
  if (px >= 13) return &lv_font_montserrat_14;
  return &lv_font_montserrat_12;
}

void sizeCenteredAt(lv_obj_t *obj, lv_coord_t centerX, lv_coord_t centerY,
                     lv_coord_t width, lv_coord_t height) {
  lv_obj_set_size(obj, width, height);
  lv_obj_set_pos(obj, centerX - width / 2, centerY - height / 2);
}

void sizeCenteredAt(lv_obj_t *obj, lv_coord_t centerX, lv_coord_t centerY, lv_coord_t size) {
  sizeCenteredAt(obj, centerX, centerY, size, size);
}

void styleArcStroke(lv_obj_t *arc, lv_coord_t width) {
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  // lv_arc's centre is coords.x1 + pad_left + radius, so any padding here
  // would silently shift every arc off its intended centre.
  lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
}

void styleLineStroke(lv_obj_t *line, lv_coord_t width) {
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_color(line, lv_color_white(), 0);
  lv_obj_set_style_line_rounded(line, true, 0);
}
}  // namespace

void FaceRenderer::init(lv_obj_t *parent, lv_coord_t width) {
  width_ = width;
  container_ = lv_obj_create(parent);
  // remove_style_all() is load-bearing, not tidiness: a bare lv_obj_create()
  // inherits the default theme's "card" style, which carries
  // pad_all = PAD_DEF (13px at this panel's DISP_SMALL/LV_DPI_DEF 130).
  // Children placed with lv_obj_set_pos() are positioned relative to the
  // parent's CONTENT area, so every feature below - all of which assume a
  // 0..width_ box - was displaced by (+13,+13) and then clipped at the
  // container's right and bottom edges. Clearing bg/border individually
  // does not touch padding, which would crop the face on the right and bottom
  // edges.
  lv_obj_remove_style_all(container_);
  lv_obj_set_size(container_, width_, width_);
  lv_obj_center(container_);
  // Decoration, never a control. Plain lv_obj children keep LVGL 8's default
  // LV_OBJ_FLAG_CLICKABLE (lines and labels clear it themselves), and a
  // clickable child swallows the tap before it reaches the screen - which
  // would break the NO-WIFI screen's tap-anywhere handler wherever the face
  // happens to cover.
  lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // --- Open eyes: filled sclera + pupil ---
  eyeLeft_ = lv_obj_create(container_);
  eyeRight_ = lv_obj_create(container_);
  pupilLeft_ = lv_obj_create(container_);
  pupilRight_ = lv_obj_create(container_);
  // Same theme-chrome removal as the container above - these are plain
  // filled circles, not cards.
  lv_obj_t *eyes[] = {eyeLeft_, eyeRight_};
  for (lv_obj_t *eye : eyes) {
    lv_obj_remove_style_all(eye);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_t *pupils[] = {pupilLeft_, pupilRight_};
  for (lv_obj_t *pupil : pupils) {
    lv_obj_remove_style_all(pupil);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pupil, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pupil, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  }

  // --- Closed-eye chevrons (points set per expression) ---
  eyeClosedLeft_ = lv_line_create(container_);
  eyeClosedRight_ = lv_line_create(container_);
  styleLineStroke(eyeClosedLeft_, S(kChevronStroke));
  styleLineStroke(eyeClosedRight_, S(kChevronStroke));

  // --- Bullish's "$" eyes ---
  dollarLeft_ = lv_label_create(container_);
  dollarRight_ = lv_label_create(container_);
  const lv_font_t *dollarFont = fontNear(S(370));
  lv_obj_t *dollars[] = {dollarLeft_, dollarRight_};
  for (lv_obj_t *dollar : dollars) {
    lv_label_set_text(dollar, "$");
    lv_obj_set_style_text_color(dollar, lv_color_white(), 0);
    lv_obj_set_style_text_font(dollar, dollarFont, 0);
  }

  // --- Straight bar brows ---
  browLeft_ = lv_line_create(container_);
  browRight_ = lv_line_create(container_);
  styleLineStroke(browLeft_, S(kBrowStroke));
  styleLineStroke(browRight_, S(kBrowStroke));

  // --- Curved brows ---
  browArcLeft_ = lv_arc_create(container_);
  browArcRight_ = lv_arc_create(container_);
  styleArcStroke(browArcLeft_, S(kBrowStroke));
  styleArcStroke(browArcRight_, S(kBrowStroke));

  // --- Mouths ---
  mouthArc_ = lv_arc_create(container_);
  styleArcStroke(mouthArc_, S(kMouthStroke));
  mouthBar_ = lv_line_create(container_);
  styleLineStroke(mouthBar_, S(kNeutralMouthStroke));
  mouthBarPoints_[0] = {0, 0};
  mouthBarPoints_[1] = {S(kNeutralMouthW), 0};
  lv_line_set_points(mouthBar_, mouthBarPoints_, 2);
  lv_obj_set_pos(mouthBar_, S(kMouthCx - kNeutralMouthW / 2), S(kNeutralMouthCy));

  zzzLabel_ = lv_label_create(container_);
  lv_label_set_text(zzzLabel_, "Z");
  lv_obj_set_style_text_color(zzzLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(zzzLabel_, fontNear(S(180)), 0);
  zzzBaseY_ = S(kZzzCy - 90);
  lv_obj_set_pos(zzzLabel_, S(kZzzCx - 90), zzzBaseY_);

  // Drop any timer left over from a previous init() on this instance.
  // BootScreens builds and tears down its faces more than once (the
  // runtime NO-WIFI screen comes and goes), and a second timer here would
  // outlive the first screen's objects and fire against freed memory.
  //
  // Deliberately not stopBlink(): that also restores the properties the idle
  // animations drive, which would dereference the PREVIOUS screen's objects
  // - already freed by the lv_obj_del() that tore that screen down. Their
  // animations went with them (LVGL's object destructor calls
  // lv_anim_del(obj, NULL)), so there is nothing left to cancel here.
  if (blinkTimer_) {
    lv_timer_del(blinkTimer_);
    blinkTimer_ = nullptr;
  }
  blinkTimer_ = lv_timer_create(blinkTimerCb, kBlinkIntervalMs, this);

  setExpression(Expression::Neutral);
}

// Places one curved brow so that its INK covers (cx +/- spanW/2) horizontally,
// starting at topY and curving down by depth.
//
// This exists because lv_arc does NOT fit an arc to its box the way you would
// expect. From lv_arc.c: the radius is MIN(w, h) / 2 and the centre is
// coords.x1 + pad + radius - NOT the box centre. A wide flat box, which is
// the obvious way to ask for a wide shallow brow, therefore produced a small
// circle of radius h/2 pinned to the box's LEFT edge. With the old 370x170
// box that meant a brow of radius 85 (a quarter of the intended width) sitting
// 100 per-mille left of where it belonged - exactly the "too small and not
// centered" that showed up on Happy and Bullish.
//
// So the box has to be SQUARE and sized to the radius the curve needs. Solve
// the circle through the brow's centreline: half-width a and sagitta d give
// R = (a^2 + d^2) / 2d, and the half-angle is asin(a/R) - which is always
// valid, since a^2 + d^2 >= 2ad makes R >= a. LVGL strokes inward from the
// radius it is given, so pass R + stroke/2 to keep the stroke centred on R.
void FaceRenderer::curvedBrow(lv_obj_t *arc, int16_t cx, int16_t topY,
                              int16_t spanW, int16_t depth) {
  float a = (spanW - kBrowStroke) * 0.5f;   // centreline half-width
  float d = (float)(depth - kBrowStroke);   // centreline sagitta
  if (a < 1.0f) a = 1.0f;
  if (d < 1.0f) d = 1.0f;
  const float rMid = (a * a + d * d) / (2.0f * d);
  const float rOuter = rMid + kBrowStroke * 0.5f;

  lv_coord_t size = S((int16_t)lroundf(rOuter * 2.0f));
  if (size < 2) size = 2;
  const lv_coord_t r = size / 2;
  lv_obj_set_size(arc, size, size);
  // Box top IS the ink top: the circle centre sits r below it, and LVGL's
  // outer radius is r.
  lv_obj_set_pos(arc, S(cx) - r, S(topY));

  int16_t half = (int16_t)lroundf(asinf(a / rMid) * 57.29577951f);
  if (half < 1) half = 1;
  if (half > 179) half = 179;
  lv_arc_set_angles(arc, 270 - half, 270 + half);
}

// Returns how far the caller must move the line objects UP so the brow still
// lands where it was asked for. See the normalisation note below.
//
// Left brow: point 0 is the outer (away from nose) end, point 1 is inner.
// Right brow is mirrored - point 0 inner, point 1 outer. Getting this
// mapping backwards makes Panicked read as angry instead
// of worried.
lv_coord_t FaceRenderer::setBrowShape(int16_t width, lv_coord_t innerY,
                                      lv_coord_t outerY) {
  // A NEGATIVE point y must never reach lv_line. Two things in lv_line.c
  // conspire against it:
  //   * LV_EVENT_GET_SELF_SIZE computes the height as LV_MAX(point.y, 0)
  //     over the points, so a negative y contributes nothing - the object
  //     ends up shorter than its own artwork and its top edge sits BELOW
  //     the highest ink.
  //   * LV_EVENT_REFR_EXT_DRAW_SIZE only extends the draw area by
  //     line_width, and refr_obj() clips every widget to coords +
  //     ext_draw_size.
  // Panicked's raised inner end (-S(kPanBrowSlope)) needed the draw area to
  // reach slope + stroke/2 above coords.y1 but only got stroke, so the top
  // of both brows was clipped by the LINE'S OWN draw area - not by the face
  // container. That is why raising kPanBrowCy never helped: the overshoot is
  // relative to the object, so it simply moved down with it.
  //
  // Shifting the points to be non-negative and moving the object up by the
  // same amount is geometrically identical and keeps lv_line's bookkeeping
  // honest.
  lv_coord_t minY = LV_MIN(innerY, outerY);
  lv_coord_t shift = (minY < 0) ? (lv_coord_t)(-minY) : 0;
  browLeftPoints_[0] = {0, (lv_coord_t)(outerY + shift)};
  browLeftPoints_[1] = {S(width), (lv_coord_t)(innerY + shift)};
  browRightPoints_[0] = {0, (lv_coord_t)(innerY + shift)};
  browRightPoints_[1] = {S(width), (lv_coord_t)(outerY + shift)};
  lv_line_set_points(browLeft_, browLeftPoints_, 2);
  lv_line_set_points(browRight_, browRightPoints_, 2);
  return shift;
}

void FaceRenderer::applyColor(lv_color_t color) {
  lv_obj_set_style_bg_color(eyeLeft_, color, 0);
  lv_obj_set_style_bg_color(eyeRight_, color, 0);
  lv_obj_set_style_line_color(eyeClosedLeft_, color, 0);
  lv_obj_set_style_line_color(eyeClosedRight_, color, 0);
  lv_obj_set_style_text_color(dollarLeft_, color, 0);
  lv_obj_set_style_text_color(dollarRight_, color, 0);
  lv_obj_set_style_line_color(browLeft_, color, 0);
  lv_obj_set_style_line_color(browRight_, color, 0);
  lv_obj_set_style_arc_color(browArcLeft_, color, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(browArcRight_, color, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(mouthArc_, color, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(mouthBar_, color, 0);
  lv_obj_set_style_text_color(zzzLabel_, color, 0);
}

void FaceRenderer::setExpression(Expression e) {
  if (!container_) return;  // torn down; nothing to express
  current_ = e;
  stopIdleAnims();

  // Hide every mutually-exclusive primitive up front, then show only what
  // this expression needs - keeps adding a new expression from silently
  // missing a hide somewhere else.
  lv_obj_t *allExclusive[] = {
      eyeLeft_,       eyeRight_,       pupilLeft_, pupilRight_,
      eyeClosedLeft_, eyeClosedRight_, dollarLeft_, dollarRight_,
      browLeft_,      browRight_,      browArcLeft_, browArcRight_,
      mouthArc_,      mouthBar_,       zzzLabel_};
  for (lv_obj_t *obj : allExclusive) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
  applyColor(accentFor(e));

  // Local helpers to cut repetition below.
  auto show = [](std::initializer_list<lv_obj_t *> objs) {
    for (lv_obj_t *o : objs) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  };
  auto openEyes = [&](int16_t leftCx, int16_t rightCx, int16_t eyeD,
                      int16_t pupilD, int16_t pupilDy) {
    eyeOpenHeight_ = S(eyeD);
    eyeCenterY_ = S(kEyeCy);
    sizeCenteredAt(eyeLeft_, S(leftCx), S(kEyeCy), S(eyeD));
    sizeCenteredAt(eyeRight_, S(rightCx), S(kEyeCy), S(eyeD));
    sizeCenteredAt(pupilLeft_, S(leftCx), S(kEyeCy) + S(pupilDy), S(pupilD));
    sizeCenteredAt(pupilRight_, S(rightCx), S(kEyeCy) + S(pupilDy), S(pupilD));
    show({eyeLeft_, eyeRight_, pupilLeft_, pupilRight_});
  };
  // Both brow helpers take the eye centres this expression actually uses
  // rather than assuming the default pair. Panicked sets its eyes closer
  // together (kPanEye*Cx) and Bullish replaces them with "$" glyphs at
  // kDollar*Cx, and brows drawn over kEye*Cx would sit visibly off to the outside of the
  // eyes they belong to.
  auto browArcs = [&](int16_t leftCx, int16_t rightCx, int16_t topY, int16_t spanW,
                      int16_t depth) {
    curvedBrow(browArcLeft_, leftCx, topY, spanW, depth);
    curvedBrow(browArcRight_, rightCx, topY, spanW, depth);
    show({browArcLeft_, browArcRight_});
  };
  auto flatBrows = [&](int16_t leftCx, int16_t rightCx, int16_t width, int16_t cy,
                       lv_coord_t innerY, lv_coord_t outerY) {
    // setBrowShape() normalises away any negative point y and tells us by how
    // much; subtract it here so the brow's absolute position is unchanged.
    lv_coord_t shift = setBrowShape(width, innerY, outerY);
    lv_obj_set_pos(browLeft_, S(leftCx - width / 2), S(cy) - shift);
    lv_obj_set_pos(browRight_, S(rightCx - width / 2), S(cy) - shift);
    show({browLeft_, browRight_});
  };
  auto chevrons = [&](int16_t cy, bool peakUp) {
    lv_coord_t w = S(kChevronW), h = S(kChevronH);
    lv_coord_t peakY = peakUp ? 0 : h;
    lv_coord_t endY = peakUp ? h : 0;
    chevronLeftPoints_[0] = {0, endY};
    chevronLeftPoints_[1] = {(lv_coord_t)(w / 2), peakY};
    chevronLeftPoints_[2] = {w, endY};
    memcpy(chevronRightPoints_, chevronLeftPoints_, sizeof(chevronRightPoints_));
    lv_line_set_points(eyeClosedLeft_, chevronLeftPoints_, 3);
    lv_line_set_points(eyeClosedRight_, chevronRightPoints_, 3);
    closedEyeBaseY_ = S(cy) - h / 2;
    lv_obj_set_pos(eyeClosedLeft_, S(kEyeLeftCx) - w / 2, closedEyeBaseY_);
    lv_obj_set_pos(eyeClosedRight_, S(kEyeRightCx) - w / 2, closedEyeBaseY_);
    show({eyeClosedLeft_, eyeClosedRight_});
  };
  auto mouthSmile = [&](int16_t diameter, int16_t cy, uint16_t startAngle,
                        uint16_t endAngle, int16_t stroke) {
    lv_obj_set_style_arc_width(mouthArc_, S(stroke), LV_PART_INDICATOR);
    sizeCenteredAt(mouthArc_, S(kMouthCx), S(cy), S(diameter));
    lv_arc_set_bg_angles(mouthArc_, 0, 360);
    lv_arc_set_angles(mouthArc_, startAngle, endAngle);
    show({mouthArc_});
  };

  switch (e) {
    case Expression::Neutral:
      openEyes(kEyeLeftCx, kEyeRightCx, kEyeD, kPupilD, 20);
      flatBrows(kEyeLeftCx, kEyeRightCx, kBrowW, kBrowCy, 0, 0);
      show({mouthBar_});
      break;

    case Expression::Greeting:
      openEyes(kEyeLeftCx, kEyeRightCx, kEyeD, kPupilD, 20);
      browArcs(kEyeLeftCx, kEyeRightCx, kGreetBrowTop, kGreetBrowW, kGreetBrowDepth);
      mouthSmile(kGreetMouthD, kGreetMouthCy, 35, 145, kMouthStroke);
      break;

    case Expression::Happy:
      chevrons(kHappyEyeCy, /*peakUp=*/true);
      browArcs(kEyeLeftCx, kEyeRightCx, kHappyBrowTop, kHappyBrowW, kHappyBrowDepth);
      mouthSmile(kHappyMouthD, kHappyMouthCy, 30, 150, kMouthStroke);
      break;

    case Expression::Bullish: {
      // Labels size to their text and only know that width after a layout
      // pass - force one before centering them on the eye spots.
      lv_obj_update_layout(dollarLeft_);
      lv_coord_t dw = lv_obj_get_width(dollarLeft_);
      lv_obj_set_pos(dollarLeft_, S(kDollarLeftCx) - dw / 2, S(kDollarCy - 190));
      lv_obj_set_pos(dollarRight_, S(kDollarRightCx) - dw / 2, S(kDollarCy - 190));
      show({dollarLeft_, dollarRight_});
      // Over the "$" glyphs, not the (hidden) default eye spots.
      browArcs(kDollarLeftCx, kDollarRightCx, kBullBrowTop, kBullBrowW, kBullBrowDepth);
      mouthSmile(kBullMouthD, kBullMouthCy, 40, 140, kMouthStroke);
      break;
    }

    case Expression::Panicked:
      openEyes(kPanEyeLeftCx, kPanEyeRightCx, kPanEyeD, kPanPupilD, kPanPupilDy);
      // Worried, not angry: inner ends (near the nose) raised, outer dropped.
      // Trimmed at the inner ends only - see kPanBrowTrim.
      flatBrows(kPanBrowCxL, kPanBrowCxR, kPanBrowW, kPanBrowCy,
                -S(kPanBrowInnerRise), S(kPanBrowSlope));
      // Hollow "shocked" oval: a full-circle arc stroked thin on a tall box.
      lv_obj_set_style_arc_width(mouthArc_, S(kPanMouthStroke), LV_PART_INDICATOR);
      sizeCenteredAt(mouthArc_, S(kMouthCx), S(kPanMouthCy), S(kPanMouthW), S(kPanMouthH));
      lv_arc_set_bg_angles(mouthArc_, 0, 360);
      lv_arc_set_angles(mouthArc_, 0, 360);
      show({mouthArc_});
      break;

    case Expression::Sleeping:
      // bar "lids" at eye height. kSleepBrowW is the artwork's narrower lid;
      // it was defined but never passed, so the lids were drawn at the full
      // kBrowW and overhung the closed eyes underneath them.
      flatBrows(kEyeLeftCx, kEyeRightCx, kSleepBrowW, kSleepBrowCy, 0, 0);
      chevrons(kSleepEyeCy, /*peakUp=*/false);
      // Calm frown: the upper crescent of a circle whose center sits low.
      mouthSmile(kSleepMouthD, kSleepMouthCy, 215, 325, kPanMouthStroke);
      show({zzzLabel_});
      break;
  }

  startIdleAnim(e);
}

void FaceRenderer::animSetY(void *obj, int32_t v) {
  lv_obj_set_y(static_cast<lv_obj_t *>(obj), (lv_coord_t)v);
}

void FaceRenderer::animSetTextOpa(void *obj, int32_t v) {
  lv_obj_set_style_text_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)v, 0);
}

void FaceRenderer::bobY(lv_obj_t *obj, lv_coord_t baseY, lv_coord_t delta,
                        uint32_t periodMs) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, animSetY);
  lv_anim_set_values(&a, baseY, baseY + delta);
  lv_anim_set_time(&a, periodMs);
  lv_anim_set_playback_time(&a, periodMs);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

void FaceRenderer::stopIdleAnims() {
  if (!container_) return;  // torn down - the objects below are gone
  // A null exec_cb removes every animation registered against that object.
  lv_anim_del(eyeClosedLeft_, nullptr);
  lv_anim_del(eyeClosedRight_, nullptr);
  lv_anim_del(dollarLeft_, nullptr);
  lv_anim_del(dollarRight_, nullptr);
  lv_anim_del(zzzLabel_, nullptr);
  // Restore what the animations drove. Without this a face that left
  // Sleeping mid-fade would keep a part-transparent "Z" the next time one
  // was shown, and the chevrons would rest wherever the bob stopped.
  if (zzzLabel_) {
    lv_obj_set_y(zzzLabel_, zzzBaseY_);
    lv_obj_set_style_text_opa(zzzLabel_, LV_OPA_COVER, 0);
  }
}

void FaceRenderer::startIdleAnim(Expression e) {
  // S(1000) is the face width, so these deltas read as a fraction of it and
  // stay proportional at both the 96px boot size and the 99px carousel one.
  switch (e) {
    case Expression::Happy:
      // Closed "^" eyes cannot blink, so Happy gets a slow contented bob.
      bobY(eyeClosedLeft_, closedEyeBaseY_, S(28), 1400);
      bobY(eyeClosedRight_, closedEyeBaseY_, S(28), 1400);
      break;

    case Expression::Bullish: {
      // "$" eyes pulse up and down - a cha-ching beat, faster than Happy's.
      lv_coord_t base = S(kDollarCy - 190);
      bobY(dollarLeft_, base, -S(30), 900);
      bobY(dollarRight_, base, -S(30), 900);
      break;
    }

    case Expression::Sleeping: {
      // The classic sleepy "Z": drifts up while fading out, then restarts
      // from the bottom. No playback - it should rise and vanish, not
      // slide back down.
      lv_anim_t rise;
      lv_anim_init(&rise);
      lv_anim_set_var(&rise, zzzLabel_);
      lv_anim_set_exec_cb(&rise, animSetY);
      lv_anim_set_values(&rise, zzzBaseY_, zzzBaseY_ - S(140));
      lv_anim_set_time(&rise, 2000);
      lv_anim_set_repeat_count(&rise, LV_ANIM_REPEAT_INFINITE);
      lv_anim_start(&rise);

      lv_anim_t fade;
      lv_anim_init(&fade);
      lv_anim_set_var(&fade, zzzLabel_);
      lv_anim_set_exec_cb(&fade, animSetTextOpa);
      lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
      lv_anim_set_time(&fade, 2000);
      lv_anim_set_repeat_count(&fade, LV_ANIM_REPEAT_INFINITE);
      lv_anim_start(&fade);
      break;
    }

    // Neutral, Greeting and Panicked all have open eyes and are animated by
    // blinkTimer_ instead.
    default:
      break;
  }
}

void FaceRenderer::blink() {
  if (!container_) return;  // torn down between timer arm and fire
  // Only the open-eye states have anything to blink; the others already
  // show closed eyes or "$" glyphs.
  if (current_ == Expression::Happy || current_ == Expression::Bullish ||
      current_ == Expression::Sleeping) {
    return;
  }

  // Collapse toward the eye's centre, not its top edge. lv_obj_set_height()
  // alone keeps y fixed, which put the closed-eye line across the top of the
  // socket rather than the middle of it.
  constexpr lv_coord_t kClosedH = 2;
  lv_obj_set_height(eyeLeft_, kClosedH);
  lv_obj_set_height(eyeRight_, kClosedH);
  lv_obj_set_y(eyeLeft_, eyeCenterY_ - kClosedH / 2);
  lv_obj_set_y(eyeRight_, eyeCenterY_ - kClosedH / 2);
  lv_obj_add_flag(pupilLeft_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(pupilRight_, LV_OBJ_FLAG_HIDDEN);

  // A previous reopen still pending (only possible if the blink interval
  // were ever shortened below the close duration) would be orphaned here.
  if (blinkReopenTimer_) lv_timer_del(blinkReopenTimer_);
  blinkReopenTimer_ = lv_timer_create(
      [](lv_timer_t *t) {
        FaceRenderer *self = static_cast<FaceRenderer *>(t->user_data);
        lv_obj_set_height(self->eyeLeft_, self->eyeOpenHeight_);
        lv_obj_set_height(self->eyeRight_, self->eyeOpenHeight_);
        lv_obj_set_y(self->eyeLeft_, self->eyeCenterY_ - self->eyeOpenHeight_ / 2);
        lv_obj_set_y(self->eyeRight_, self->eyeCenterY_ - self->eyeOpenHeight_ / 2);
        lv_obj_clear_flag(self->pupilLeft_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(self->pupilRight_, LV_OBJ_FLAG_HIDDEN);
        // repeat_count 1: LVGL deletes t right after this returns, so drop
        // our handle rather than leaving it dangling for stopBlink().
        self->blinkReopenTimer_ = nullptr;
      },
      kBlinkCloseDurationMs, this);
  lv_timer_set_repeat_count(blinkReopenTimer_, 1);
}

void FaceRenderer::blinkTimerCb(lv_timer_t *timer) {
  static_cast<FaceRenderer *>(timer->user_data)->blink();
}

void FaceRenderer::stopBlink() {
  stopIdleAnims();  // safe here: the caller has not deleted the screen yet
  if (blinkTimer_) {
    lv_timer_del(blinkTimer_);
    blinkTimer_ = nullptr;
  }
  if (blinkReopenTimer_) {
    lv_timer_del(blinkReopenTimer_);
    blinkReopenTimer_ = nullptr;
  }
  // Forget every handle. The caller's next move is lv_obj_del() on the
  // parent screen, which frees all of these; nulling container_ is what
  // makes the guards above turn a second stopBlink(), a late timer, or a
  // stray setExpression() into a no-op instead of a use-after-free.
  container_ = nullptr;
}
