// Perch OS shell on LVGL 9. Design source of truth: design/index.html.
// Face (clock + Smart Stack), honeycomb app grid, Control Center, Camera, Ask, Answer,
// Photos gallery, History, Settings, Model picker. All apps are layers on one screen.
#include "device_ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include "ai_client.h"
#include "clock.h"
#include "display.h"
#include "storage.h"

namespace {
constexpr int W = 240, H = 284;
// System edges: drag up from the bottom strip for home, down from the top strip for
// Control Center. Touches that start there never reach the app underneath.
constexpr int HOME_ZONE = 236;
constexpr int TOP_ZONE = 22;
// Distance from the bottom edge to the lowest tappable control (clear of the home zone).
constexpr int ABOVE_HOME = H - HOME_ZONE + 14;
// Drag this far (or flick) to commit an edge gesture; less springs back.
constexpr int COMMIT_DRAG = 70;
constexpr int MAX_PHOTOS = 12;
// Lower number = less JPEG compression. 20 showed visible blocks in the viewfinder.
constexpr int PREVIEW_QUALITY = 12;
// 480x320 gives enough pixels to fill the 240x284 screen by downscaling, not upscaling.
constexpr framesize_t PREVIEW_SIZE = FRAMESIZE_HVGA;
const uint8_t BRIGHTNESS[] = {255, 140, 50};

// Design tokens (design/index.html :root)
lv_color_t VOID_, GRAPHITE, ICON_BG, ICON_DIM, LINE, MIST, INK, LENS;
const lv_font_t *F_SMALL = &lv_font_montserrat_12;
const lv_font_t *F_BODY = &lv_font_montserrat_16;
const lv_font_t *F_LARGE = &lv_font_montserrat_24;
const lv_font_t *F_CLOCK = &lv_font_montserrat_48;

enum class Screen { Face, Apps, Camera, Ask, Answer, Photos, History, Settings, Model, Notice, Count };
Screen current = Screen::Face;
Screen noticeReturn = Screen::Ask;

Preferences settings;
bool useGemini = true;
bool cameraOff = false;
uint8_t brightnessLevel = 0;
uint8_t spiMhz = 10;
unsigned long sequence = 0;
unsigned long lastSerialMs = 0;

// Rendering
uint16_t *framebuffer = nullptr;
lv_display_t *display = nullptr;
lv_indev_t *touch = nullptr;
lv_obj_t *root = nullptr;

// Camera and photos
uint16_t *livePixels = nullptr;     // viewfinder, 240x284
uint16_t *photoPixels = nullptr;    // newest photo, filled to the screen
uint16_t *galleryPixels = nullptr;  // photo shown in Photos / Answer
uint8_t *savedJpeg = nullptr, *rgbScratch = nullptr;
size_t jpegBytes = 0, jpegCapacity = 0, rgbCapacity = 0;
uint32_t latestPhotoId = 0;
bool previewActive = false;
framesize_t stillSize = FRAMESIZE_QXGA;
int stillQuality = 8;
uint32_t photoIds[MAX_PHOTOS];
int photoCount = 0, photoIndex = 0;
lv_image_dsc_t liveDsc, photoDsc, galleryDsc;

// Answers
String lastAnswer;
AnswerInfo lastInfo;
bool pendingGemini = true;
uint32_t pendingPhotoId = 0;
uint32_t answerIds[ANSWER_KEEP];
int answerCount = 0;
uint16_t *historyThumbs = nullptr;  // ANSWER_KEEP x 64x64
lv_image_dsc_t historyDsc[ANSWER_KEEP];
bool historyDirty = true;

// Widgets
lv_obj_t *scr[(int)Screen::Count];
lv_obj_t *clockLabel, *dateLabel, *faceOffline, *card, *cardImage, *cardScrim, *cardRing, *cardKey, *cardValue,
    *cardDots;
lv_obj_t *iconName;
std::vector<lv_obj_t *> icons;
lv_obj_t *viewfinder, *thumb, *flash, *shutter, *cameraOffLabel;
lv_obj_t *askPhoto, *askEmpty, *askHint, *askPill, *askButtonLabel, *askOffline, *busy, *busyRing, *busyLabel,
    *askSheet;
lv_obj_t *answerScroll, *answerPhoto, *answerLead, *answerBody, *answerMeta;
lv_obj_t *photosImage, *photosEmpty, *photoCounter;
lv_obj_t *historyList, *historyEmpty;
lv_obj_t *modelValue, *wifiValue, *brightSlider, *storageValue, *storageSub;
lv_obj_t *modelCheck[2], *modelSub[2];
lv_obj_t *noticeText;
lv_obj_t *homeBar;
lv_obj_t *cc, *ccWifi, *ccBright, *ccBrightLabel, *ccModel, *ccModelLabel, *ccCamera;
bool ccOpen = false;

// Touch
bool injecting = false;
int injectX = 0, injectY = 0;
bool fingerDown = false;
enum class Edge { None, Home, Control } edge = Edge::None;
int downX = 0, downY = 0, lastX = 0, lastY = 0;

// ---------- widget helpers ----------
lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, value);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}
lv_obj_t *circle(lv_obj_t *parent, int size, lv_color_t border, int width, lv_color_t fill, lv_opa_t fillOpa) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, width, 0);
  lv_obj_set_style_border_color(o, border, 0);
  lv_obj_set_style_bg_color(o, fill, 0);
  lv_obj_set_style_bg_opa(o, fillOpa, 0);
  return o;
}
lv_obj_t *screenBase() {
  lv_obj_t *s = plain(root);
  lv_obj_set_size(s, W, H);
  lv_obj_set_style_bg_color(s, VOID_, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  return s;
}
lv_obj_t *scrimBottom(lv_obj_t *parent, int height) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, lv_pct(100), height);
  lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_opa(o, 170, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
lv_obj_t *pill(lv_obj_t *parent, const char *label, int width) {
  lv_obj_t *p = plain(parent);
  lv_obj_set_size(p, width, 44);
  lv_obj_set_style_radius(p, 22, 0);
  lv_obj_set_style_bg_color(p, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(p, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_t *l = text(p, label, F_BODY, INK);
  lv_obj_center(l);
  return p;
}
void setupImage(lv_image_dsc_t &dsc, const uint16_t *pixels, int w, int h) {
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = w;
  dsc.header.h = h;
  dsc.header.stride = w * 2;
  dsc.data_size = w * h * 2;
  dsc.data = reinterpret_cast<const uint8_t *>(pixels);
}
void hide(lv_obj_t *o, bool hidden) {
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void onClick(lv_obj_t *o, void (*fn)()) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      o, [](lv_event_t *e) { ((void (*)())lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, (void *)fn);
}
lv_obj_t *pressedFeedback(lv_obj_t *o) {
  lv_obj_set_style_opa(o, LV_OPA_60, LV_STATE_PRESSED);
  return o;
}
String ago(uint32_t when) {
  const time_t now = time(nullptr);
  if (!when || !clockKnown() || now < (time_t)when) return "";
  const uint32_t d = now - when;
  if (d < 60) return "just now";
  if (d < 3600) return String(d / 60) + " min ago";
  if (d < 86400) return String(d / 3600) + " h ago";
  char buf[16];
  const time_t t = when;
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%b %e", &tm);
  return buf;
}

// ---------- images ----------
bool jpegSize(const uint8_t *b, size_t n, int &w, int &h) {
  for (size_t i = 2; i + 9 < n;) {
    if (b[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = b[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      h = (b[i + 5] << 8) | b[i + 6];
      w = (b[i + 7] << 8) | b[i + 8];
      return true;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    i += 2 + ((b[i + 2] << 8) | b[i + 3]);
  }
  return false;
}
// Decode a JPEG and fill the portrait screen: scale to the screen height, centre-crop the width.
bool decodeToScreen(const uint8_t *jpeg, size_t len, int width, int height, uint16_t *out, jpg_scale_t scale) {
  const int div = scale == JPG_SCALE_8X ? 8 : scale == JPG_SCALE_4X ? 4 : scale == JPG_SCALE_2X ? 2 : 1;
  const int dw = width / div, dh = height / div;
  const size_t needed = dw * dh * 2;
  if (needed > rgbCapacity) {
    free(rgbScratch);
    rgbScratch = (uint8_t *)ps_malloc(needed);
    rgbCapacity = rgbScratch ? needed : 0;
  }
  if (!rgbScratch || !out || !jpg2rgb565(jpeg, len, rgbScratch, scale)) return false;
  const int cropW = min(dw, dh * W / H), left = (dw - cropW) / 2;
  for (int y = 0; y < H; ++y) {
    const int sy = y * dh / H;
    for (int x = 0; x < W; ++x) {
      const size_t o = (sy * dw + left + x * cropW / W) * 2;
      out[y * W + x] = rgbScratch[o] | (uint16_t(rgbScratch[o + 1]) << 8);
    }
  }
  return true;
}
bool decodeStored(const uint8_t *jpeg, size_t len, uint16_t *out) {
  int w, h;
  return jpegSize(jpeg, len, w, h) && decodeToScreen(jpeg, len, w, h, out, w >= 1024 ? JPG_SCALE_4X : JPG_SCALE_2X);
}
// 64x64 thumbnail from the centre square of a 240x284 screen image.
void makeThumb(const uint16_t *src, uint16_t *out) {
  const int top = (H - W) / 2;
  for (int y = 0; y < THUMB; ++y)
    for (int x = 0; x < THUMB; ++x) out[y * THUMB + x] = src[(top + y * W / THUMB) * W + x * W / THUMB];
}
bool loadPhotoInto(uint32_t id, uint16_t *out) {
  if (id == latestPhotoId && jpegBytes) {
    memcpy(out, photoPixels, W * H * 2);
    return true;
  }
  uint8_t *jpeg;
  size_t len;
  if (!loadPhoto(id, jpeg, len)) return false;
  const bool ok = decodeStored(jpeg, len, out);
  free(jpeg);
  return ok;
}

// ---------- camera ----------
void stopPreview() {
  if (!previewActive) return;
  sensor_t *sensor = esp_camera_sensor_get();
  sensor->set_framesize(sensor, stillSize);
  sensor->set_quality(sensor, stillQuality);
  previewActive = false;
  for (int i = 0; i < 2; ++i) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
  }
}
bool ensurePreviewMode() {
  if (previewActive) return true;
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return false;
  stillSize = sensor->status.framesize;
  stillQuality = sensor->status.quality;
  if (sensor->set_framesize(sensor, PREVIEW_SIZE) || sensor->set_quality(sensor, PREVIEW_QUALITY)) {
    sensor->set_framesize(sensor, stillSize);
    sensor->set_quality(sensor, stillQuality);
    return false;
  }
  previewActive = true;
  for (int i = 0; i < 2; ++i) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
  }
  return true;
}
bool sendAcknowledged(const uint8_t *bytes, size_t length) {
  size_t offset = 0;
  unsigned long started = millis();
  while (offset < length && millis() - started < 12000) {
    offset += Serial.write(bytes + offset, min((size_t)256, length - offset));
    delay(2);
  }
  return offset == length;
}

// ---------- navigation ----------
void refreshDynamic();
lv_obj_t *layer(Screen s) { return scr[(int)s]; }
void setY(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, v); }
// Motion: ease-out when something arrives, ease-in when it leaves (design/index.html --ease).
void animY(lv_obj_t *o, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done) {
  lv_anim_delete(o, setY);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, path);
  lv_anim_set_exec_cb(&a, setY);
  if (done) lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}
// Only the current layer stays visible once motion has finished.
void settleLayers(lv_anim_t * = nullptr) {
  for (int i = 0; i < (int)Screen::Count; ++i) {
    if (i == (int)current) continue;
    lv_anim_delete(scr[i], setY);
    lv_obj_set_y(scr[i], 0);
    lv_obj_add_flag(scr[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_y(layer(current), 0);
}
void onEnter(Screen s);
void enter(Screen next) {
  if (next != Screen::Camera) stopPreview();
  current = next;
  onEnter(next);
  refreshDynamic();
  hide(homeBar, next == Screen::Face || next == Screen::Apps);
}
// Opening something: it rises a short distance into place over what was there.
void show(Screen next) {
  if (next == current) {
    enter(next);
    return;
  }
  lv_obj_t *to = layer(next);
  lv_obj_remove_flag(to, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(to);
  if (ccOpen) lv_obj_move_foreground(cc);
  enter(next);
  animY(to, 28, 0, 200, lv_anim_path_ease_out, settleLayers);
}
void notice(const String &message, Screen back) {
  lv_label_set_text(noticeText, message.c_str());
  noticeReturn = back;
  show(Screen::Notice);
}
// Going home: the app lifts away and the face is underneath.
void leaveTo(Screen under, int fromY) {
  lv_obj_t *top = layer(current), *below = layer(under);
  lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(below, 0);
  lv_obj_move_foreground(top);
  enter(under);
  animY(top, fromY, -H, 180, lv_anim_path_ease_in, settleLayers);
}
void openApps(int fromY = H) {
  lv_obj_t *grid = layer(Screen::Apps);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(grid);
  enter(Screen::Apps);
  animY(grid, fromY, 0, 220, lv_anim_path_ease_out, settleLayers);
}
void goHome() {
  // Same gesture everywhere: from an app to the face, from the face to the app grid.
  if (current == Screen::Face) openApps();
  else leaveTo(Screen::Face, 0);
}

// Interactive home gesture: the layer follows the finger, then commits or springs back.
bool dragging = false;
void dragHome(int dy) {
  if (!dragging) {
    dragging = true;
    if (current == Screen::Face) {
      lv_obj_t *grid = layer(Screen::Apps);
      lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(grid);
    } else {
      lv_obj_t *face = layer(Screen::Face);
      lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_y(face, 0);
      lv_obj_move_foreground(layer(current));
    }
  }
  if (current == Screen::Face) lv_obj_set_y(layer(Screen::Apps), H - dy);
  else lv_obj_set_y(layer(current), -dy);
}
void releaseHome(int dy, int speed) {
  if (!dragging) return;
  dragging = false;
  const bool commit = dy > COMMIT_DRAG || (dy > 24 && speed > 600);
  if (current == Screen::Face) {
    if (commit) openApps(H - dy);
    else animY(layer(Screen::Apps), H - dy, H, 200, lv_anim_path_ease_out, settleLayers);
  } else {
    if (commit) leaveTo(Screen::Face, -dy);
    else animY(layer(current), -dy, 0, 220, lv_anim_path_ease_out, settleLayers);
  }
}

// Control Center: pulled down from the top edge over whatever is on screen.
void ccClosed(lv_anim_t *) { lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN); }
void openControl(int fromY = -H) {
  ccOpen = true;
  refreshDynamic();
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cc);
  animY(cc, fromY, 0, 220, lv_anim_path_ease_out, nullptr);
}
void closeControl(int fromY = 0) {
  if (!ccOpen) return;
  ccOpen = false;
  animY(cc, fromY, -H, 180, lv_anim_path_ease_in, ccClosed);
}
void dragControl(int dy) {
  if (!ccOpen && !lv_obj_has_flag(cc, LV_OBJ_FLAG_HIDDEN)) return;
  refreshDynamic();
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cc);
  lv_obj_set_y(cc, min(0, -H + dy));
}
void releaseControl(int dy, int speed) {
  if (dy > COMMIT_DRAG || (dy > 24 && speed < -600)) openControl(-H + dy);
  else {
    ccOpen = true;
    closeControl(-H + dy);
  }
}

// ---------- content ----------
void loadLatestAnswer() {
  answerCount = listAnswers(answerIds, ANSWER_KEEP);
  lastAnswer = "";
  lastInfo = AnswerInfo();
  if (answerCount) loadAnswer(answerIds[0], lastAnswer, lastInfo);
  historyDirty = true;
}
void showAnswer(uint32_t id) {
  String body;
  AnswerInfo info;
  if (!loadAnswer(id, body, info)) {
    notice("That answer could not be read.", Screen::History);
    return;
  }
  // Answer first: the first sentence (or line) is set large, the rest as body text.
  int cut = body.indexOf('\n');
  const int period = body.indexOf(". ");
  if (period >= 0 && period < 140 && (cut < 0 || period < cut)) cut = period + 1;
  if (cut > 160) cut = -1;
  String lead = cut > 0 ? body.substring(0, cut) : "";
  String rest = cut > 0 ? body.substring(cut) : body;
  rest.trim();
  lv_label_set_text(answerLead, lead.c_str());
  hide(answerLead, lead.isEmpty());
  lv_label_set_text(answerBody, rest.c_str());
  String meta = String("Answered by ") + (info.gemini ? "Gemini" : "GPT");
  const String when = ago(info.when);
  if (when.length()) meta += ", " + when;
  lv_label_set_text(answerMeta, meta.c_str());
  const bool photo = info.photoId && loadPhotoInto(info.photoId, galleryPixels);
  if (photo) lv_image_cache_drop(&galleryDsc);
  hide(answerPhoto, !photo);
  lv_obj_scroll_to_y(answerScroll, 0, LV_ANIM_OFF);
  show(Screen::Answer);
}

// Smart Stack: glanceable cards; swipe the card to cycle, tap to open.
enum class Card { Answer, Photo, Offline };
std::vector<Card> cards;
int cardIndex = 0;
void renderCard() {
  cards.clear();
  if (!networkConnected()) cards.push_back(Card::Offline);
  if (!lastAnswer.isEmpty()) cards.push_back(Card::Answer);
  if (jpegBytes) cards.push_back(Card::Photo);
  if (cards.empty()) cards.push_back(Card::Answer);
  cardIndex = constrain(cardIndex, 0, (int)cards.size() - 1);
  const Card c = cards[cardIndex];
  const bool photo = c == Card::Photo;
  hide(cardImage, !photo);
  hide(cardScrim, !photo);
  hide(cardValue, photo);
  hide(cardRing, c != Card::Answer);
  if (c == Card::Offline) {
    lv_label_set_text(cardKey, "Wi-Fi");
    lv_label_set_text(cardValue, networkEnabled() ? "Not connected. Camera and Photos work offline."
                                                  : "Wi-Fi is off. Turn it on in Control Center.");
  } else if (c == Card::Photo) {
    lv_label_set_text(cardKey, "Last photo");
  } else if (!lastAnswer.isEmpty()) {
    const String when = ago(lastInfo.when);
    lv_label_set_text(cardKey, when.length() ? ("Last answer, " + when).c_str() : "Last answer");
    lv_label_set_text(cardValue, lastAnswer.c_str());
  } else {
    lv_label_set_text(cardKey, "Ask");
    lv_label_set_text(cardValue, "Point at a question, then open Ask.");
  }
  lv_obj_set_style_text_color(cardKey, photo ? INK : MIST, 0);
  lv_obj_align(cardKey, photo ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_LEFT, c == Card::Answer ? 16 : 0, 0);
  // Dots on the right edge: one per card, the current one longer.
  lv_obj_clean(cardDots);
  hide(cardDots, cards.size() < 2);
  for (size_t i = 0; i < cards.size(); ++i) {
    lv_obj_t *d = plain(cardDots);
    lv_obj_set_size(d, 5, (int)i == cardIndex ? 12 : 5);
    lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_color(d, INK, 0);
    lv_obj_set_style_bg_opa(d, (int)i == cardIndex ? LV_OPA_COVER : LV_OPA_30, 0);
  }
}
void cycleCard(int step) {
  const int next = constrain(cardIndex + step, 0, (int)cards.size() - 1);
  if (next == cardIndex) return;
  cardIndex = next;
  renderCard();
  // The card is bottom-aligned; its y is an offset from that anchor.
  animY(card, -ABOVE_HOME + (step > 0 ? 18 : -18), -ABOVE_HOME, 200, lv_anim_path_ease_out, nullptr);
}

void refreshDynamic() {
  const bool online = networkConnected();
  const time_t now = time(nullptr);
  if (clockKnown()) {
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M", &t);
    lv_label_set_text(clockLabel, buf[0] == '0' ? buf + 1 : buf);
    strftime(buf, sizeof(buf), "%a %b %e", &t);
    lv_label_set_text(dateLabel, buf);
  } else {
    lv_label_set_text(clockLabel, "");
    lv_label_set_text(dateLabel, "");
  }
  hide(faceOffline, online);
  hide(askOffline, online || !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN));
  renderCard();
  // Ask: offline is stated on the button itself, not hidden.
  const bool photo = jpegBytes > 0;
  hide(thumb, !photo || cameraOff);
  hide(askPhoto, !photo);
  hide(askEmpty, photo);
  hide(askHint, !photo || !online);
  lv_label_set_text(askButtonLabel, !online ? "Ask when online" : photo ? "Ask about this" : "Capture and ask");
  lv_obj_set_style_opa(askPill, online ? LV_OPA_COVER : LV_OPA_60, 0);
  // Camera privacy
  hide(viewfinder, cameraOff);
  hide(shutter, cameraOff);
  hide(cameraOffLabel, !cameraOff);
  // Settings and Model
  const char *model = useGemini ? "Gemini" : "GPT";
  lv_label_set_text(modelValue, model);
  lv_label_set_text(wifiValue, !networkEnabled() ? "Off" : online ? WiFi.SSID().c_str() : "Not connected");
  const bool keys[2] = {settings.isKey("gemini-key"), settings.isKey("gpt-key")};
  for (int i = 0; i < 2; ++i) {
    lv_label_set_text(modelCheck[i], (i == 0) == useGemini ? LV_SYMBOL_OK : "");
    lv_label_set_text(modelSub[i], keys[i] ? "Key saved" : "No key saved");
  }
  lv_label_set_text(storageValue, (String(storageFreeBytes() / 1024) + " KB free").c_str());
  lv_label_set_text(storageSub, (String(photoCount) + " photos, " + answerCount + " answers on the device").c_str());
  // Control Center
  auto toggle = [](lv_obj_t *t, bool on) {
    lv_obj_set_style_bg_color(t, on ? INK : ICON_BG, 0);
    lv_obj_t *glyph = lv_obj_get_child(t, 0);
    if (glyph) lv_obj_set_style_text_color(glyph, on ? VOID_ : INK, 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(t); ++i) {
      lv_obj_t *c = lv_obj_get_child(t, i);
      lv_obj_set_style_border_color(c, on ? VOID_ : INK, 0);
      lv_obj_set_style_bg_color(c, on ? VOID_ : INK, 0);
    }
  };
  toggle(ccWifi, networkEnabled());
  toggle(ccBright, brightnessLevel == 0);
  toggle(ccCamera, cameraOff);
  toggle(ccModel, false);
  lv_label_set_text(lv_obj_get_child(ccModel, 0), useGemini ? "G" : "GPT");
  lv_label_set_text(ccBrightLabel, brightnessLevel == 0 ? "100%" : brightnessLevel == 1 ? "55%" : "20%");
}

// ---------- actions ----------
void flashOnce() {
  lv_obj_remove_flag(flash, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, flash);
  lv_anim_set_values(&a, 220, 0);
  lv_anim_set_duration(&a, 260);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, v, 0); });
  lv_anim_set_completed_cb(&a, [](lv_anim_t *x) { lv_obj_add_flag((lv_obj_t *)x->var, LV_OBJ_FLAG_HIDDEN); });
  lv_anim_start(&a);
}
void refreshPhotos() { photoCount = listPhotos(photoIds, MAX_PHOTOS); }
// Returns true when a new photo replaced the old one. Failures keep the previous photo.
bool takePhoto(String &problem) {
  if (cameraOff) {
    problem = "Camera is off. Turn it on in Control Center.";
    return false;
  }
  stopPreview();
  if (!esp_camera_sensor_get()) {
    problem = "Camera is unavailable.";
    return false;
  }
  camera_fb_t *queued = esp_camera_fb_get();
  if (queued) esp_camera_fb_return(queued);
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    problem = "No camera frame. Try again.";
    return false;
  }
  if (frame->format != PIXFORMAT_JPEG || frame->len < 4 || frame->buf[0] != 0xff || frame->buf[1] != 0xd8 ||
      frame->buf[frame->len - 2] != 0xff || frame->buf[frame->len - 1] != 0xd9) {
    esp_camera_fb_return(frame);
    problem = "Incomplete photo. Try again.";
    return false;
  }
  Serial.printf("CAPTURE_FRAME %u %u %u\n", frame->width, frame->height, frame->len);
  uint8_t *jpegTarget = frame->len > jpegCapacity ? (uint8_t *)ps_malloc(frame->len) : savedJpeg;
  uint16_t *decoded = (uint16_t *)ps_malloc(W * H * 2);
  const bool ok = jpegTarget && decoded &&
                  decodeToScreen(frame->buf, frame->len, frame->width, frame->height, decoded, JPG_SCALE_4X);
  if (!ok) {
    if (jpegTarget != savedJpeg) free(jpegTarget);
    free(decoded);
    esp_camera_fb_return(frame);
    problem = "Image decode failed.";
    return false;
  }
  if (jpegTarget != savedJpeg) {
    free(savedJpeg);
    savedJpeg = jpegTarget;
    jpegCapacity = frame->len;
  }
  memcpy(savedJpeg, frame->buf, frame->len);
  jpegBytes = frame->len;
  memcpy(photoPixels, decoded, W * H * 2);
  free(decoded);
  esp_camera_fb_return(frame);
  static uint16_t thumbPixels[THUMB * THUMB];  // 8 KB: too big for the loop task stack
  makeThumb(photoPixels, thumbPixels);
  latestPhotoId = savePhoto(savedJpeg, jpegBytes, thumbPixels);
  if (!latestPhotoId) Serial.println("STORAGE_ERROR Photo not saved");
  refreshPhotos();
  lv_image_cache_drop(&photoDsc);
  lv_obj_invalidate(root);
  refreshDynamic();
  return true;
}
void startAsk() {
  if (!jpegBytes) {
    notice("Take a photo first.", Screen::Ask);
    return;
  }
  if (!networkConnected()) {
    notice("Not connected to Wi-Fi. Nothing was sent; the photo is kept.", Screen::Ask);
    return;
  }
  if (aiBusy()) {
    notice("The previous request is still finishing. Try again in a moment.", Screen::Ask);
    return;
  }
  String key = settings.getString(useGemini ? "gemini-key" : "gpt-key", "");
  const bool started = aiStart(useGemini, key, savedJpeg, jpegBytes);
  key = "";
  if (!started) {
    notice("Could not start the request. Not enough memory.", Screen::Ask);
    return;
  }
  pendingGemini = useGemini;
  pendingPhotoId = latestPhotoId;
  lv_label_set_text(busyLabel, useGemini ? "Asking Gemini" : "Asking GPT");
  lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
  show(Screen::Ask);
}
void captureAndAsk() {
  String problem;
  show(Screen::Ask);
  if (!takePhoto(problem)) {
    notice(problem, Screen::Ask);
    return;
  }
  startAsk();
}
void cancelAsk() {
  aiCancel();
  lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
  refreshDynamic();
}
void setModel(bool gemini) {
  useGemini = gemini;
  settings.putBool("gemini", useGemini);
  refreshDynamic();
}
void setBrightness(uint8_t level) {
  brightnessLevel = level % 3;
  settings.putUChar("bright", brightnessLevel);
  displayBrightness(BRIGHTNESS[brightnessLevel]);
  lv_slider_set_value(brightSlider, BRIGHTNESS[brightnessLevel], LV_ANIM_OFF);
  refreshDynamic();
}
void setCameraOff(bool off) {
  cameraOff = off;
  settings.putBool("camera-off", off);
  if (off) stopPreview();
  refreshDynamic();
}
void showPhoto(int index) {
  if (!photoCount) return;
  photoIndex = constrain(index, 0, photoCount - 1);
  if (loadPhotoInto(photoIds[photoIndex], galleryPixels)) {
    lv_image_cache_drop(&galleryDsc);
    lv_obj_invalidate(photosImage);
  }
  // Position appears briefly, then fades: it is only needed while moving through photos.
  lv_label_set_text(photoCounter, (String(photoIndex + 1) + " of " + photoCount).c_str());
  lv_anim_delete(photoCounter, nullptr);
  lv_obj_set_style_opa(photoCounter, LV_OPA_COVER, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, photoCounter);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_delay(&a, 1200);
  lv_anim_set_duration(&a, 300);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); });
  lv_anim_start(&a);
}
void rebuildHistory() {
  if (!historyDirty) return;
  historyDirty = false;
  lv_obj_clean(historyList);
  hide(historyEmpty, answerCount > 0);
  lv_obj_t *t = text(historyList, "History", F_LARGE, INK);
  lv_obj_set_style_margin_left(t, 22, 0);
  lv_obj_set_style_margin_bottom(t, 6, 0);
  for (int i = 0; i < answerCount; ++i) {
    String body;
    AnswerInfo info;
    if (!loadAnswer(answerIds[i], body, info)) continue;
    lv_obj_t *r = pressedFeedback(plain(historyList));
    lv_obj_set_size(r, W, 56);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r, i ? 1 : 0, 0);
    lv_obj_set_style_border_color(r, LINE, 0);
    uint16_t *th = historyThumbs + i * THUMB * THUMB;
    int textX = 18;
    if (loadAnswerThumb(answerIds[i], th)) {
      lv_image_cache_drop(&historyDsc[i]);
      setupImage(historyDsc[i], th, THUMB, THUMB);
      lv_obj_t *img = lv_image_create(r);
      lv_image_set_src(img, &historyDsc[i]);
      lv_obj_set_size(img, 40, 40);
      lv_image_set_scale(img, 256 * 40 / THUMB);
      lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
      lv_obj_set_style_radius(img, 8, 0);
      lv_obj_set_style_clip_corner(img, true, 0);
      lv_obj_align(img, LV_ALIGN_LEFT_MID, 18, 0);
      textX = 70;
    }
    body.replace("\n", " ");
    lv_obj_t *one = text(r, body.c_str(), F_BODY, INK);
    lv_label_set_long_mode(one, LV_LABEL_LONG_DOT);
    lv_obj_set_size(one, W - textX - 16, 20);
    lv_obj_set_pos(one, textX, 9);
    String meta = String(info.gemini ? "Gemini" : "GPT");
    const String when = ago(info.when);
    if (when.length()) meta += ", " + when;
    lv_obj_t *two = text(r, meta.c_str(), F_SMALL, MIST);
    lv_obj_set_pos(two, textX, 33);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        r, [](lv_event_t *e) { showAnswer((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED,
        (void *)(uintptr_t)answerIds[i]);
  }
}
void onEnter(Screen s) {
  if (s == Screen::Photos) {
    hide(photosImage, photoCount == 0);
    hide(photosEmpty, photoCount != 0);
    showPhoto(0);
  } else if (s == Screen::History) {
    rebuildHistory();
    lv_obj_scroll_to_y(historyList, 0, LV_ANIM_OFF);
  } else if (s == Screen::Apps) {
    lv_obj_scroll_to_y(layer(Screen::Apps), 0, LV_ANIM_OFF);
  }
}

// ---------- screens ----------
void buildFace() {
  lv_obj_t *s = scr[(int)Screen::Face] = screenBase();
  clockLabel = text(s, "", F_CLOCK, INK);
  lv_obj_set_pos(clockLabel, 20, 18);
  dateLabel = text(s, "", F_BODY, MIST);
  lv_obj_set_pos(dateLabel, 24, 76);
  faceOffline = text(s, "Offline", F_SMALL, MIST);
  lv_obj_align(faceOffline, LV_ALIGN_TOP_RIGHT, -26, 28);
  card = pressedFeedback(plain(s));
  lv_obj_set_size(card, 216, 108);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_radius(card, 26, 0);
  lv_obj_set_style_clip_corner(card, true, 0);
  lv_obj_set_style_bg_color(card, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(card, 16, 0);
  lv_obj_set_style_pad_ver(card, 14, 0);
  // Card swipes cycle the stack instead of opening the grid.
  lv_obj_remove_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
  cardImage = lv_image_create(card);
  lv_image_set_src(cardImage, &photoDsc);
  lv_obj_add_flag(cardImage, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_style_margin_all(cardImage, 0, 0);
  lv_obj_set_pos(cardImage, -16, -14);
  lv_obj_set_size(cardImage, 216, 108);
  lv_image_set_inner_align(cardImage, LV_IMAGE_ALIGN_CENTER);
  lv_obj_remove_flag(cardImage, LV_OBJ_FLAG_CLICKABLE);
  cardScrim = scrimBottom(card, 60);
  lv_obj_set_style_margin_bottom(cardScrim, -14, 0);
  lv_obj_set_width(cardScrim, 216);
  lv_obj_align(cardScrim, LV_ALIGN_BOTTOM_MID, 0, 14);
  cardRing = circle(card, 10, LENS, 2, VOID_, LV_OPA_TRANSP);
  lv_obj_set_pos(cardRing, 0, 3);
  cardKey = text(card, "", F_SMALL, MIST);
  cardValue = text(card, "", F_BODY, INK);
  lv_obj_set_width(cardValue, 174);
  lv_obj_set_height(cardValue, 60);
  lv_label_set_long_mode(cardValue, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(cardValue, 0, 22);
  cardDots = plain(card);
  lv_obj_set_size(cardDots, 6, 40);
  lv_obj_align(cardDots, LV_ALIGN_RIGHT_MID, 4, 0);
  lv_obj_set_flex_flow(cardDots, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cardDots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(cardDots, 4, 0);
  onClick(card, [] {
    const Card c = cards.empty() ? Card::Answer : cards[cardIndex];
    if (c == Card::Offline) openControl();
    else if (c == Card::Photo) show(Screen::Photos);
    else if (!lastAnswer.isEmpty()) showAnswer(answerIds[0]);
    else show(Screen::Ask);
  });
  lv_obj_add_event_cb(
      card,
      [](lv_event_t *) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) cycleCard(1);
        else if (dir == LV_DIR_BOTTOM) cycleCard(-1);
      },
      LV_EVENT_GESTURE, nullptr);
  // Swipe up anywhere else on the face opens the app grid.
  lv_obj_add_event_cb(
      s,
      [](lv_event_t *) {
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP && current == Screen::Face) openApps();
      },
      LV_EVENT_GESTURE, nullptr);
}

// Icons shrink toward the rounded edges of the screen, like watchOS.
void fisheye() {
  lv_obj_t *grid = layer(Screen::Apps);
  const int scroll = lv_obj_get_scroll_y(grid);
  for (lv_obj_t *icon : icons) {
    const float cx = lv_obj_get_x(icon) + 31, cy = lv_obj_get_y(icon) + 31 - scroll;
    const float d = sqrtf(powf((cx - 120) / 120, 2) + powf((cy - 142) / 142, 2));
    const float scale = constrain(1.25f - d * 0.55f, 0.55f, 1.0f);
    lv_obj_set_style_transform_scale(icon, (int)(256 * scale), 0);
  }
}
void appIcon(lv_obj_t *parent, int x, int y, const char *name, lv_color_t bg, void (*open)(),
             void (*glyph)(lv_obj_t *)) {
  lv_obj_t *b = circle(parent, 62, bg, 0, bg, LV_OPA_COVER);
  lv_obj_set_pos(b, x - 31, y - 31);
  lv_obj_set_style_transform_pivot_x(b, 31, 0);
  lv_obj_set_style_transform_pivot_y(b, 31, 0);
  lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  glyph(b);
  onClick(b, open);
  icons.push_back(b);
  // The name shows at the top only while a finger is on the icon (no labels under icons).
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        lv_label_set_text(iconName, (const char *)lv_event_get_user_data(e));
        lv_obj_remove_flag(iconName, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_PRESSED, (void *)name);
  auto clear = [](lv_event_t *) { lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN); };
  lv_obj_add_event_cb(b, clear, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(b, clear, LV_EVENT_PRESS_LOST, nullptr);
}
void glyphCamera(lv_obj_t *b) {
  lv_obj_t *body = plain(b);
  lv_obj_set_size(body, 30, 22);
  lv_obj_center(body);
  lv_obj_set_style_radius(body, 6, 0);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, INK, 0);
  lv_obj_t *lens = circle(body, 11, INK, 2, VOID_, LV_OPA_TRANSP);
  lv_obj_center(lens);
}
void glyphAsk(lv_obj_t *b) {
  lv_obj_t *outer = circle(b, 26, VOID_, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_center(outer);
  lv_obj_t *inner = circle(b, 9, VOID_, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_center(inner);
}
lv_obj_t *glyphSymbol(lv_obj_t *b, const char *symbol, lv_color_t color) {
  lv_obj_t *l = text(b, symbol, F_LARGE, color);
  lv_obj_center(l);
  return l;
}
void planned(const char *name) {
  notice(String(name) + " is planned for the gesture and remote-control update.", Screen::Apps);
}
void buildApps() {
  lv_obj_t *s = scr[(int)Screen::Apps] = screenBase();
  lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
  // Honeycomb rows (design/index.html): 3, 2 offset, then planned apps.
  appIcon(s, 50, 62, "Camera", ICON_BG, [] { show(Screen::Camera); }, glyphCamera);
  appIcon(s, 120, 62, "Ask", LENS, [] { show(Screen::Ask); }, glyphAsk);
  appIcon(
      s, 190, 62, "Photos", ICON_BG, [] { show(Screen::Photos); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_IMAGE, INK); });
  appIcon(
      s, 85, 128, "History", ICON_BG, [] { show(Screen::History); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_LIST, INK); });
  appIcon(
      s, 155, 128, "Settings", ICON_BG, [] { show(Screen::Settings); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_SETTINGS, INK); });
  appIcon(
      s, 50, 194, "Gestures", ICON_DIM, [] { planned("Gestures"); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_EYE_OPEN, MIST); });
  appIcon(
      s, 120, 194, "Remote", ICON_DIM, [] { planned("Remote"); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_KEYBOARD, MIST); });
  lv_obj_t *spacer = plain(s);  // room to scroll, as more apps arrive
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, 330);
  iconName = text(s, "", F_SMALL, INK);
  lv_obj_align(iconName, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s, [](lv_event_t *) { fisheye(); }, LV_EVENT_SCROLL, nullptr);
  lv_obj_update_layout(s);
  fisheye();
}

void buildCamera() {
  lv_obj_t *s = scr[(int)Screen::Camera] = screenBase();
  viewfinder = lv_image_create(s);
  lv_image_set_src(viewfinder, &liveDsc);
  scrimBottom(s, 110);
  cameraOffLabel = text(s, "Camera is off.\nTurn it on in Control Center.", F_BODY, MIST);
  lv_obj_set_style_text_align(cameraOffLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(cameraOffLabel, LV_ALIGN_CENTER, 0, -20);
  // Shutter: one ring. Press fills it, so the feedback lands on touch-down.
  shutter = circle(s, 60, INK, 4, INK, LV_OPA_TRANSP);
  lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_bg_opa(shutter, 215, LV_STATE_PRESSED);
  onClick(shutter, [] {
    flashOnce();
    lv_refr_now(display);
    String problem;
    if (!takePhoto(problem)) notice(problem, Screen::Camera);
  });
  thumb = lv_image_create(s);
  lv_image_set_src(thumb, &photoDsc);
  lv_obj_set_size(thumb, 40, 40);
  // Scale the 240x284 photo to the thumbnail width and centre-crop the height.
  lv_image_set_scale(thumb, 256 * 40 / W + 1);
  lv_image_set_inner_align(thumb, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_style_radius(thumb, 9, 0);
  lv_obj_set_style_clip_corner(thumb, true, 0);
  lv_obj_set_style_border_width(thumb, 2, 0);
  lv_obj_set_style_border_color(thumb, INK, 0);
  lv_obj_set_style_border_opa(thumb, LV_OPA_70, 0);
  lv_obj_align(thumb, LV_ALIGN_BOTTOM_LEFT, 26, -ABOVE_HOME - 10);
  onClick(thumb, [] { show(Screen::Photos); });
  flash = plain(s);
  lv_obj_set_size(flash, W, H);
  lv_obj_set_style_bg_color(flash, INK, 0);
  lv_obj_set_style_bg_opa(flash, 0, 0);
  lv_obj_add_flag(flash, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(flash, LV_OBJ_FLAG_CLICKABLE);
}

void buildAsk() {
  lv_obj_t *s = scr[(int)Screen::Ask] = screenBase();
  askPhoto = lv_image_create(s);
  lv_image_set_src(askPhoto, &photoDsc);
  askEmpty = text(s, "No photo yet.", F_BODY, MIST);
  lv_obj_align(askEmpty, LV_ALIGN_CENTER, 0, -30);
  scrimBottom(s, 120);
  askOffline = text(s, "Offline", F_SMALL, INK);
  lv_obj_align(askOffline, LV_ALIGN_TOP_MID, 0, 26);
  askHint = text(s, "Hold for a new photo", F_SMALL, MIST);
  lv_obj_align(askHint, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME - 56);
  askPill = pressedFeedback(plain(s));
  lv_obj_set_size(askPill, 200, 46);
  lv_obj_align(askPill, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_radius(askPill, 23, 0);
  lv_obj_set_style_bg_color(askPill, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(askPill, 230, 0);
  lv_obj_t *ring = circle(askPill, 28, LENS, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_align(ring, LV_ALIGN_LEFT_MID, 9, 0);
  askButtonLabel = text(askPill, "", F_BODY, INK);
  lv_obj_align(askButtonLabel, LV_ALIGN_LEFT_MID, 46, 0);
  onClick(askPill, [] {
    if (jpegBytes) startAsk();
    else captureAndAsk();
  });
  // Long-press anywhere: new photo, then ask. Replaces the tiny corner icon of the old UI.
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      s,
      [](lv_event_t *) {
        if (lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN) && !cameraOff) lv_obj_remove_flag(askSheet, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_LONG_PRESSED, nullptr);

  askSheet = plain(s);
  lv_obj_set_size(askSheet, 220, 96);
  lv_obj_align(askSheet, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME + 4);
  lv_obj_set_style_radius(askSheet, 22, 0);
  lv_obj_set_style_bg_color(askSheet, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(askSheet, 245, 0);
  lv_obj_add_flag(askSheet, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *newPhoto = pressedFeedback(plain(askSheet));
  lv_obj_set_size(newPhoto, 220, 48);
  lv_obj_center(text(newPhoto, "Take new photo and ask", F_BODY, INK));
  onClick(newPhoto, [] {
    lv_obj_add_flag(askSheet, LV_OBJ_FLAG_HIDDEN);
    captureAndAsk();
  });
  lv_obj_t *keep = pressedFeedback(plain(askSheet));
  lv_obj_set_size(keep, 220, 48);
  lv_obj_set_pos(keep, 0, 48);
  lv_obj_set_style_border_side(keep, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(keep, 1, 0);
  lv_obj_set_style_border_color(keep, LINE, 0);
  lv_obj_center(text(keep, "Keep this photo", F_BODY, INK));
  onClick(keep, [] { lv_obj_add_flag(askSheet, LV_OBJ_FLAG_HIDDEN); });

  // Thinking state: calm breathing ring, honest label, always cancellable.
  busy = plain(s);
  lv_obj_set_size(busy, W, H);
  lv_obj_set_style_bg_color(busy, VOID_, 0);
  lv_obj_set_style_bg_opa(busy, 160, 0);
  lv_obj_add_flag(busy, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
  busyRing = circle(busy, 64, LENS, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_align(busyRing, LV_ALIGN_CENTER, 0, -36);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, busyRing);
  lv_anim_set_values(&a, 64, 50);
  lv_anim_set_duration(&a, 800);
  lv_anim_set_playback_duration(&a, 800);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_size((lv_obj_t *)o, v, v); });
  lv_anim_start(&a);
  busyLabel = text(busy, "", F_BODY, INK);
  lv_obj_align(busyLabel, LV_ALIGN_CENTER, 0, 24);
  lv_obj_t *cancel = pressedFeedback(plain(busy));
  lv_obj_set_size(cancel, 140, 44);
  lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_center(text(cancel, "Cancel", F_BODY, MIST));
  onClick(cancel, cancelAsk);
}

void buildAnswer() {
  lv_obj_t *s = scr[(int)Screen::Answer] = screenBase();
  answerScroll = plain(s);
  lv_obj_set_size(answerScroll, W, H);
  lv_obj_add_flag(answerScroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(answerScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(answerScroll, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(answerScroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(answerScroll, 56, 0);
  lv_obj_set_style_pad_row(answerScroll, 10, 0);
  answerPhoto = lv_image_create(answerScroll);
  lv_image_set_src(answerPhoto, &galleryDsc);
  lv_obj_set_size(answerPhoto, W, 84);
  lv_image_set_inner_align(answerPhoto, LV_IMAGE_ALIGN_CENTER);  // middle band of the photo
  lv_obj_set_style_image_opa(answerPhoto, LV_OPA_80, 0);
  auto para = [](const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = text(answerScroll, "", font, color);
    lv_obj_set_width(l, W - 36);
    lv_obj_set_style_margin_left(l, 18, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 4, 0);
    return l;
  };
  answerLead = para(F_LARGE, INK);
  answerBody = para(F_BODY, INK);
  answerMeta = para(F_SMALL, MIST);
}

void buildPhotos() {
  lv_obj_t *s = scr[(int)Screen::Photos] = screenBase();
  photosImage = lv_image_create(s);
  lv_image_set_src(photosImage, &galleryDsc);
  lv_obj_add_flag(photosImage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(photosImage, LV_OBJ_FLAG_GESTURE_BUBBLE);
  // Swipe left/right through photos, newest first; tap shows the position again.
  lv_obj_add_event_cb(
      photosImage,
      [](lv_event_t *) {
        const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) showPhoto(photoIndex + 1);
        else if (dir == LV_DIR_RIGHT) showPhoto(photoIndex - 1);
      },
      LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(photosImage, [](lv_event_t *) { showPhoto(photoIndex); }, LV_EVENT_CLICKED, nullptr);
  photoCounter = text(s, "", F_SMALL, INK);
  lv_obj_align(photoCounter, LV_ALIGN_TOP_MID, 0, 14);
  photosEmpty = plain(s);
  lv_obj_set_size(photosEmpty, W, H);
  lv_obj_align(text(photosEmpty, "No photos yet.", F_BODY, MIST), LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *open = pill(photosEmpty, "Open Camera", 150);
  lv_obj_align(open, LV_ALIGN_CENTER, 0, 20);
  onClick(open, [] { show(Screen::Camera); });
}

lv_obj_t *title(lv_obj_t *s, const char *value) {
  lv_obj_t *t = text(s, value, F_LARGE, INK);
  lv_obj_set_pos(t, 22, 18);
  return t;
}
lv_obj_t *row(lv_obj_t *s, int y, const char *label, lv_obj_t **value, int height = 52) {
  lv_obj_t *r = pressedFeedback(plain(s));
  lv_obj_set_size(r, W, height);
  lv_obj_set_pos(r, 0, y);
  lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_set_style_border_color(r, LINE, 0);
  lv_obj_t *l = text(r, label, F_BODY, INK);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 22, 0);
  if (value) {
    *value = text(r, "", F_BODY, MIST);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, -20, 0);
  }
  return r;
}

void buildHistory() {
  lv_obj_t *s = scr[(int)Screen::History] = screenBase();
  historyList = plain(s);
  lv_obj_set_size(historyList, W, H);
  lv_obj_add_flag(historyList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(historyList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(historyList, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(historyList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_top(historyList, 18, 0);
  lv_obj_set_style_pad_bottom(historyList, 56, 0);
  historyEmpty = text(s, "No answers yet.", F_BODY, MIST);
  lv_obj_align(historyEmpty, LV_ALIGN_CENTER, 0, 0);
}

void buildSettings() {
  lv_obj_t *s = scr[(int)Screen::Settings] = screenBase();
  lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
  title(s, "Settings");
  row(s, 58, "Wi-Fi", &wifiValue);
  lv_obj_t *model = row(s, 110, "Model", &modelValue);
  onClick(model, [] { show(Screen::Model); });
  lv_obj_t *bright = row(s, 162, "Brightness", nullptr);
  brightSlider = lv_slider_create(bright);
  lv_obj_set_size(brightSlider, 92, 6);
  lv_obj_align(brightSlider, LV_ALIGN_RIGHT_MID, -22, 0);
  lv_slider_set_range(brightSlider, 20, 255);
  lv_obj_set_style_bg_color(brightSlider, LINE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(brightSlider, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightSlider, INK, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(brightSlider, 3, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightSlider, INK, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(brightSlider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(brightSlider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(brightSlider, 6, LV_PART_KNOB);
  lv_obj_set_ext_click_area(brightSlider, 16);
  lv_obj_add_event_cb(
      brightSlider, [](lv_event_t *e) { displayBrightness(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e))); },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *storage = row(s, 214, "Storage", &storageValue, 64);
  lv_obj_align(lv_obj_get_child(storage, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_align(storageValue, LV_ALIGN_TOP_RIGHT, -20, 12);
  storageSub = text(storage, "", F_SMALL, MIST);
  lv_obj_set_pos(storageSub, 22, 36);
  lv_obj_t *spacer = plain(s);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, 330);
}

void buildModel() {
  lv_obj_t *s = scr[(int)Screen::Model] = screenBase();
  title(s, "Model");
  const char *names[2] = {"Gemini", "GPT"};
  for (int i = 0; i < 2; ++i) {
    lv_obj_t *r = row(s, 58 + i * 60, names[i], nullptr, 60);
    lv_obj_align(lv_obj_get_child(r, 0), LV_ALIGN_TOP_LEFT, 22, 10);
    modelSub[i] = text(r, "", F_SMALL, MIST);
    lv_obj_set_pos(modelSub[i], 22, 34);
    modelCheck[i] = text(r, "", F_BODY, LENS);
    lv_obj_align(modelCheck[i], LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          setModel(lv_event_get_user_data(e) == nullptr);
          leaveTo(Screen::Settings, 0);
        },
        LV_EVENT_CLICKED, i == 0 ? nullptr : (void *)1);
  }
}

void buildNotice() {
  lv_obj_t *s = scr[(int)Screen::Notice] = screenBase();
  noticeText = text(s, "", F_BODY, INK);
  lv_obj_set_width(noticeText, W - 48);
  lv_label_set_long_mode(noticeText, LV_LABEL_LONG_WRAP);
  lv_obj_align(noticeText, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *ok = pill(s, "OK", 120);
  lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  onClick(ok, [] { show(noticeReturn); });
}

lv_obj_t *ccToggle(lv_obj_t *parent, const char *label, lv_obj_t **labelOut) {
  lv_obj_t *col = plain(parent);
  lv_obj_set_size(col, 68, 80);
  lv_obj_t *t = circle(col, 56, INK, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_opa(t, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_t *l = text(col, label, F_SMALL, MIST);
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
  if (labelOut) *labelOut = l;
  return t;
}
void buildControl() {
  cc = plain(root);
  lv_obj_set_size(cc, W, H);
  lv_obj_set_style_bg_color(cc, VOID_, 0);
  lv_obj_set_style_bg_opa(cc, 248, 0);
  lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_y(cc, -H);
  lv_obj_t *grab = plain(cc);
  lv_obj_set_size(grab, 36, 4);
  lv_obj_set_style_radius(grab, 2, 0);
  lv_obj_set_style_bg_color(grab, INK, 0);
  lv_obj_set_style_bg_opa(grab, 90, 0);
  lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_t *grid = plain(cc);
  lv_obj_set_size(grid, 170, 180);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(grid, 14, 0);
  ccWifi = ccToggle(grid, "Wi-Fi", nullptr);
  glyphSymbol(ccWifi, LV_SYMBOL_WIFI, INK);
  onClick(ccWifi, [] {
    networkSetEnabled(!networkEnabled());
    refreshDynamic();
  });
  ccBright = ccToggle(grid, "100%", &ccBrightLabel);
  lv_obj_t *sun = circle(ccBright, 14, INK, 2, VOID_, LV_OPA_TRANSP);
  lv_obj_center(sun);
  for (int i = 0; i < 8; ++i) {
    lv_obj_t *ray = circle(ccBright, 3, INK, 0, INK, LV_OPA_COVER);
    lv_obj_align(ray, LV_ALIGN_CENTER, (int)(12 * cosf(i * PI / 4)), (int)(12 * sinf(i * PI / 4)));
  }
  onClick(ccBright, [] { setBrightness(brightnessLevel + 1); });
  ccModel = ccToggle(grid, "Model", &ccModelLabel);
  lv_obj_t *modelGlyph = text(ccModel, "G", F_BODY, INK);
  lv_obj_center(modelGlyph);
  onClick(ccModel, [] { setModel(!useGemini); });
  ccCamera = ccToggle(grid, "Camera off", nullptr);
  glyphSymbol(ccCamera, LV_SYMBOL_EYE_CLOSE, INK);
  onClick(ccCamera, [] { setCameraOff(!cameraOff); });
  lv_obj_t *hint = text(cc, "Swipe up to close", F_SMALL, MIST);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
  // Swipe up on Control Center, or tap outside the toggles, closes it.
  lv_obj_add_event_cb(
      cc,
      [](lv_event_t *) {
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) closeControl();
      },
      LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(
      cc,
      [](lv_event_t *e) {
        if (lv_event_get_target(e) == cc) closeControl();
      },
      LV_EVENT_CLICKED, nullptr);
}

// ---------- LVGL glue ----------
void flush(lv_display_t *d, const lv_area_t *area, uint8_t *) {
  // Direct mode: the framebuffer already holds the full image; push only the dirty rows.
  displayPresent(framebuffer, area->y1, area->y2 + 1);
  lv_display_flush_ready(d);
}
void readTouch(lv_indev_t *, lv_indev_data_t *data) {
  int x = 0, y = 0;
  bool pressed;
  if (injecting) {
    x = injectX;
    y = injectY;
    pressed = true;
  } else pressed = touchRead(x, y);
  static unsigned long moveAt = 0;
  static int speed = 0;  // px/s, smoothed; positive = upward
  if (pressed) {
    const unsigned long now = millis();
    if (!fingerDown) {
      fingerDown = true;
      downX = x;
      downY = y;
      speed = 0;
      // Edges belong to the system: bottom drags home, top pulls Control Center.
      edge = injecting                       ? Edge::None
             : downY >= HOME_ZONE            ? Edge::Home
             : (downY < TOP_ZONE && !ccOpen) ? Edge::Control
                                             : Edge::None;
    } else if (now > moveAt) {
      speed = (speed + (lastY - y) * 1000 / (int)(now - moveAt)) / 2;
    }
    moveAt = now;
    lastX = x;
    lastY = y;
    if (edge == Edge::Home && downY - y > 6) {
      if (ccOpen) closeControl();
      else dragHome(downY - y);
    }
    if (edge == Edge::Control && y - downY > 6) dragControl(y - downY);
  } else if (fingerDown) {
    fingerDown = false;
    if (edge == Edge::Home) releaseHome(max(0, downY - lastY), speed);
    if (edge == Edge::Control) releaseControl(max(0, lastY - downY), speed);
    edge = Edge::None;
  }
  if (edge != Edge::None) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  data->point.x = pressed ? x : lastX;
  data->point.y = pressed ? y : lastY;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
uint32_t tick() { return millis(); }
const char *screenName() {
  if (ccOpen) return "control";
  if (current == Screen::Ask && !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN)) return "busy";
  switch (current) {
    case Screen::Face: return "face";
    case Screen::Apps: return "apps";
    case Screen::Camera: return "home";  // historical mirror name for the camera view
    case Screen::Ask: return "ai";
    case Screen::Answer: return "answer";
    case Screen::Photos: return "photo";
    case Screen::History: return "history";
    case Screen::Settings: return "status";
    case Screen::Model: return "model";
    default: return "error";
  }
}
void settle() {
  // Let LVGL finish transitions so the USB mirror sees the resulting screen, not motion.
  const unsigned long start = millis();
  do {
    lv_timer_handler();
    delay(5);
  } while (lv_anim_count_running() > 1 && millis() - start < 700);
  lv_refr_now(display);
}
void sendFrame() {
  settle();
  const uint8_t *bytes = reinterpret_cast<uint8_t *>(framebuffer);
  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < W * H * 2; ++i) {
    checksum ^= bytes[i];
    checksum *= 16777619u;
  }
  Serial.printf("SCREEN_BEGIN %u %lu %s %d %d %08lx %s %d %d %d\n", W * H * 2, ++sequence, screenName(), 1, 1,
                (unsigned long)checksum, useGemini ? "gemini" : "gpt", settings.isKey("gemini-key"),
                settings.isKey("gpt-key"), current == Screen::Camera && !cameraOff);
  if (!sendAcknowledged(bytes, W * H * 2)) {
    Serial.println("\nSCREEN_ERROR USB write timed out");
    return;
  }
  Serial.println("\nSCREEN_END");
}
void injectTap(int x, int y) {
  injectX = x;
  injectY = y;
  injecting = true;
  for (int i = 0; i < 4; ++i) {
    lv_timer_handler();
    delay(20);
  }
  injecting = false;
  for (int i = 0; i < 4; ++i) {
    lv_timer_handler();
    delay(20);
  }
}
void tuneCamera(char profile) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;
  static bool saved = false;
  static int sharpness, contrast, denoise;
  if (!saved) {
    sharpness = sensor->status.sharpness;
    contrast = sensor->status.contrast;
    denoise = sensor->status.denoise;
    saved = true;
  }
  int errors = 0;
  errors |= sensor->set_framesize(sensor, profile == '5' ? FRAMESIZE_UXGA : FRAMESIZE_QXGA);
  errors |= sensor->set_quality(sensor, profile == '1' ? 8 : profile == '5' ? 1 : 4);
  errors |= sensor->set_contrast(sensor, (profile == '3' || profile == '4') ? 1 : contrast);
  errors |= sensor->set_sharpness(sensor, (profile == '3' || profile == '4') ? 2 : sharpness);
  errors |= sensor->set_denoise(sensor, (profile == '3' || profile == '4') ? 0 : denoise);
  errors |= sensor->set_exposure_ctrl(sensor, profile == '4' ? 0 : 1);
  errors |= sensor->set_gain_ctrl(sensor, 1);
  if (profile == '4') errors |= sensor->set_aec_value(sensor, 300);
  Serial.printf("TUNING profile=%c errors=%d\n", profile, errors);
  for (int i = 0; i < 3; ++i) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
    delay(100);
  }
}
void previewToUsb() {
  if (current != Screen::Camera || cameraOff) {
    Serial.println("CAPTURE FAILED Preview is not active");
    return;
  }
  if (!esp_camera_sensor_get()) {
    Serial.println("CAPTURE FAILED Camera unavailable");
    return;
  }
  if (!ensurePreviewMode()) {
    Serial.println("CAPTURE FAILED Preview setup failed");
    return;
  }
  camera_fb_t *f = esp_camera_fb_get();
  if (!f) {
    Serial.println("CAPTURE FAILED No preview frame");
    return;
  }
  Serial.printf("JPEG_BEGIN %u\n", f->len);
  const bool sent = sendAcknowledged(f->buf, f->len);
  esp_camera_fb_return(f);
  Serial.println(sent ? "\nJPEG_END" : "\nCAPTURE FAILED Preview transfer failed");
}
void restoreLatestPhoto() {
  // The newest stored photo comes back after a reboot, so Ask and the face have it.
  refreshPhotos();
  if (!photoCount) return;
  uint8_t *jpeg;
  size_t len;
  if (!loadPhoto(photoIds[0], jpeg, len)) return;
  if (decodeStored(jpeg, len, photoPixels)) {
    savedJpeg = jpeg;
    jpegBytes = jpegCapacity = len;
    latestPhotoId = photoIds[0];
  } else {
    free(jpeg);
  }
}
}  // namespace

void deviceNoteSerial() { lastSerialMs = millis(); }

void initDeviceUi() {
  settings.begin("tiny-ai", false);
  useGemini = settings.getBool("gemini", true);
  cameraOff = settings.getBool("camera-off", false);
  brightnessLevel = settings.getUChar("bright", 0) % 3;
  if (!storageBegin()) Serial.println("STORAGE_ERROR Flash filesystem unavailable");
  framebuffer = (uint16_t *)ps_malloc(W * H * 2);
  livePixels = (uint16_t *)ps_calloc(W * H, 2);
  photoPixels = (uint16_t *)ps_calloc(W * H, 2);
  galleryPixels = (uint16_t *)ps_calloc(W * H, 2);
  historyThumbs = (uint16_t *)ps_calloc(ANSWER_KEEP * THUMB * THUMB, 2);
  // Screen link speed is stored so it can be tuned for the wiring without reflashing ('Y').
  spiMhz = constrain(settings.getUChar("lcd-mhz", 10), 5, 80);
  const bool lcd = displayBegin(spiMhz * 1000000UL);
  Serial.printf("DISPLAY lcd=%d touch=%d spi=%uMHz\n", lcd, touchAvailable(), spiMhz);
  if (!framebuffer || !livePixels || !photoPixels || !galleryPixels || !historyThumbs) {
    Serial.println("DISPLAY_ERROR Out of PSRAM");
    framebuffer = nullptr;
    return;
  }
  // Answers saved by older firmware in NVS join the history once.
  if (!listAnswers(answerIds, 1)) {
    const String legacy = settings.getString("last-answer", "");
    if (!legacy.isEmpty()) saveAnswer(legacy, settings.getBool("answer-gemini", true), 0, 0, nullptr);
  }
  loadLatestAnswer();
  restoreLatestPhoto();

  VOID_ = lv_color_hex(0x000000);
  GRAPHITE = lv_color_hex(0x1a1b1e);
  ICON_BG = lv_color_hex(0x2a2c31);
  ICON_DIM = lv_color_hex(0x1c1d20);
  LINE = lv_color_hex(0x2a2c30);
  MIST = lv_color_hex(0x8b9097);
  INK = lv_color_hex(0xf3f4f5);
  LENS = lv_color_hex(0xffb547);

  lv_init();
  lv_tick_set_cb(tick);
  display = lv_display_create(W, H);
  lv_display_set_buffers(display, framebuffer, nullptr, W * H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, flush);
  touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, readTouch);
  root = lv_screen_active();
  lv_obj_remove_style_all(root);
  lv_obj_set_style_bg_color(root, VOID_, 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  setupImage(liveDsc, livePixels, W, H);
  setupImage(photoDsc, photoPixels, W, H);
  setupImage(galleryDsc, galleryPixels, W, H);
  buildFace();
  buildApps();
  buildCamera();
  buildAsk();
  buildAnswer();
  buildPhotos();
  buildHistory();
  buildSettings();
  buildModel();
  buildNotice();
  buildControl();
  homeBar = plain(lv_layer_top());
  lv_obj_set_size(homeBar, 40, 4);
  lv_obj_set_style_radius(homeBar, 2, 0);
  lv_obj_set_style_bg_color(homeBar, INK, 0);
  lv_obj_set_style_bg_opa(homeBar, 115, 0);
  lv_obj_align(homeBar, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_add_flag(homeBar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(homeBar, LV_OBJ_FLAG_CLICKABLE);
  lv_slider_set_value(brightSlider, BRIGHTNESS[brightnessLevel], LV_ANIM_OFF);

  lv_obj_remove_flag(layer(Screen::Face), LV_OBJ_FLAG_HIDDEN);
  enter(Screen::Face);
  const uint32_t start = micros();
  lv_refr_now(display);
  Serial.printf("DISPLAY first frame %lu us\n", (unsigned long)(micros() - start));
  if (lcd) displayBrightness(BRIGHTNESS[brightnessLevel]);
}

void handleDeviceButton(char command) {
  if (!framebuffer) return;
  if (command == 'T') {
    String xy = Serial.readStringUntil('\n');
    const int comma = xy.indexOf(',');
    if (comma < 1) {
      Serial.println("SCREEN_ERROR Invalid touch");
      return;
    }
    const int x = xy.substring(0, comma).toInt(), y = xy.substring(comma + 1).toInt();
    if (x >= 0 && x < W && y >= TOP_ZONE && y < HOME_ZONE) injectTap(x, y);
    sendFrame();
    return;
  }
  if (command == 'K') {
    String input = Serial.readStringUntil('\n');
    if (input.length() < 1 || input.length() > 513 || (input[0] != 'G' && input[0] != 'O')) {
      Serial.println("SCREEN_ERROR Invalid key settings");
      return;
    }
    String key = input.substring(1);
    for (size_t i = 0; i < key.length(); ++i) {
      const unsigned char c = key[i];
      if (c < 33 || c > 126) {
        Serial.println("SCREEN_ERROR Invalid key characters");
        return;
      }
    }
    const char *name = input[0] == 'G' ? "gemini-key" : "gpt-key";
    const bool ok = key.isEmpty() ? (!settings.isKey(name) || settings.remove(name))
                                  : settings.putString(name, key) == key.length();
    key = "";
    input = "";
    if (!ok) {
      Serial.println("SCREEN_ERROR Key could not be saved");
      return;
    }
    sendFrame();
    return;
  }
  if (command == 'Y') {
    // Screen link speed in MHz, stored, then restart (the clock survives a software restart).
    const int mhz = Serial.readStringUntil('\n').toInt();
    if (mhz < 5 || mhz > 80) {
      Serial.println("SCREEN_ERROR Invalid speed");
      return;
    }
    settings.putUChar("lcd-mhz", mhz);
    Serial.printf("LCD_SPEED %d MHz, restarting\n", mhz);
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  if (command == 'Z') {
    // PC clock over USB (sent by the bridge when it connects). No screen reply.
    const uint32_t epoch = strtoul(Serial.readStringUntil('\n').c_str(), nullptr, 10);
    if (epoch > 1700000000) {
      clockSet(epoch);
      refreshDynamic();
    }
    return;
  }
  if (command == 'B') {
    // Full-screen redraw benchmark: LVGL render + SPI transfer, averaged over 10 frames.
    uint32_t total = 0;
    for (int i = 0; i < 10; ++i) {
      lv_obj_invalidate(root);
      const uint32_t t = micros();
      lv_refr_now(display);
      total += micros() - t;
    }
    Serial.printf("BENCH full_frame_ms=%.1f fps=%.1f spi=%uMHz transfer_ms=%.1f\n", total / 10000.0f, 1e7f / total,
                  spiMhz, displayLastPresentMicros() / 1000.0f);
    return;
  }
  if (command == 'n') {
    previewToUsb();
    return;
  }
  if (command == 'e') {
    stopPreview();
    Serial.println("PREVIEW_STOPPED");
    return;
  }
  if (command == 'o') {
    if (!savedJpeg || !jpegBytes) {
      Serial.println("CAPTURE FAILED Capture a photo first.");
      return;
    }
    Serial.printf("JPEG_BEGIN %u\n", jpegBytes);
    if (!sendAcknowledged(savedJpeg, jpegBytes)) {
      Serial.println("\nCAPTURE FAILED USB write timed out");
      return;
    }
    Serial.println("\nJPEG_END");
    return;
  }
  String problem;
  if (ccOpen && command != 'f' && command != 'D') {
    ccOpen = false;
    lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
  }
  switch (command) {
    case 'h':
      if (current != Screen::Face) leaveTo(Screen::Face, 0);
      break;
    case 'D':
      if (ccOpen) closeControl();
      else openControl();
      break;
    case 'M':
      if (current != Screen::Apps) openApps();
      break;
    case 'C': show(Screen::Camera); break;
    case 'A': show(Screen::Ask); break;
    case 'p': show(Screen::Photos); break;
    case 'H': show(Screen::History); break;
    case 'i': show(Screen::Settings); break;
    case 'b':
      if (answerCount) showAnswer(answerIds[0]);
      break;
    case 'a':
      show(Screen::Camera);
      if (takePhoto(problem)) show(Screen::Photos);
      else notice(problem, Screen::Camera);
      break;
    case 'q':
      show(Screen::Ask);
      startAsk();
      break;
    case 'Q': captureAndAsk(); break;
    case 'x': cancelAsk(); break;
    case 'X': show(noticeReturn); break;
    case 'P': setModel(!useGemini); break;
    case 'G': setModel(true); break;
    case 'O': setModel(false); break;
    case 'u':
      if (current == Screen::Answer) lv_obj_scroll_by(answerScroll, 0, 150, LV_ANIM_OFF);
      break;
    case 'v':
      if (current == Screen::Answer) lv_obj_scroll_by(answerScroll, 0, -150, LV_ANIM_OFF);
      break;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
      stopPreview();
      tuneCamera(command);
      if (takePhoto(problem)) show(Screen::Photos);
      else notice(problem, Screen::Camera);
      break;
    case 'f': break;
    default: return;
  }
  sendFrame();
}

void deviceTick() {
  if (!framebuffer) return;
  const unsigned long now = millis();
  // Collect finished requests even if the user has moved to another app.
  String result;
  const AiState ai = aiPoll(result);
  if (ai == AiState::Done) {
    static uint16_t th[THUMB * THUMB];  // 8 KB: too big for the loop task stack
    const bool haveThumb = pendingPhotoId && loadPhotoThumb(pendingPhotoId, th);
    const uint32_t when = clockKnown() ? (uint32_t)time(nullptr) : 0;
    const uint32_t id = saveAnswer(result, pendingGemini, pendingPhotoId, when, haveThumb ? th : nullptr);
    if (!id) Serial.println("STORAGE_ERROR Answer not saved");
    loadLatestAnswer();
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    refreshDynamic();
    if (waiting && current == Screen::Ask && id) showAnswer(id);
  } else if (ai == AiState::Failed) {
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    if (waiting && current == Screen::Ask) notice(result, Screen::Ask);
  }
  static unsigned long lastRefresh = 0;
  if (now - lastRefresh > 1000 && !dragging && !fingerDown) {
    lastRefresh = now;
    refreshDynamic();
  }
  // Live viewfinder on the LCD while the PC mirror is not streaming.
  static unsigned long lastFpsReport = 0;
  static int frames = 0;
  if (current == Screen::Camera && !cameraOff && !ccOpen && now - lastSerialMs > 3000 && ensurePreviewMode()) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) {
      const bool ok = decodeToScreen(f->buf, f->len, f->width, f->height, livePixels, JPG_SCALE_NONE);
      esp_camera_fb_return(f);
      if (ok) {
        lv_image_cache_drop(&liveDsc);
        lv_obj_invalidate(viewfinder);
        ++frames;
      }
    }
    if (now - lastFpsReport >= 5000) {
      if (lastFpsReport) Serial.printf("PREVIEW_FPS %.1f\n", frames * 1000.0f / (now - lastFpsReport));
      lastFpsReport = now;
      frames = 0;
    }
  } else {
    lastFpsReport = 0;
    frames = 0;
  }
  lv_timer_handler();
}
