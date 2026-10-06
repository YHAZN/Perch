// Perch OS shell on LVGL 9: face, app grid, Camera, Ask, Answer, Photos, History, Settings.
// Design source of truth: design/index.html (tokens, layout and gestures match it).
#include "device_ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "ai_client.h"
#include "clock.h"
#include "display.h"
#include "storage.h"

namespace {
constexpr int W = 240, H = 284;
// Bottom strip reserved for the system swipe-up. Nothing tappable lives here.
constexpr int HOME_ZONE = 236;
// Distance from the bottom edge to the lowest tappable control (clear of the home zone).
constexpr int ABOVE_HOME = H - HOME_ZONE + 14;
// Drag this far (or flick) to commit the home gesture; less springs back.
constexpr int COMMIT_DRAG = 70;

// Design tokens (design/index.html :root)
lv_color_t VOID_, GRAPHITE, ICON_BG, LINE, MIST, INK, LENS;
const lv_font_t *F_SMALL = &lv_font_montserrat_12;
const lv_font_t *F_BODY = &lv_font_montserrat_16;
const lv_font_t *F_LARGE = &lv_font_montserrat_24;
const lv_font_t *F_CLOCK = &lv_font_montserrat_48;

enum class Screen { Face, Apps, Camera, Ask, Answer, Photos, History, Settings, Notice };
Screen current = Screen::Face;
Screen noticeReturn = Screen::Ask;

Preferences settings;
bool useGemini = true;
String lastAnswer;
bool lastAnswerGemini = true;
bool pendingGemini = true;
unsigned long sequence = 0;
unsigned long lastSerialMs = 0;

// Framebuffer: LVGL renders straight into it (direct mode); the USB mirror reads it too.
uint16_t *framebuffer = nullptr;
lv_display_t *display = nullptr;
lv_indev_t *touch = nullptr;

// Camera state
uint16_t *livePixels = nullptr;   // 240x284 viewfinder
uint16_t *photoPixels = nullptr;  // 240x284 last photo, filled to the screen
uint8_t *savedJpeg = nullptr, *rgbScratch = nullptr;
size_t jpegBytes = 0, jpegCapacity = 0, rgbCapacity = 0;
bool liveRequested = false, previewActive = false;
framesize_t stillSize = FRAMESIZE_QXGA;
int stillQuality = 8;
// Lower number = less JPEG compression. 20 showed visible blocks in the viewfinder.
constexpr int PREVIEW_QUALITY = 12;
// 480x320 gives enough pixels to fill the 240x284 screen by downscaling, not upscaling.
constexpr framesize_t PREVIEW_SIZE = FRAMESIZE_HVGA;
lv_image_dsc_t liveDsc, photoDsc;

// Screens and the widgets we update later
lv_obj_t *scr[9];
lv_obj_t *clockLabel, *dateLabel, *cardKey, *cardValue, *faceOffline;
lv_obj_t *iconName;
lv_obj_t *viewfinder, *thumb, *flash;
lv_obj_t *askPhoto, *askEmpty, *askHint, *askButtonLabel, *askOffline, *busy, *busyRing, *busyLabel, *askSheet;
lv_obj_t *answerScroll, *answerPhoto, *answerLead, *answerBody, *answerMeta;
lv_obj_t *photosImage, *photosEmpty;
lv_obj_t *historyRow, *historyText, *historyMeta, *historyEmpty;
lv_obj_t *modelValue, *wifiValue;
lv_obj_t *noticeText;
lv_obj_t *homeBar;

// Touch injected by the USB mirror ('T' command)
uint8_t spiMhz = 10;
bool injecting = false;
int injectX = 0, injectY = 0;
// Physical touch tracking for the home swipe
bool fingerDown = false, homeGesture = false;
int downX = 0, downY = 0, lastX = 0, lastY = 0;

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
// Every app is a full-screen layer on one LVGL screen, so two can be on screen at once
// (the face shows underneath an app while you drag it away).
lv_obj_t *root = nullptr;
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
  lv_obj_set_size(o, W, height);
  lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_color(o, VOID_, 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_main_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_opa(o, 160, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
void setupImage(lv_image_dsc_t &dsc, uint16_t *pixels) {
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = W;
  dsc.header.h = H;
  dsc.header.stride = W * 2;
  dsc.data_size = W * H * 2;
  dsc.data = reinterpret_cast<const uint8_t *>(pixels);
}

// ---------- camera ----------
bool decodeToScreen(camera_fb_t *f, uint16_t *out, jpg_scale_t scale) {
  const int div = scale == JPG_SCALE_4X ? 4 : scale == JPG_SCALE_2X ? 2 : 1;
  const int dw = f->width / div, dh = f->height / div;
  const size_t needed = dw * dh * 2;
  if (needed > rgbCapacity) {
    free(rgbScratch);
    rgbScratch = (uint8_t *)ps_malloc(needed);
    rgbCapacity = rgbScratch ? needed : 0;
  }
  if (!rgbScratch || !out || !jpg2rgb565(f->buf, f->len, rgbScratch, scale)) return false;
  // Fill the portrait screen: scale to the screen height, centre-crop the width.
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
const char *screenName() {
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
    default: return "error";
  }
}
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
  for (int i = 0; i < 9; ++i) {
    if (i == (int)current) continue;
    lv_anim_delete(scr[i], setY);
    lv_obj_set_y(scr[i], 0);
    lv_obj_add_flag(scr[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_y(layer(current), 0);
}
void enter(Screen next) {
  if (next != Screen::Camera) stopPreview();
  liveRequested = next == Screen::Camera;
  current = next;
  refreshDynamic();
  const bool chrome = next != Screen::Face && next != Screen::Apps;
  if (chrome) lv_obj_remove_flag(homeBar, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(homeBar, LV_OBJ_FLAG_HIDDEN);
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
  enter(next);
  animY(to, 28, 0, 200, lv_anim_path_ease_out, settleLayers);
}
void notice(const String &message, Screen back) {
  lv_label_set_text(noticeText, message.c_str());
  noticeReturn = back;
  show(Screen::Notice);
}
// Going home: the app lifts away and the face is underneath. Used by flicks and the mirror.
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

// ---------- content ----------
void setAnswer(const String &answer, bool gemini) {
  // Answer first: the first sentence (or line) is set large, the rest as body text.
  int cut = answer.indexOf('\n');
  const int period = answer.indexOf(". ");
  if (period >= 0 && period < 140 && (cut < 0 || period < cut)) cut = period + 1;
  if (cut > 160) cut = -1;
  String lead = cut > 0 ? answer.substring(0, cut) : "";
  String rest = cut > 0 ? answer.substring(cut) : answer;
  rest.trim();
  lv_label_set_text(answerLead, lead.c_str());
  if (lead.isEmpty()) lv_obj_add_flag(answerLead, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(answerLead, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(answerBody, rest.c_str());
  lv_label_set_text(answerMeta, gemini ? "Answered by Gemini" : "Answered by GPT");
  if (jpegBytes) lv_obj_remove_flag(answerPhoto, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(answerPhoto, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(answerScroll, 0, LV_ANIM_OFF);
}
void refreshDynamic() {
  const bool online = networkConnected();
  // Face clock appears only once the internet clock is set; there is no clock battery.
  time_t now = time(nullptr);
  if (now > 1700000000) {
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
  if (online) lv_obj_add_flag(faceOffline, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(faceOffline, LV_OBJ_FLAG_HIDDEN);
  if (online) lv_obj_add_flag(askOffline, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(askOffline, LV_OBJ_FLAG_HIDDEN);
  // Smart Stack: the most useful single card right now.
  if (!lastAnswer.isEmpty()) {
    lv_label_set_text(cardKey, "Last answer");
    lv_label_set_text(cardValue, lastAnswer.c_str());
  } else if (!online) {
    lv_label_set_text(cardKey, "Wi-Fi");
    lv_label_set_text(cardValue, "Not connected. Camera and Photos work offline.");
  } else {
    lv_label_set_text(cardKey, "Ask");
    lv_label_set_text(cardValue, "Point at a question, then open Ask.");
  }
  lv_label_set_text(modelValue, useGemini ? "Gemini" : "GPT");
  lv_label_set_text(wifiValue, online ? WiFi.SSID().c_str() : "Not connected");
  const bool photo = jpegBytes > 0;
  if (photo) {
    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(askPhoto, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(askEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(photosImage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(photosEmpty, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(askPhoto, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(askEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(photosImage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(photosEmpty, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(askButtonLabel, photo ? "Ask about this" : "Capture and ask");
  if (photo) lv_obj_remove_flag(askHint, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(askHint, LV_OBJ_FLAG_HIDDEN);
  if (lastAnswer.isEmpty()) {
    lv_obj_add_flag(historyRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(historyEmpty, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(historyRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(historyEmpty, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(historyText, lastAnswer.c_str());
    lv_label_set_text(historyMeta, lastAnswerGemini ? "Gemini" : "GPT");
  }
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
// Returns true when a new photo replaced the old one. Failures keep the previous photo.
bool takePhoto(String &problem) {
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
  const bool ok = jpegTarget && decoded && decodeToScreen(frame, decoded, JPG_SCALE_4X);
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
  lv_image_cache_drop(&photoDsc);
  lv_obj_invalidate(lv_screen_active());
  refreshDynamic();
  return true;
}
void startAsk() {
  if (!jpegBytes) {
    notice("Take a photo first.", Screen::Ask);
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
}
void toggleModel() {
  useGemini = !useGemini;
  settings.putBool("gemini", useGemini);
  refreshDynamic();
}

// ---------- screens ----------
void onClick(lv_obj_t *o, void (*fn)()) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      o, [](lv_event_t *e) { ((void (*)())lv_event_get_user_data(e))(); }, LV_EVENT_CLICKED, (void *)fn);
}
lv_obj_t *pressedFeedback(lv_obj_t *o) {
  lv_obj_set_style_opa(o, LV_OPA_60, LV_STATE_PRESSED);
  return o;
}

void buildFace() {
  lv_obj_t *s = scr[(int)Screen::Face] = screenBase();
  clockLabel = text(s, "", F_CLOCK, INK);
  lv_obj_set_pos(clockLabel, 20, 18);
  dateLabel = text(s, "", F_BODY, MIST);
  lv_obj_set_pos(dateLabel, 24, 76);
  faceOffline = text(s, "Offline", F_SMALL, MIST);
  lv_obj_align(faceOffline, LV_ALIGN_TOP_RIGHT, -26, 28);
  lv_obj_t *card = pressedFeedback(plain(s));
  lv_obj_set_size(card, 216, 108);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_radius(card, 26, 0);
  lv_obj_set_style_bg_color(card, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(card, 16, 0);
  lv_obj_set_style_pad_ver(card, 14, 0);
  lv_obj_t *ring = circle(card, 10, LENS, 2, VOID_, LV_OPA_TRANSP);
  lv_obj_set_pos(ring, 0, 3);
  cardKey = text(card, "", F_SMALL, MIST);
  lv_obj_set_pos(cardKey, 16, 0);
  cardValue = text(card, "", F_BODY, INK);
  lv_obj_set_width(cardValue, 184);
  lv_obj_set_height(cardValue, 60);
  lv_label_set_long_mode(cardValue, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(cardValue, 0, 22);
  onClick(card, [] {
    if (!lastAnswer.isEmpty()) {
      setAnswer(lastAnswer, lastAnswerGemini);
      show(Screen::Answer);
    } else if (!networkConnected()) show(Screen::Settings);
    else show(Screen::Ask);
  });
  // Swipe up on the face opens the app grid.
  lv_obj_add_event_cb(
      s,
      [](lv_event_t *) {
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP && current == Screen::Face) openApps();
      },
      LV_EVENT_GESTURE, nullptr);
}

void appIcon(lv_obj_t *parent, int x, int y, const char *name, bool accent, void (*open)(), void (*glyph)(lv_obj_t *)) {
  lv_obj_t *b = circle(parent, 62, ICON_BG, 0, accent ? LENS : ICON_BG, LV_OPA_COVER);
  lv_obj_set_pos(b, x - 31, y - 31);
  lv_obj_set_style_transform_scale(b, 230, LV_STATE_PRESSED);
  lv_obj_set_style_transform_pivot_x(b, 31, 0);
  lv_obj_set_style_transform_pivot_y(b, 31, 0);
  glyph(b);
  onClick(b, open);
  // The name shows at the top only while a finger is on the icon (no labels under icons).
  lv_obj_add_event_cb(
      b,
      [](lv_event_t *e) {
        lv_label_set_text(iconName, (const char *)lv_event_get_user_data(e));
        lv_obj_remove_flag(iconName, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_PRESSED, (void *)name);
  lv_obj_add_event_cb(
      b, [](lv_event_t *) { lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(
      b, [](lv_event_t *) { lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_PRESS_LOST, nullptr);
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
void glyphSymbol(lv_obj_t *b, const char *symbol) {
  lv_obj_t *l = text(b, symbol, F_LARGE, INK);
  lv_obj_center(l);
}

void buildApps() {
  lv_obj_t *s = scr[(int)Screen::Apps] = screenBase();
  lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
  // Honeycomb rows (design/index.html): 3, then 2 offset.
  appIcon(s, 50, 80, "Camera", false, [] { show(Screen::Camera); }, glyphCamera);
  appIcon(s, 120, 80, "Ask", true, [] { show(Screen::Ask); }, glyphAsk);
  appIcon(
      s, 190, 80, "Photos", false, [] { show(Screen::Photos); }, [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_IMAGE); });
  appIcon(
      s, 85, 146, "History", false, [] { show(Screen::History); }, [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_LIST); });
  appIcon(
      s, 155, 146, "Settings", false, [] { show(Screen::Settings); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_SETTINGS); });
  iconName = text(s, "", F_SMALL, INK);
  lv_obj_align(iconName, LV_ALIGN_TOP_MID, 0, 18);
  lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN);
}

void buildCamera() {
  lv_obj_t *s = scr[(int)Screen::Camera] = screenBase();
  viewfinder = lv_image_create(s);
  lv_image_set_src(viewfinder, &liveDsc);
  lv_obj_set_pos(viewfinder, 0, 0);
  scrimBottom(s, 110);
  lv_obj_t *shutter = circle(s, 60, INK, 4, INK, LV_OPA_TRANSP);
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
  lv_obj_align(askOffline, LV_ALIGN_TOP_MID, 0, 12);
  askHint = text(s, "Hold for a new photo", F_SMALL, MIST);
  lv_obj_align(askHint, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME - 56);
  lv_obj_t *pill = pressedFeedback(plain(s));
  lv_obj_set_size(pill, 196, 46);
  lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_radius(pill, 23, 0);
  lv_obj_set_style_bg_color(pill, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(pill, 230, 0);
  lv_obj_t *ring = circle(pill, 28, LENS, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_align(ring, LV_ALIGN_LEFT_MID, 9, 0);
  askButtonLabel = text(pill, "", F_BODY, INK);
  lv_obj_align(askButtonLabel, LV_ALIGN_LEFT_MID, 46, 0);
  onClick(pill, [] {
    if (jpegBytes) startAsk();
    else captureAndAsk();
  });
  // Long-press anywhere: new photo, then ask. Replaces the tiny corner icon of the old UI.
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      s,
      [](lv_event_t *) {
        if (lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(askSheet, LV_OBJ_FLAG_HIDDEN);
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
  lv_obj_t *l1 = text(newPhoto, "Take new photo and ask", F_BODY, INK);
  lv_obj_center(l1);
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
  lv_obj_t *l2 = text(keep, "Keep this photo", F_BODY, INK);
  lv_obj_center(l2);
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
  lv_obj_t *cl = text(cancel, "Cancel", F_BODY, MIST);
  lv_obj_center(cl);
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
  lv_obj_set_style_pad_bottom(answerScroll, 48, 0);
  lv_obj_set_style_pad_row(answerScroll, 10, 0);
  answerPhoto = lv_image_create(answerScroll);
  lv_image_set_src(answerPhoto, &photoDsc);
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
  lv_image_set_src(photosImage, &photoDsc);
  photosEmpty = plain(s);
  lv_obj_set_size(photosEmpty, W, H);
  lv_obj_t *msg = text(photosEmpty, "No photos yet.", F_BODY, MIST);
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *open = pressedFeedback(plain(photosEmpty));
  lv_obj_set_size(open, 150, 44);
  lv_obj_align(open, LV_ALIGN_CENTER, 0, 20);
  lv_obj_set_style_radius(open, 22, 0);
  lv_obj_set_style_bg_color(open, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(open, LV_OPA_COVER, 0);
  lv_obj_t *ol = text(open, "Open Camera", F_BODY, INK);
  lv_obj_center(ol);
  onClick(open, [] { show(Screen::Camera); });
}

lv_obj_t *title(lv_obj_t *s, const char *value) {
  lv_obj_t *t = text(s, value, F_LARGE, INK);
  lv_obj_set_pos(t, 22, 18);
  return t;
}
lv_obj_t *row(lv_obj_t *s, int y, const char *label, lv_obj_t **value) {
  lv_obj_t *r = pressedFeedback(plain(s));
  lv_obj_set_size(r, W, 52);
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
  title(s, "History");
  historyRow = pressedFeedback(plain(s));
  lv_obj_set_size(historyRow, W, 64);
  lv_obj_set_pos(historyRow, 0, 58);
  historyText = text(historyRow, "", F_BODY, INK);
  lv_obj_set_width(historyText, W - 44);
  lv_label_set_long_mode(historyText, LV_LABEL_LONG_DOT);
  lv_obj_set_height(historyText, 20);
  lv_obj_set_pos(historyText, 22, 10);
  historyMeta = text(historyRow, "", F_SMALL, MIST);
  lv_obj_set_pos(historyMeta, 22, 36);
  onClick(historyRow, [] {
    setAnswer(lastAnswer, lastAnswerGemini);
    show(Screen::Answer);
  });
  historyEmpty = text(s, "No answers yet.", F_BODY, MIST);
  lv_obj_align(historyEmpty, LV_ALIGN_CENTER, 0, 0);
}

void buildSettings() {
  lv_obj_t *s = scr[(int)Screen::Settings] = screenBase();
  title(s, "Settings");
  lv_obj_t *model = row(s, 58, "Model", &modelValue);
  onClick(model, toggleModel);
  row(s, 110, "Wi-Fi", &wifiValue);
}

void buildNotice() {
  lv_obj_t *s = scr[(int)Screen::Notice] = screenBase();
  noticeText = text(s, "", F_BODY, INK);
  lv_obj_set_width(noticeText, W - 48);
  lv_label_set_long_mode(noticeText, LV_LABEL_LONG_WRAP);
  lv_obj_align(noticeText, LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *ok = pressedFeedback(plain(s));
  lv_obj_set_size(ok, 120, 44);
  lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_radius(ok, 22, 0);
  lv_obj_set_style_bg_color(ok, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
  lv_obj_t *l = text(ok, "OK", F_BODY, INK);
  lv_obj_center(l);
  onClick(ok, [] { show(noticeReturn); });
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
  static int speed = 0;  // upward px/s, smoothed, for flick detection
  if (pressed) {
    const unsigned long now = millis();
    if (!fingerDown) {
      fingerDown = true;
      downX = x;
      downY = y;
      speed = 0;
      // The bottom strip belongs to the system: drag up from it to go home; taps do nothing.
      homeGesture = downY >= HOME_ZONE && !injecting;
    } else if (now > moveAt) {
      speed = (speed + (lastY - y) * 1000 / (int)(now - moveAt)) / 2;
    }
    moveAt = now;
    lastX = x;
    lastY = y;
    if (homeGesture && downY - y > 6) dragHome(downY - y);
  } else if (fingerDown) {
    fingerDown = false;
    if (homeGesture) releaseHome(max(0, downY - lastY), speed);
    homeGesture = false;
  }
  if (homeGesture) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  data->point.x = pressed ? x : lastX;
  data->point.y = pressed ? y : lastY;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
uint32_t tick() { return millis(); }
void settle() {
  // Let LVGL finish transitions so the USB mirror sees the resulting screen, not a fade.
  const unsigned long start = millis();
  do {
    lv_timer_handler();
    delay(5);
  } while (lv_anim_count_running() > 1 && millis() - start < 600);
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
                settings.isKey("gpt-key"), liveRequested && current == Screen::Camera);
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
  if (current != Screen::Camera) {
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
}  // namespace

void deviceNoteSerial() { lastSerialMs = millis(); }

void initDeviceUi() {
  settings.begin("tiny-ai", false);
  useGemini = settings.getBool("gemini", true);
  if (!storageBegin()) Serial.println("STORAGE_ERROR Flash filesystem unavailable");
  if (!loadLastAnswer(lastAnswer, lastAnswerGemini)) {
    // Copy an answer saved by older firmware in NVS. The NVS copy is left untouched.
    lastAnswer = settings.getString("last-answer", "");
    lastAnswerGemini = settings.getBool("answer-gemini", true);
    if (!lastAnswer.isEmpty()) saveLastAnswer(lastAnswer, lastAnswerGemini);
  }
  framebuffer = (uint16_t *)ps_malloc(W * H * 2);
  livePixels = (uint16_t *)ps_calloc(W * H, 2);
  photoPixels = (uint16_t *)ps_calloc(W * H, 2);
  // Screen link speed is stored so it can be tuned for the wiring without reflashing ('Y').
  spiMhz = constrain(settings.getUChar("lcd-mhz", 10), 5, 80);
  const bool lcd = displayBegin(spiMhz * 1000000UL);
  Serial.printf("DISPLAY lcd=%d touch=%d spi=%uMHz\n", lcd, touchAvailable(), spiMhz);
  if (!framebuffer || !livePixels || !photoPixels) {
    Serial.println("DISPLAY_ERROR Out of PSRAM");
    framebuffer = nullptr;
    return;
  }

  VOID_ = lv_color_hex(0x000000);
  GRAPHITE = lv_color_hex(0x1a1b1e);
  ICON_BG = lv_color_hex(0x2a2c31);
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

  setupImage(liveDsc, livePixels);
  setupImage(photoDsc, photoPixels);
  buildFace();
  buildApps();
  buildCamera();
  buildAsk();
  buildAnswer();
  buildPhotos();
  buildHistory();
  buildSettings();
  buildNotice();
  homeBar = plain(lv_layer_top());
  lv_obj_set_size(homeBar, 40, 4);
  lv_obj_set_style_radius(homeBar, 2, 0);
  lv_obj_set_style_bg_color(homeBar, INK, 0);
  lv_obj_set_style_bg_opa(homeBar, 115, 0);
  lv_obj_align(homeBar, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_add_flag(homeBar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(homeBar, LV_OBJ_FLAG_CLICKABLE);

  refreshDynamic();
  lv_obj_remove_flag(layer(Screen::Face), LV_OBJ_FLAG_HIDDEN);
  enter(Screen::Face);
  const uint32_t start = micros();
  lv_refr_now(display);
  Serial.printf("DISPLAY first frame %lu us\n", (unsigned long)(micros() - start));
  if (lcd) displayBrightness(255);
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
    if (x >= 0 && x < W && y >= 0 && y < HOME_ZONE) injectTap(x, y);
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
  switch (command) {
    case 'h': show(Screen::Face); break;
    case 'C': show(Screen::Camera); break;
    case 'A': show(Screen::Ask); break;
    case 'p': show(Screen::Photos); break;
    case 'H': show(Screen::History); break;
    case 'i': show(Screen::Settings); break;
    case 'b':
      if (!lastAnswer.isEmpty()) {
        setAnswer(lastAnswer, lastAnswerGemini);
        show(Screen::Answer);
      }
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
    case 'P': toggleModel(); break;
    case 'G':
      useGemini = true;
      settings.putBool("gemini", true);
      refreshDynamic();
      break;
    case 'O':
      useGemini = false;
      settings.putBool("gemini", false);
      refreshDynamic();
      break;
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
    lastAnswer = result;
    lastAnswerGemini = pendingGemini;
    if (!saveLastAnswer(lastAnswer, lastAnswerGemini)) Serial.println("STORAGE_ERROR Answer not saved");
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    refreshDynamic();
    if (waiting && current == Screen::Ask) {
      setAnswer(lastAnswer, lastAnswerGemini);
      show(Screen::Answer);
    }
  } else if (ai == AiState::Failed) {
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    if (waiting && current == Screen::Ask) notice(result, Screen::Ask);
  }
  static unsigned long lastRefresh = 0;
  if (now - lastRefresh > 1000) {
    lastRefresh = now;
    refreshDynamic();
  }
  // Live viewfinder on the LCD while the PC mirror is not streaming.
  static unsigned long lastFpsReport = 0;
  static int frames = 0;
  if (current == Screen::Camera && now - lastSerialMs > 3000 && ensurePreviewMode()) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) {
      const bool ok = decodeToScreen(f, livePixels, JPG_SCALE_NONE);
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
