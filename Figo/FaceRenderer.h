#ifndef FACE_RENDERER_H
#define FACE_RENDERER_H

#include <lvgl.h>

enum class Expression : uint8_t {
  Neutral,
  Happy,
  Panicked,
  Bullish,
  Sleeping,
  // Boot-splash only - shown once by BootScreens before the market-data
  // carousel exists. Must never be reachable from computeExpression(); it has no meaning as a market state.
  Greeting
};

// Composes the procedural vector face from a fixed set of pre-allocated
// LVGL primitives (vector-only, no bitmaps). setExpression()
// only mutates existing objects' geometry/angle/color - it never creates or
// deletes lv_obj_t widgets at runtime, avoiding LVGL object-churn overhead
// in the animation hot path. Pure with respect to other Figo modules: it
// only touches its own LVGL objects, no SharedState access here - the
// expression-to-metric mapping lives elsewhere and just calls
// setExpression().
//
// Geometry comes from the face artwork, stored as per-mille-of-width constants so one face can be
// built at any size: the carousel face is 99px wide, the boot Greeting face
// 96px. Expressions carry their artwork's accent color too (Bullish light
// green, Panicked red, the rest white) per the exported carousel frames.
class FaceRenderer {
 public:
  // width scales the whole face; every feature position is a fraction of it.
  // The container is a transparent width x width square.
  void init(lv_obj_t *parent, lv_coord_t width = 99);
  void setExpression(Expression e);
  Expression currentExpression() const { return current_; }

  // init() centers the face container in its parent by default. Callers
  // sharing the parent with other content can reposition it after init().
  // lv_obj_align (not lv_obj_set_pos): init()'s lv_obj_center() sets a
  // persistent LV_ALIGN_CENTER style, and a bare set_pos afterward only
  // changes the x/y OFFSETS from that center - the face would stay
  // horizontally centered. Realigning to TOP_LEFT makes x/y behave as
  // absolute coordinates again.
  void setPosition(lv_coord_t x, lv_coord_t y) {
    if (!container_) return;  // torn down (stopBlink() nulls it)
    lv_obj_align(container_, LV_ALIGN_TOP_LEFT, x, y);
  }

  // Deletes blinkTimer_ and any pending blink-reopen timer. Must be called before a caller deletes this
  // FaceRenderer's parent screen - otherwise the still-running LVGL timer
  // fires again afterward and touches now-dangling child objects. Safe to
  // call at most once; a FaceRenderer that outlives its screen for the
  // whole program (ScreenManager's carousel instance) never needs this.
  void stopBlink();

 private:
  static void blinkTimerCb(lv_timer_t *timer);
  // Scale a per-mille-of-width design value to pixels for this instance.
  lv_coord_t S(int16_t perMille) const {
    return (lv_coord_t)(((int32_t)width_ * perMille + 500) / 1000);
  }
  // Rewrites the brow line point arrays directly (inner/outer end Y offsets
  // in pixels, mirrored left/right) rather than rotating a fixed-shape line
  // - a prior rotation-based version made the sign of "up" vs "down" easy
  // to get backwards. Equal values = flat brow.
  // Returns the y shift the caller must apply (lv_line cannot handle
  // negative point coordinates - see the definition).
  lv_coord_t setBrowShape(int16_t width, lv_coord_t innerY, lv_coord_t outerY);
  // Fits one curved brow to the ink it should cover - see the long note on
  // the definition for why an arc's box has to be square.
  void curvedBrow(lv_obj_t *arc, int16_t cx, int16_t topY, int16_t spanW,
                  int16_t depth);
  // Applies this expression's accent color to every visible stroke/fill.
  void applyColor(lv_color_t color);
  void blink();

  // Idle animation, one per expression. Neutral/Greeting/Panicked blink on
  // blinkTimer_; Happy, Bullish and Sleeping have no eyes to blink (chevrons
  // and "$" glyphs) and so had no motion at all until these. Every one of
  // them only mutates already-allocated objects, in keeping with this
  // class's no-object-churn rule.
  void startIdleAnim(Expression e);
  // Cancels every running idle animation and restores the properties they
  // drive. Called at the top of setExpression() - an animation left running
  // against a hidden object would keep writing to it, and worse, would still
  // be mid-flight when the next expression positioned the same object.
  void stopIdleAnims();
  // Ping-pongs obj's y between baseY and baseY + delta, forever.
  static void bobY(lv_obj_t *obj, lv_coord_t baseY, lv_coord_t delta, uint32_t periodMs);
  static void animSetY(void *obj, int32_t v);
  static void animSetTextOpa(void *obj, int32_t v);

  lv_obj_t *container_ = nullptr;
  lv_coord_t width_ = 99;

  // Open-eye state (Neutral, Greeting, Panicked): filled sclera + pupil.
  lv_obj_t *eyeLeft_ = nullptr;
  lv_obj_t *eyeRight_ = nullptr;
  lv_obj_t *pupilLeft_ = nullptr;
  lv_obj_t *pupilRight_ = nullptr;

  // Closed-eye chevrons, reshaped per expression: Happy peaks UP ("^"),
  // Sleeping bows DOWN ("v") below its flat-bar lids.
  lv_obj_t *eyeClosedLeft_ = nullptr;
  lv_obj_t *eyeClosedRight_ = nullptr;

  // Bullish-only: eyes replaced entirely with "$" glyphs. Text labels are
  // not a bitmap-asset concern under the vector-only rule.
  lv_obj_t *dollarLeft_ = nullptr;
  lv_obj_t *dollarRight_ = nullptr;

  // Straight bar brows (Neutral/Sleeping flat, Panicked steep) - thick
  // round-capped lines, per the artwork.
  lv_obj_t *browLeft_ = nullptr;
  lv_obj_t *browRight_ = nullptr;
  // Curved brows (Greeting gentle, Happy medium, Bullish dramatic).
  lv_obj_t *browArcLeft_ = nullptr;
  lv_obj_t *browArcRight_ = nullptr;

  lv_obj_t *mouthArc_ = nullptr;  // smiles, Panicked's oval, Sleeping's frown
  lv_obj_t *mouthBar_ = nullptr;  // Neutral's short flat bar
  lv_obj_t *zzzLabel_ = nullptr;
  lv_timer_t *blinkTimer_ = nullptr;
  // The one-shot that re-opens the eyes ~150ms after blink() shuts them.
  // Tracked (rather than fire-and-forget) so stopBlink() can cancel it: it
  // holds raw pointers to this face's eye objects, and a screen deleted
  // inside that 150ms window would leave it firing against freed memory.
  // BootScreens tears its faces down on every recovery from a runtime WiFi
  // disconnect, not just once at boot.
  lv_timer_t *blinkReopenTimer_ = nullptr;

  // Line-point storage is per-instance (LVGL retains the pointers): two
  // differently-sized faces coexist at boot, so file-scope shared arrays
  // would corrupt one of them.
  lv_point_t browLeftPoints_[2] = {};
  lv_point_t browRightPoints_[2] = {};
  lv_point_t chevronLeftPoints_[3] = {};
  lv_point_t chevronRightPoints_[3] = {};
  lv_point_t mouthBarPoints_[2] = {};

  Expression current_ = Expression::Neutral;
  lv_coord_t eyeOpenHeight_ = 0;
  // Vertical centre of the open eyes. blink() collapses the sclera toward
  // this rather than just setting its height: lv_obj_set_height() keeps the
  // TOP edge fixed, so the closed eye would appear as a line at the top of the eye socket
  // instead of across its middle.
  lv_coord_t eyeCenterY_ = 0;
  // Resting y of the closed-eye chevrons and the "Z", captured where they
  // are positioned so the idle animations can return to it.
  lv_coord_t closedEyeBaseY_ = 0;
  lv_coord_t zzzBaseY_ = 0;
};

#endif // FACE_RENDERER_H
