#include "BootScreens.h"
#include "Config.h"
#include "ErrorCodes.h"
#include "FigoTheme.h"
#include <Arduino.h>

namespace {
// OTA screen text rows. Progress states stack title / percentage / bar / note
// down the right-hand block; the failure state drops the percentage and the bar
// and centres the two remaining lines on the face instead (see showOta()).
constexpr lv_coord_t kOtaTitleY = 26;
constexpr lv_coord_t kOtaNoteY = 122;
constexpr lv_coord_t kOtaFaceMidY = 86;  // the 96px face sits at y=38..134
constexpr lv_coord_t kOtaFailGap = 8;    // between the two failure lines
// "NO WIFI SCREEN" text block: the design's two lines have left edges of
// 146.4 and 152.1, which are both a centered block on x=220 - the middle of
// the space left of the 104px face at x=16..120.
constexpr lv_coord_t kNoWifiTextW = 190;
constexpr lv_coord_t kNoWifiTextX = 220 - kNoWifiTextW / 2;
}  // namespace

void BootScreens::init() {
  helloScreen_ = lv_obj_create(NULL);
  buildHelloScreen(helloScreen_);
  ensureConnectScreen();
}

void BootScreens::ensureConnectScreen() {
  if (connectScreen_) return;
  connectScreen_ = lv_obj_create(NULL);
  buildConnectScreen(connectScreen_);
}

void BootScreens::ensureNoWifiScreen() {
  if (noWifiScreen_) return;
  noWifiScreen_ = lv_obj_create(NULL);
  buildNoWifiScreen(noWifiScreen_);
}

void BootScreens::hideOta() {
  if (otaScreen_) {
    otaFace_.stopBlink();
    lv_obj_del(otaScreen_);  // children (labels, bar, face) go with it
    otaScreen_ = nullptr;
    otaTitle_ = otaPercent_ = otaBar_ = otaNote_ = nullptr;
  }
  otaActive_ = false;
}

void BootScreens::showOta(OtaUiState state, uint8_t percent, const char *message) {
  if (!otaScreen_) {
    otaScreen_ = lv_obj_create(NULL);
    buildOtaScreen(otaScreen_);
    otaShownState_ = -1;
    otaShownPct_ = -1;
    otaShownMsg_ = nullptr;
  }
  otaActive_ = true;
  if (lv_scr_act() != otaScreen_) lv_scr_load(otaScreen_);

  if ((int)state == otaShownState_ && (int)percent == otaShownPct_ && message == otaShownMsg_) return;
  bool stateChanged = (int)state != otaShownState_;
  otaShownState_ = (int)state;
  otaShownPct_ = (int)percent;
  otaShownMsg_ = message;

  bool failed = (state == OtaUiState::FAILED);
  if (stateChanged) {
    // Failure is red, like the disconnect screen, and gets the Panicked face.
    otaFace_.setExpression(failed ? Expression::Panicked : Expression::Greeting);
    lv_obj_set_style_text_color(otaTitle_, failed ? figo_theme::negative() : figo_theme::textWhite(), 0);
    lv_label_set_text(otaTitle_, failed ? "Update failed" : "Updating Figo");
    // Both failure lines are red, not just the title: a grey reason under a red
    // headline read as a second, unrelated message.
    lv_obj_set_style_text_color(otaNote_, failed ? figo_theme::negative() : figo_theme::subtitleGrey(), 0);
    if (failed) {
      lv_obj_add_flag(otaBar_, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(otaBar_, LV_OBJ_FLAG_HIDDEN);
      // Back to the progress layout (a new attempt while a failure is still up).
      lv_obj_set_pos(otaTitle_, kNoWifiTextX, kOtaTitleY);
      lv_obj_set_pos(otaNote_, kNoWifiTextX, kOtaNoteY);
    }
  }

  char pct[8];
  switch (state) {
    case OtaUiState::RECEIVING:
      snprintf(pct, sizeof(pct), "%u%%", (unsigned)percent);
      lv_label_set_text(otaPercent_, pct);
      lv_label_set_text(otaNote_, "Keep Figo powered");
      break;
    case OtaUiState::VERIFYING:
      lv_label_set_text(otaPercent_, "100%");
      lv_label_set_text(otaNote_, "Checking the update");
      break;
    case OtaUiState::DONE:
      lv_label_set_text(otaPercent_, "100%");
      lv_label_set_text(otaNote_, "Restarting");
      break;
    case OtaUiState::FAILED:
      lv_label_set_text(otaPercent_, "");
      // Points at a string literal owned by OtaManager (see SharedState.h).
      lv_label_set_text(otaNote_, message && message[0] ? message : "Unknown error");
      break;
    default:  // PREPARING
      lv_label_set_text(otaPercent_, "0%");
      lv_label_set_text(otaNote_, "Getting ready");
      break;
  }
  lv_bar_set_value(otaBar_, percent, LV_ANIM_OFF);

  if (failed) {
    // Centre the title + reason as ONE block on the face's vertical midline. The
    // reason can wrap to two lines, so measure after the text is set: the layout
    // must be brought up to date first or the heights are still the old ones.
    lv_obj_update_layout(otaTitle_);
    lv_obj_update_layout(otaNote_);
    lv_coord_t th = lv_obj_get_height(otaTitle_);
    lv_coord_t nh = lv_obj_get_height(otaNote_);
    lv_coord_t top = kOtaFaceMidY - (th + kOtaFailGap + nh) / 2;
    lv_obj_set_pos(otaTitle_, kNoWifiTextX, top);
    lv_obj_set_pos(otaNote_, kNoWifiTextX, top + th + kOtaFailGap);
  }
}

void BootScreens::showHello() {
  if (lv_scr_act() != helloScreen_) {
    lv_scr_load(helloScreen_);
    if (helloShownAtMs_ == 0) {
      helloShownAtMs_ = millis();
    }
  }
}

void BootScreens::showConnect() {
  // Hold the hello splash on screen for its minimum duration first - see
  // BootScreens.h. Callers poll this every tick, so no timer is needed.
  if (!helloMinElapsed()) {
    return;
  }
  ensureConnectScreen();
  if (lv_scr_act() != connectScreen_) {
    lv_scr_load(connectScreen_);
  }
}

// Entered from a running carousel, so there is no hello splash to hold and
// no helloMinElapsed() gate here - the point is to stop showing prices that
// can no longer be refreshed as soon as the drop is confirmed.
void BootScreens::showNoWifi() {
  ensureNoWifiScreen();
  noWifiActive_ = true;
  if (lv_scr_act() != noWifiScreen_ && lv_scr_act() != connectScreen_) {
    lv_scr_load(noWifiScreen_);
  }
}

// One-way on purpose. Toggling back to the disconnect frame on a second tap
// would be confusing and misleading: raising the portal STOPS the background reconnect attempts
// (FigoNetworkManager::serviceStaLink()), so once the credentials are on
// screen "click the screen to configure new WiFi" is no longer true and
// there is nothing useful to go back to. The credentials stay up until the
// user finishes provisioning, or the old network returns by itself.
void BootScreens::runtimeTapCb(lv_event_t *e) {
  BootScreens *self = static_cast<BootScreens *>(lv_event_get_user_data(e));
  if (!self->noWifiActive_) return;      // first-run AP flow: nothing to do
  if (lv_scr_act() != self->noWifiScreen_) return;  // already on credentials

  // The "click the screen to configure new WiFi" action. Ask Task 2 for the
  // setup AP, then show the credentials.
  self->portalRequestPending_ = true;
  self->ensureConnectScreen();
  lv_scr_load(self->connectScreen_);
}

bool BootScreens::helloMinElapsed() const {
  if (helloShownAtMs_ == 0) {
    return true;  // never shown - nothing to hold
  }
  return (millis() - helloShownAtMs_) >= BOOT_HELLO_MIN_DURATION_MS;
}

void BootScreens::showConfigError(uint32_t mask) {
  if (!configErrorScreen_) {
    configErrorScreen_ = lv_obj_create(NULL);
    buildConfigErrorScreen(configErrorScreen_, mask);
  }
  if (lv_scr_act() != configErrorScreen_) lv_scr_load(configErrorScreen_);
}

void BootScreens::hideConfigError() {
  if (!configErrorScreen_) return;
  // lv_obj_del() frees the objects but not the blink timer that points at them.
  configErrorFace_.stopBlink();
  lv_obj_del(configErrorScreen_);
  configErrorScreen_ = nullptr;
}

void BootScreens::teardown() {
  hideConfigError();
  if (helloScreen_) {
    greetingFace_.stopBlink();
    lv_obj_del(helloScreen_);
    helloScreen_ = nullptr;
  }
  if (connectScreen_) {
    lv_obj_del(connectScreen_);
    connectScreen_ = nullptr;
  }
  hideOta();
  if (noWifiScreen_) {
    // Same reason greetingFace_ stops first: lv_obj_del() frees the LVGL
    // objects but knows nothing about the blink timer still pointing at them.
    noWifiFace_.stopBlink();
    lv_obj_del(noWifiScreen_);
    noWifiScreen_ = nullptr;
  }
  noWifiActive_ = false;
  portalRequestPending_ = false;
}

// The greeting screen: a 96px Greeting face at (19,44) on the left, two white
// text lines on the right. Fully procedural - no bitmap assets.
void BootScreens::buildHelloScreen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), LV_PART_MAIN);

  greetingFace_.init(screen, 96);
  greetingFace_.setPosition(19, 44);
  greetingFace_.setExpression(Expression::Greeting);

  // Plain ASCII apostrophes: the design's U+2019 isn't in LVGL's built-in
  // Montserrat glyph range and would render as a blank.
  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Hi! I'm Figo!");
  lv_obj_set_style_text_font(title, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(title, figo_theme::textWhite(), 0);
  lv_obj_set_pos(title, 173, 68);  // baseline 79.2 in the design

  lv_obj_t *subtitle = lv_label_create(screen);
  lv_label_set_text(subtitle, "Let's get you online!");
  lv_obj_set_style_text_font(subtitle, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(subtitle, figo_theme::textWhite(), 0);
  lv_obj_set_pos(subtitle, 145, 94);  // baseline 105.2
}

// The connection-instructions screen: a centered title plus the real SoftAP
// credentials, centered as a two-line block. (The red "NO WIFI" screen is a
// separate screen, buildNoWifiScreen().)
void BootScreens::buildConnectScreen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Connect to my WiFi for setup");
  lv_obj_set_style_text_font(title, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(title, figo_theme::textWhite(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 56);  // baseline 67.2

  lv_obj_t *creds = lv_label_create(screen);
  lv_label_set_text(creds, "SSID: " SOFTAP_SSID "\nPASS: " SOFTAP_PASSWORD);
  lv_obj_set_style_text_font(creds, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(creds, figo_theme::textWhite(), 0);
  lv_obj_set_style_text_align(creds, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(creds, LV_ALIGN_TOP_MID, 0, 82);  // first baseline 93.2

  // Only live during the runtime-disconnect flow (runtimeTapCb bails out
  // otherwise) - it taps back to the "WiFi Disconnected" screen.
  lv_obj_add_event_cb(screen, runtimeTapCb, LV_EVENT_CLICKED, this);
}

// The "NO WIFI" screen: a red 104px Panicked face at (16,34) and two red text
// lines centered in the remaining space to its right (a block centered on
// x=220).
// Shown on a runtime STA drop, in place of a carousel whose prices have
// stopped refreshing.
void BootScreens::buildNoWifiScreen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), LV_PART_MAIN);

  // Expression::Panicked already carries the design's #D60307 accent
  // (FaceRenderer::accentFor), so no per-object recoloring is needed here.
  noWifiFace_.init(screen, 104);
  noWifiFace_.setPosition(16, 34);
  noWifiFace_.setExpression(Expression::Panicked);

  lv_color_t red = figo_theme::negative();  // #D60307, the frame's text color

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "WiFi Disconnected");
  lv_obj_set_style_text_font(title, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(title, red, 0);
  lv_obj_set_width(title, kNoWifiTextW);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(title, kNoWifiTextX, 56);  // baseline 67.2

  lv_obj_t *hint = lv_label_create(screen);
  // Explicit break at the design's own line split rather than relying on
  // where the width happens to wrap it.
  lv_label_set_text(hint, "click the screen to\nconfigure new WiFi");
  lv_obj_set_style_text_font(hint, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(hint, red, 0);
  lv_obj_set_width(hint, kNoWifiTextW);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(hint, kNoWifiTextX, 82);  // first baseline 93.2

  lv_obj_add_event_cb(screen, runtimeTapCb, LV_EVENT_CLICKED, this);
}

// Same composition as the NO WIFI screen (red 104px Panicked face at (16,34), two
// red centred lines to its right) so the two failure screens read as one family.
// Only static text: nothing on it updates after it is built, so the mask is
// consumed here and not kept.
void BootScreens::buildConfigErrorScreen(lv_obj_t *screen, uint32_t mask) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), LV_PART_MAIN);

  configErrorFace_.init(screen, 104);
  configErrorFace_.setPosition(16, 34);
  configErrorFace_.setExpression(Expression::Panicked);

  lv_color_t red = figo_theme::negative();

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Settings problem");
  lv_obj_set_style_text_font(title, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(title, red, 0);
  lv_obj_set_width(title, kNoWifiTextW);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(title, kNoWifiTextX, 44);

  // "Error 60 64": the prefix once, then every failed section's code. Five
  // codes wrap to a second line, which the block has room for.
  char codes[20];
  formatConfigErrorCodes(codes, sizeof(codes), mask);
  char body[48];
  snprintf(body, sizeof(body), "Using defaults\nError %s", codes);

  lv_obj_t *hint = lv_label_create(screen);
  lv_label_set_text(hint, body);
  lv_obj_set_style_text_font(hint, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(hint, red, 0);
  lv_obj_set_width(hint, kNoWifiTextW);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(hint, kNoWifiTextX, 70);
}

// A 96px face on the left (the Greeting frame's size and position) and a text
// block centred in the space to its right, like the disconnect screen. Widgets
// are created once here and only retexted by showOta().
void BootScreens::buildOtaScreen(lv_obj_t *screen) {
  lv_obj_set_style_bg_color(screen, figo_theme::bg(), LV_PART_MAIN);

  otaFace_.init(screen, 96);
  otaFace_.setPosition(19, 38);
  otaFace_.setExpression(Expression::Greeting);

  otaTitle_ = lv_label_create(screen);
  lv_label_set_text(otaTitle_, "Updating Figo");
  lv_obj_set_style_text_font(otaTitle_, FIGO_FONT_SMALL, 0);
  lv_obj_set_style_text_color(otaTitle_, figo_theme::textWhite(), 0);
  lv_obj_set_width(otaTitle_, kNoWifiTextW);
  lv_obj_set_style_text_align(otaTitle_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(otaTitle_, kNoWifiTextX, kOtaTitleY);

  otaPercent_ = lv_label_create(screen);
  lv_label_set_text(otaPercent_, "0%");
  lv_obj_set_style_text_font(otaPercent_, FIGO_FONT_PRICE_MD, 0);
  lv_obj_set_style_text_color(otaPercent_, figo_theme::textWhite(), 0);
  lv_obj_set_width(otaPercent_, kNoWifiTextW);
  lv_obj_set_style_text_align(otaPercent_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(otaPercent_, kNoWifiTextX, 54);

  // The bar's track and indicator use the Fear & Greed gauge greys/green so it
  // reads as part of the same design. Explicit size: LVGL's default bar is 150x10
  // and the theme's padding would otherwise eat into it (see the notes on
  // widgets not fitting their box).
  otaBar_ = lv_bar_create(screen);
  lv_obj_remove_style_all(otaBar_);
  lv_obj_set_size(otaBar_, kNoWifiTextW - 20, 8);
  lv_obj_set_pos(otaBar_, kNoWifiTextX + 10, 100);
  lv_bar_set_range(otaBar_, 0, 100);
  lv_bar_set_value(otaBar_, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_opa(otaBar_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(otaBar_, figo_theme::trackDark(), LV_PART_MAIN);
  lv_obj_set_style_radius(otaBar_, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(otaBar_, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(otaBar_, figo_theme::positive(), LV_PART_INDICATOR);
  lv_obj_set_style_radius(otaBar_, 4, LV_PART_INDICATOR);

  otaNote_ = lv_label_create(screen);
  lv_label_set_text(otaNote_, "");
  lv_obj_set_style_text_font(otaNote_, FIGO_FONT_TINY, 0);
  lv_obj_set_style_text_color(otaNote_, figo_theme::subtitleGrey(), 0);
  lv_obj_set_width(otaNote_, kNoWifiTextW);
  lv_obj_set_style_text_align(otaNote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(otaNote_, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(otaNote_, kNoWifiTextX, kOtaNoteY);
}
