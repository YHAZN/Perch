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
#include "camera.h"
#include "clock.h"
#include "display.h"
#include "remote.h"
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
// When Ask has earlier answers, its first screen ends this short so the list peeks in.
constexpr int ASK_PEEK = 26;
constexpr int MAX_PHOTOS = 12;
// Left strip: drag right from here to go back one level.
constexpr int BACK_ZONE = 24;
const uint8_t BRIGHTNESS[] = {255, 140, 50};

// Design tokens (design/index.html :root)
lv_color_t VOID_, GRAPHITE, ICON_BG, ICON_DIM, LINE, MIST, INK, LENS;
const lv_font_t *F_SMALL = &lv_font_montserrat_12;
const lv_font_t *F_BODY = &lv_font_montserrat_16;
const lv_font_t *F_LARGE = &lv_font_montserrat_24;
const lv_font_t *F_CLOCK = &lv_font_montserrat_48;

enum class Screen {
  Face,
  Apps,
  Camera,
  Ask,
  Answer,
  Photos,
  History,
  Settings,
  Model,
  Notice,
  Remote,
  Wifi,
  Password,
  Count
};
Screen current = Screen::Face;
Screen noticeReturn = Screen::Ask;

Preferences settings;
bool useGemini = true;
bool cameraOff = false;
uint8_t brightnessLevel = 0;
uint8_t spiMhz = 10;
unsigned long sequence = 0;
unsigned long lastSerialMs = 0;

// Rendering: LVGL draws chunks into two small internal buffers while the previous chunk is
// still being sent by DMA. `framebuffer` keeps a full copy for the USB mirror.
uint16_t *framebuffer = nullptr;
uint16_t *drawBuffers[2] = {nullptr, nullptr};
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
// Bottom-of-viewfinder shading, baked into each preview frame (cheaper than blending a layer).
uint8_t scrimRow[284];
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
lv_obj_t *historyList, *historyEmpty, *askScroll, *askHero;
lv_obj_t *remoteStatus, *remoteSlides, *remoteMediaPanel, *remoteModeLabel[2];
bool remoteMediaMode = false;
// Offline queue state (see storage.h). queueInFlight is the item being asked right now.
QueuedAsk queued[16];
int queuedCount = 0;
uint32_t queueInFlight = 0;
bool queueWhenSaved = false;
unsigned long queueRetryAt = 0;
void refreshQueue();
void rebuildHistory();
lv_obj_t *wifiList, *wifiStatus, *passwordTitle, *passwordField, *keyboard;
String joiningSsid;
bool scanShown = false;
unsigned long scanStartedAt = 0;
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
enum class Edge { None, Home, Control, Back } edge = Edge::None;
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
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
// Set a label only when the text differs: every set redraws the label's area.
void setText(lv_obj_t *l, const char *t) {
  if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}
// Toast: one short line at the top that fades by itself. For confirmations and warnings that
// do not need a decision ("Saved", "Blurry, hold still"). Never for errors that need action.
lv_obj_t *toastBox = nullptr, *toastLabel = nullptr;
void toast(const char *message) {
  if (!toastBox) return;
  lv_label_set_text(toastLabel, message);
  lv_obj_remove_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toastBox);
  lv_anim_delete(toastBox, nullptr);
  lv_obj_set_style_opa(toastBox, LV_OPA_COVER, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, toastBox);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_delay(&a, 1800);
  lv_anim_set_duration(&a, 300);
  lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); });
  lv_anim_set_completed_cb(&a, [](lv_anim_t *x) { lv_obj_add_flag((lv_obj_t *)x->var, LV_OBJ_FLAG_HIDDEN); });
  lv_anim_start(&a);
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
bool decodeToScreen(const uint8_t *jpeg, size_t len, int width, int height, uint16_t *out, jpg_scale_t scale,
                    uint8_t *&scratch, size_t &capacity) {
  const int div = scale == JPG_SCALE_8X ? 8 : scale == JPG_SCALE_4X ? 4 : scale == JPG_SCALE_2X ? 2 : 1;
  const int dw = width / div, dh = height / div;
  const size_t needed = dw * dh * 2;
  if (needed > capacity) {
    free(scratch);
    scratch = (uint8_t *)ps_malloc(needed);
    capacity = scratch ? needed : 0;
  }
  uint8_t *rgbScratch = scratch;
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
// Smallest decode that still covers the screen height (decoding at 1/4 or 1/8 is much faster).
jpg_scale_t scaleFor(int h) {
  return h / 8 >= H ? JPG_SCALE_8X : h / 4 >= H ? JPG_SCALE_4X : h / 2 >= H ? JPG_SCALE_2X : JPG_SCALE_NONE;
}
bool decodeStoredWith(const uint8_t *jpeg, size_t len, uint16_t *out, uint8_t *&scratch, size_t &capacity) {
  int w, h;
  return jpegSize(jpeg, len, w, h) && decodeToScreen(jpeg, len, w, h, out, scaleFor(h), scratch, capacity);
}
bool decodeStored(const uint8_t *jpeg, size_t len, uint16_t *out) {
  return decodeStoredWith(jpeg, len, out, rgbScratch, rgbCapacity);
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
  // The small screen-sized copy decodes in ~0.1 s; the full photo is only a fallback.
  if (!loadPhotoScreen(id, jpeg, len) && !loadPhoto(id, jpeg, len)) return false;
  const bool ok = decodeStored(jpeg, len, out);
  free(jpeg);
  return ok;
}

// ---------- camera ----------
// Viewfinder pipeline: a task on core 0 fetches and converts camera frames while the UI on
// core 1 draws the previous one. Three buffers: front (on screen), ready (newest complete),
// back (being written). camLock serialises every use of the camera driver.
SemaphoreHandle_t camLock = nullptr;
portMUX_TYPE liveMux = portMUX_INITIALIZER_UNLOCKED;
uint16_t *liveBuf[3] = {nullptr, nullptr, nullptr};
volatile int liveFront = 0, liveReady = -1;
volatile bool previewWanted = false;
volatile uint32_t framesConverted = 0, convertMicros = 0;
void convertPreview(const camera_fb_t *f, uint16_t *out, bool shade = true);
void captureInTask();
volatile bool captureRequested = false;
void previewTask(void *) {
  for (;;) {
    if (captureRequested) {
      xSemaphoreTake(camLock, portMAX_DELAY);
      captureInTask();
      xSemaphoreGive(camLock);
      captureRequested = false;
      continue;
    }
    if (!previewWanted) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    int back = 0;
    portENTER_CRITICAL(&liveMux);
    while (back == liveFront || back == liveReady) ++back;
    portEXIT_CRITICAL(&liveMux);
    bool got = false;
    xSemaphoreTake(camLock, portMAX_DELAY);
    if (previewWanted && cameraMode() == CameraMode::Preview) {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) {
        const uint32_t t = micros();
        convertPreview(f, liveBuf[back]);
        convertMicros += micros() - t;
        esp_camera_fb_return(f);
        got = true;
      }
    }
    xSemaphoreGive(camLock);
    if (!got) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    portENTER_CRITICAL(&liveMux);
    liveReady = back;
    portEXIT_CRITICAL(&liveMux);
    ++framesConverted;
  }
}
bool cameraSwitch(CameraMode mode) {
  xSemaphoreTake(camLock, portMAX_DELAY);
  const bool ok = cameraSetMode(mode);
  xSemaphoreGive(camLock);
  return ok;
}
void stopPreview() {
  previewWanted = false;
  if (cameraMode() == CameraMode::Preview) cameraSwitch(CameraMode::Off);
}
bool ensurePreviewMode() {
  if (!cameraSwitch(CameraMode::Preview)) return false;
  previewWanted = true;
  return true;
}
// Show the newest converted frame, if there is one. Returns true when the image changed.
bool takeLiveFrame() {
  bool changed = false;
  portENTER_CRITICAL(&liveMux);
  if (liveReady >= 0) {
    liveFront = liveReady;
    liveReady = -1;
    changed = true;
  }
  portEXIT_CRITICAL(&liveMux);
  if (changed) liveDsc.data = reinterpret_cast<const uint8_t *>(liveBuf[liveFront]);
  return changed;
}
// Camera frame -> 240x284 screen: scale to height, centre-crop the width, and darken the
// bottom so the shutter reads on any scene. Accepts JPEG (decoded here) or raw RGB565.
uint8_t *previewScratch = nullptr;  // owned by the viewfinder task
void convertPreview(const camera_fb_t *f, uint16_t *out, bool shade) {
  const int sw = f->width, sh = f->height;
  const uint8_t *src = f->buf;
  bool bigEndian = true;
  if (f->format == PIXFORMAT_JPEG) {
    if (!previewScratch) previewScratch = (uint8_t *)ps_malloc(sw * sh * 2);
    if (!previewScratch || !jpg2rgb565(f->buf, f->len, previewScratch, JPG_SCALE_NONE)) return;
    src = previewScratch;
    bigEndian = false;
  }
  const int cropW = min(sw, sh * W / H), left = (sw - cropW) / 2;
  for (int y = 0; y < H; ++y) {
    const uint8_t *row = src + (y * sh / H) * sw * 2;
    const uint32_t m = shade ? scrimRow[y] : 255;
    uint16_t *dst = out + y * W;
    for (int x = 0; x < W; ++x) {
      const uint8_t *px = row + (left + x * cropW / W) * 2;
      uint16_t v = bigEndian ? (px[0] << 8) | px[1] : px[0] | (px[1] << 8);
      if (m < 255) {
        const uint32_t r = ((v >> 11) * m) >> 8, g = (((v >> 5) & 63) * m) >> 8, b = ((v & 31) * m) >> 8;
        v = (r << 11) | (g << 5) | b;
      }
      dst[x] = v;
    }
  }
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
void startScan();
void renderScan();
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
// Where "back" goes: each screen opened from somewhere remembers it. Home clears it.
std::vector<Screen> backStack;
lv_obj_t *backButton = nullptr;
void enter(Screen next) {
  // Automatic Wi-Fi retries would cancel scans; pause them while choosing a network.
  networkPauseRetries(next == Screen::Wifi || next == Screen::Password);
  if (next != Screen::Camera) stopPreview();
  current = next;
  onEnter(next);
  refreshDynamic();
  hide(homeBar, next == Screen::Face || next == Screen::Apps);
  // The back arrow appears on screens you reached from inside another app (two levels deep).
  const bool showBack = !backStack.empty() && backStack.back() != Screen::Face && backStack.back() != Screen::Apps &&
                        next != Screen::Notice;
  if (backButton) hide(backButton, !showBack);
  if (next == Screen::Face) backStack.clear();
}
// Opening something: it rises a short distance into place over what was there.
void show(Screen next) {
  if (next == current) {
    enter(next);
    return;
  }
  if (next == Screen::Face) backStack.clear();
  else if (current == Screen::Face || current == Screen::Apps) {
    backStack.clear();
    backStack.push_back(current);
  } else backStack.push_back(current);
  lv_obj_t *to = layer(next);
  lv_obj_remove_flag(to, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(to);
  if (ccOpen) lv_obj_move_foreground(cc);
  enter(next);
  animY(to, 28, 0, 260, lv_anim_path_ease_out, settleLayers);
}
void notice(const String &message, Screen back) {
  lv_label_set_text(noticeText, message.c_str());
  noticeReturn = back;
  show(Screen::Notice);
}
// Going home: the app lifts away and the face is underneath.
void leaveTo(Screen under, int fromY) {
  if (under == Screen::Face) backStack.clear();
  else if (!backStack.empty() && backStack.back() == under) backStack.pop_back();
  lv_obj_t *top = layer(current), *below = layer(under);
  lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(below, 0);
  lv_obj_move_foreground(top);
  enter(under);
  animY(top, fromY, -H, 240, lv_anim_path_ease_in, settleLayers);
}
void openApps(int fromY = H) {
  lv_obj_t *grid = layer(Screen::Apps);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(grid);
  enter(Screen::Apps);
  animY(grid, fromY, 0, 280, lv_anim_path_ease_out, settleLayers);
}
void setX(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, v); }
void animX(lv_obj_t *o, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done) {
  lv_anim_delete(o, setX);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_path_cb(&a, path);
  lv_anim_set_exec_cb(&a, setX);
  if (done) lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}
void resetX(lv_anim_t *a) {
  for (int i = 0; i < (int)Screen::Count; ++i) lv_obj_set_x(scr[i], 0);
  settleLayers(a);
}
Screen backTarget() { return backStack.empty() ? Screen::Face : backStack.back(); }
// Going back: the screen slides off to the right, revealing where you came from.
void goBack(int fromX = 0) {
  if (current == Screen::Face) return;
  const Screen target = backTarget();
  if (!backStack.empty()) backStack.pop_back();
  lv_obj_t *top = layer(current), *below = layer(target);
  lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(below, 0, 0);
  lv_obj_move_foreground(top);
  enter(target);
  animX(top, fromX, W, 240, lv_anim_path_ease_in, resetX);
}
bool backDragging = false;
void dragBack(int dx) {
  if (!backDragging) {
    backDragging = true;
    lv_obj_t *below = layer(backTarget());
    lv_obj_remove_flag(below, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(below, 0, 0);
    lv_obj_move_foreground(layer(current));
  }
  lv_obj_set_x(layer(current), dx);
}
void releaseBack(int dx, int speedX) {
  if (!backDragging) return;
  backDragging = false;
  if (dx > COMMIT_DRAG || (dx > 24 && speedX > 600)) goBack(dx);
  else animX(layer(current), dx, 0, 280, lv_anim_path_ease_out, resetX);
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
    else animY(layer(Screen::Apps), H - dy, H, 260, lv_anim_path_ease_out, settleLayers);
  } else {
    if (commit) leaveTo(Screen::Face, -dy);
    else animY(layer(current), -dy, 0, 280, lv_anim_path_ease_out, settleLayers);
  }
}

// Control Center: pulled down from the top edge over whatever is on screen.
void ccClosed(lv_anim_t *) { lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN); }
void openControl(int fromY = -H) {
  ccOpen = true;
  refreshDynamic();
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(cc);
  animY(cc, fromY, 0, 280, lv_anim_path_ease_out, nullptr);
}
void closeControl(int fromY = 0) {
  if (!ccOpen) return;
  ccOpen = false;
  animY(cc, fromY, -H, 240, lv_anim_path_ease_in, ccClosed);
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
    notice("That answer could not be read.", Screen::Ask);
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
enum class Card { Answer, Photo, Offline, Queue };
std::vector<Card> cards;
int cardIndex = 0;
String cardSignature;
void renderCard() {
  const bool online = networkConnected();
  // Rebuild only when something the card shows has changed.
  const String signature = String(online) + networkEnabled() + "|" + lastInfo.id + "|" + latestPhotoId + "|" +
                           jpegBytes + "|" + cardIndex + "|" + ago(lastInfo.when) + "|" + queuedCount;
  if (signature == cardSignature) return;
  cardSignature = signature;
  cards.clear();
  if (queuedCount) cards.push_back(Card::Queue);
  if (!online) cards.push_back(Card::Offline);
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
  if (c == Card::Queue) {
    setText(cardKey, online ? "Asking" : "Waiting for Wi-Fi");
    setText(cardValue, (String(queuedCount) + (queuedCount == 1 ? " question" : " questions") +
                        (online ? " being answered." : " will be asked when Wi-Fi is back."))
                           .c_str());
  } else if (c == Card::Offline) {
    setText(cardKey, "Wi-Fi");
    setText(cardValue, networkEnabled() ? "Not connected. Camera and Photos work offline."
                                        : "Wi-Fi is off. Turn it on in Control Center.");
  } else if (c == Card::Photo) {
    setText(cardKey, "Last photo");
  } else if (!lastAnswer.isEmpty()) {
    const String when = ago(lastInfo.when);
    setText(cardKey, when.length() ? ("Last answer, " + when).c_str() : "Last answer");
    setText(cardValue, lastAnswer.c_str());
  } else {
    setText(cardKey, "Ask");
    setText(cardValue, "Point at a question, then open Ask.");
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

// Cached so the UI never waits on flash: key presence changes only via 'K'; free space is
// measured on the save task (the query walks the whole filesystem, ~175 ms).
int keyCache = -1;  // bit 0 = Gemini key, bit 1 = GPT key; -1 = unknown
volatile uint32_t freeKbCache = 0;
String ccSignature;
void refreshDynamic() {
  const bool online = networkConnected();
  const time_t now = time(nullptr);
  if (clockKnown()) {
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M", &t);
    setText(clockLabel, buf[0] == '0' ? buf + 1 : buf);
    strftime(buf, sizeof(buf), "%a %b %e", &t);
    setText(dateLabel, buf);
  } else {
    setText(clockLabel, "");
    setText(dateLabel, "");
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
  setText(askButtonLabel, !online ? "Ask when online" : photo ? "Ask about this" : "Capture and ask");
  const lv_opa_t pillOpa = online ? LV_OPA_COVER : LV_OPA_60;
  if (lv_obj_get_style_opa(askPill, 0) != pillOpa) lv_obj_set_style_opa(askPill, pillOpa, 0);
  // Camera privacy
  hide(viewfinder, cameraOff);
  hide(shutter, cameraOff);
  hide(cameraOffLabel, !cameraOff);
  // Settings and Model
  setText(modelValue, useGemini ? "Gemini" : "GPT");
  setText(wifiValue, !networkEnabled() ? "Off" : online ? networkName().c_str() : "Not connected");
  // Wi-Fi list: show results when a scan finishes; rescan every 20 s while it is open.
  if (current == Screen::Wifi) {
    renderScan();
    if (scanShown && millis() - scanStartedAt > 20000) startScan();
  }
  if (joiningSsid.length() && networkName() == joiningSsid) {
    toast(("Connected to " + joiningSsid).c_str());
    joiningSsid = "";
    if (current == Screen::Wifi) startScan();
  }
  if (keyCache < 0) keyCache = (settings.isKey("gemini-key") ? 1 : 0) | (settings.isKey("gpt-key") ? 2 : 0);
  for (int i = 0; i < 2; ++i) {
    setText(modelCheck[i], (i == 0) == useGemini ? LV_SYMBOL_OK : "");
    setText(modelSub[i], (keyCache >> i) & 1 ? "Key saved" : "No key saved");
  }
  setText(storageValue, freeKbCache ? (String(freeKbCache) + " KB free").c_str() : "");
  setText(storageSub, (String(photoCount) + " photos, " + answerCount + " answers on the device").c_str());
  setText(remoteStatus, !remoteStarted()    ? ""
                        : remoteConnected() ? "Connected"
                                            : "Pair \"Perch\" in your computer's Bluetooth");
  lv_obj_set_style_opa(remoteSlides, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_set_style_opa(remoteMediaPanel, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  // Control Center: restyle only when a toggle actually changed.
  const String cc = String(networkEnabled()) + brightnessLevel + cameraOff + useGemini;
  if (cc != ccSignature) {
    ccSignature = cc;
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
    setText(lv_obj_get_child(ccModel, 0), useGemini ? "G" : "GPT");
    setText(ccBrightLabel, brightnessLevel == 0 ? "100%" : brightnessLevel == 1 ? "55%" : "20%");
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
void refreshPhotos() { photoCount = listPhotos(photoIds, MAX_PHOTOS); }
// ---------- capture ----------
// Runs on the camera task (core 0) so the screen never freezes:
//  1. the viewfinder frame you were looking at becomes the on-screen photo at once;
//  2. the sensor switches to 2048x1536 JPEG for the real photo;
//  3. the result is handed to the UI, then saved to flash in the background.
uint8_t *taskScratch = nullptr;  // owned by the camera task
size_t taskScratchCapacity = 0;
struct CaptureResult {
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  String problem;
  bool blurry = false;
};
// Burst capture, keeping the sharpest frame. Sharpness = variance of a Laplacian
// over the centre of a 1/8-scale decode, where text usually is. Below the floor even the
// best frame is soft, and the user is told to hold still.
constexpr int BURST = 3;
constexpr uint32_t BLUR_FLOOR = 35;
uint32_t sharpness(const uint8_t *jpeg, size_t len, int width, int height) {
  const int w = width / 8, h = height / 8;
  if ((size_t)(w * h * 2) > taskScratchCapacity) {
    free(taskScratch);
    taskScratch = (uint8_t *)ps_malloc(w * h * 2);
    taskScratchCapacity = taskScratch ? w * h * 2 : 0;
  }
  if (!taskScratch || !jpg2rgb565(jpeg, len, taskScratch, JPG_SCALE_8X)) return 0;
  auto gray = [&](int x, int y) {
    const uint16_t v = taskScratch[(y * w + x) * 2] | (taskScratch[(y * w + x) * 2 + 1] << 8);
    return (int)(((v >> 11) << 1) + (((v >> 5) & 63)) + ((v & 31) << 1));  // ~luma, 0..187
  };
  int64_t sum = 0, sumSq = 0;
  int n = 0;
  for (int y = h / 4; y < h * 3 / 4; ++y)
    for (int x = w / 4; x < w * 3 / 4; ++x) {
      const int lap = 4 * gray(x, y) - gray(x - 1, y) - gray(x + 1, y) - gray(x, y - 1) - gray(x, y + 1);
      sum += lap;
      sumSq += (int64_t)lap * lap;
      ++n;
    }
  if (!n) return 0;
  const int64_t mean = sum / n;
  return (uint32_t)(sumSq / n - mean * mean);
}
CaptureResult captureOut;
volatile uint32_t savedPhotoId = 0;
uint16_t *captureScreen = nullptr;  // filled by the task, swapped with photoPixels on accept
volatile bool captureReady = false;
volatile int savesPending = 0;
#define photoSavePending (savesPending > 0)
// Saving to flash takes ~3 s, so it runs on its own low-priority task with its own copies:
// the viewfinder resumes at once and quick successive photos are all kept.
struct SaveJob {
  uint8_t *jpeg;
  size_t len;
  uint8_t *small;
  size_t smallLen;
  uint16_t thumb[THUMB * THUMB];
};
QueueHandle_t saveQueue = nullptr;
void saveTask(void *) {
  freeKbCache = storageFreeBytes() / 1024;
  for (;;) {
    SaveJob *job = nullptr;
    if (xQueueReceive(saveQueue, &job, portMAX_DELAY) != pdTRUE || !job) continue;
    const uint32_t t = millis();
    const uint32_t id = savePhoto(job->jpeg, job->len, job->thumb);
    if (id && job->small) savePhotoScreen(id, job->small, job->smallLen);
    if (id) savedPhotoId = id;
    else Serial.println("STORAGE_ERROR Photo not saved");
    freeKbCache = storageFreeBytes() / 1024;
    Serial.printf("PHOTO_SAVED id=%lu in %lums\n", (unsigned long)id, (unsigned long)(millis() - t));
    free(job->jpeg);
    free(job->small);
    free(job);
    --savesPending;
  }
}

void captureInTask() {
  CaptureResult r;
  const uint32_t t0 = millis();
  bool haveScreen = false;
  if (cameraMode() == CameraMode::Preview) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) {
      convertPreview(f, captureScreen, false);
      esp_camera_fb_return(f);
      haveScreen = true;
    }
  }
  if (!cameraSetMode(CameraMode::Still)) {
    r.problem = "Camera is unavailable.";
  } else {
    // A freshly started sensor needs a couple of frames for exposure to settle.
    for (int i = 0; i < 2; ++i) {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) esp_camera_fb_return(f);
    }
    uint32_t best = 0;
    for (int shot = 0; shot < BURST; ++shot) {
      camera_fb_t *frame = esp_camera_fb_get();
      if (!frame) continue;
      const bool valid = frame->format == PIXFORMAT_JPEG && frame->len >= 4 && frame->buf[0] == 0xff &&
                         frame->buf[1] == 0xd8 && frame->buf[frame->len - 2] == 0xff &&
                         frame->buf[frame->len - 1] == 0xd9;
      const uint32_t score = valid ? sharpness(frame->buf, frame->len, frame->width, frame->height) : 0;
      Serial.printf("BURST %d sharpness=%lu\n", shot, (unsigned long)score);
      if (valid && (!r.jpeg || score > best)) {
        uint8_t *copy = (uint8_t *)ps_malloc(frame->len);
        if (copy) {
          memcpy(copy, frame->buf, frame->len);
          free(r.jpeg);
          r.jpeg = copy;
          r.len = frame->len;
          best = score;
        }
      }
      esp_camera_fb_return(frame);
    }
    if (!r.jpeg) r.problem = "No usable photo. Try again.";
    r.blurry = r.jpeg && best < BLUR_FLOOR;
    if (r.jpeg && !haveScreen)
      haveScreen = decodeStoredWith(r.jpeg, r.len, captureScreen, taskScratch, taskScratchCapacity);
  }
  const uint32_t t1 = millis();
  // Prepare what saving needs (own copies), then hand the photo to the UI at once.
  SaveJob *job = r.jpeg ? (SaveJob *)ps_malloc(sizeof(SaveJob)) : nullptr;
  if (job) {
    job->jpeg = (uint8_t *)ps_malloc(r.len);
    job->len = r.len;
    job->small = nullptr;
    job->smallLen = 0;
    if (job->jpeg) memcpy(job->jpeg, r.jpeg, r.len);
    makeThumb(captureScreen, job->thumb);
    // A screen-sized copy makes browsing Photos fast.
    uint16_t *swapped = (uint16_t *)ps_malloc(W * H * 2);
    if (swapped) {
      for (int i = 0; i < W * H; ++i) swapped[i] = __builtin_bswap16(captureScreen[i]);
      fmt2jpg((uint8_t *)swapped, W * H * 2, W, H, PIXFORMAT_RGB565, 80, &job->small, &job->smallLen);
      free(swapped);
    }
    if (!job->jpeg) {
      free(job->small);
      free(job);
      job = nullptr;
    }
  }
  if (job) {
    ++savesPending;
    if (xQueueSend(saveQueue, &job, 0) != pdTRUE) {
      --savesPending;
      free(job->jpeg);
      free(job->small);
      free(job);
    }
  }
  captureOut = r;
  captureReady = true;
  Serial.printf("SHUTTER capture=%lums handoff=%lums\n", (unsigned long)(t1 - t0), (unsigned long)(millis() - t1));
  if (previewWanted) cameraSetMode(CameraMode::Preview);
}
bool requestCapture(String &problem) {
  if (cameraOff) {
    problem = "Camera is off. Turn it on in Control Center.";
    return false;
  }
  if (captureRequested || captureReady) {
    problem = "Still taking the last photo.";
    return false;
  }
  captureRequested = true;
  return true;
}
// UI thread: take the finished capture. Failures keep the previous photo.
bool acceptCapture(String &problem) {
  if (!captureReady) {
    problem = "The camera did not answer. Try again.";
    return false;
  }
  captureReady = false;
  if (!captureOut.jpeg) {
    problem = captureOut.problem;
    return false;
  }
  if (captureOut.blurry) toast("Blurry. Hold still and try again");
  free(savedJpeg);
  savedJpeg = captureOut.jpeg;
  jpegBytes = jpegCapacity = captureOut.len;
  captureOut.jpeg = nullptr;
  uint16_t *old = photoPixels;
  photoPixels = captureScreen;
  captureScreen = old;
  photoDsc.data = reinterpret_cast<const uint8_t *>(photoPixels);
  lv_image_cache_drop(&photoDsc);
  lv_obj_invalidate(root);
  refreshDynamic();
  return true;
}
// Blocking form for flows that need the photo before continuing (capture-and-ask, mirror).
// LVGL keeps running meanwhile, so animations stay smooth.
bool takePhoto(String &problem) {
  if (!requestCapture(problem)) return false;
  const unsigned long start = millis();
  while (!captureReady && millis() - start < 15000) {
    lv_timer_handler();
    delay(5);
  }
  return acceptCapture(problem);
}
void startAsk() {
  if (!jpegBytes) {
    notice("Take a photo first.", Screen::Ask);
    return;
  }
  if (!networkConnected()) {
    // Badge mode: keep the question and answer it when Wi-Fi returns.
    if (photoSavePending) {
      queueWhenSaved = true;
    } else if (latestPhotoId) {
      queueAdd(latestPhotoId, useGemini, clockKnown() ? (uint32_t)time(nullptr) : 0);
      refreshQueue();
    }
    toast("Saved. It will ask when Wi-Fi is back");
    historyDirty = true;
    rebuildHistory();
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
  pendingPhotoId = photoSavePending ? UINT32_MAX : latestPhotoId;
  lv_label_set_text(busyLabel, useGemini ? "Asking Gemini" : "Asking GPT");
  lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
  show(Screen::Ask);
}
void refreshQueue() {
  queuedCount = queueList(queued, 16);
  historyDirty = true;
}
void captureAndAsk() {
  String problem;
  show(Screen::Ask);
  lv_label_set_text(busyLabel, "Taking photo");
  lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
  const bool ok = takePhoto(problem);
  lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
  if (!ok) {
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
void setX(void *o, int32_t v);
void animX(lv_obj_t *o, int from, int to, uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done);
void showPhoto(int index) {
  if (!photoCount) return;
  const int next = constrain(index, 0, photoCount - 1);
  const int direction = next > photoIndex ? 1 : next < photoIndex ? -1 : 0;
  photoIndex = next;
  if (loadPhotoInto(photoIds[photoIndex], galleryPixels)) {
    lv_image_cache_drop(&galleryDsc);
    lv_obj_invalidate(photosImage);
    // The new photo slides in from the side you swiped toward.
    if (direction) animX(photosImage, direction * 70, 0, 220, lv_anim_path_ease_out, nullptr);
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
  // Rows are rebuilt below; the queue list must be current.
  queuedCount = queueList(queued, 16);
  lv_obj_clean(historyList);
  // With earlier answers, the question screen ends a little short so the list peeks in.
  lv_obj_set_height(askHero, answerCount || queuedCount ? H - ASK_PEEK : H);
  // Questions waiting for Wi-Fi come first: tap a failed one to try again.
  for (int i = queuedCount - 1; i >= 0; --i) {
    lv_obj_t *r = pressedFeedback(plain(historyList));
    lv_obj_set_size(r, W, 56);
    static uint16_t *qthumbs = (uint16_t *)ps_malloc(4 * THUMB * THUMB * 2);  // PSRAM, not internal RAM
    static lv_image_dsc_t qdsc[4];
    int textX = 18;
    uint16_t *qthumb = qthumbs ? qthumbs + i * THUMB * THUMB : nullptr;
    if (i < 4 && qthumb && loadPhotoThumb(queued[i].photoId, qthumb)) {
      lv_image_cache_drop(&qdsc[i]);
      setupImage(qdsc[i], qthumb, THUMB, THUMB);
      lv_obj_t *img = lv_image_create(r);
      lv_image_set_src(img, &qdsc[i]);
      lv_obj_set_size(img, 40, 40);
      lv_image_set_scale(img, 256 * 40 / THUMB);
      lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
      lv_obj_set_style_radius(img, 8, 0);
      lv_obj_set_style_clip_corner(img, true, 0);
      lv_obj_set_style_image_opa(img, LV_OPA_60, 0);
      lv_obj_align(img, LV_ALIGN_LEFT_MID, 18, 0);
      textX = 70;
    }
    const bool inFlight = queued[i].id == queueInFlight;
    lv_obj_t *one = text(r,
                         queued[i].failed ? "Could not ask"
                         : inFlight       ? "Asking now"
                                          : "Waiting for Wi-Fi",
                         F_BODY, queued[i].failed ? INK : MIST);
    lv_obj_set_pos(one, textX, 9);
    lv_obj_t *two = text(r,
                         queued[i].failed   ? "Tap to try again"
                         : queued[i].gemini ? "Gemini"
                                            : "GPT",
                         F_SMALL, queued[i].failed ? LENS : MIST);
    lv_obj_set_pos(two, textX, 33);
    // Hold a waiting question to remove it (nothing is sent).
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
          if (id == queueInFlight) return;
          queueRemove(id);
          refreshQueue();
          rebuildHistory();
          toast("Removed");
        },
        LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)queued[i].id);
    if (queued[i].failed) {
      lv_obj_add_event_cb(
          r,
          [](lv_event_t *e) {
            queueSetFailed((uint32_t)(uintptr_t)lv_event_get_user_data(e), false);
            queueRetryAt = 0;
            refreshQueue();
            rebuildHistory();
            toast(networkConnected() ? "Trying again" : "Will try when Wi-Fi is back");
          },
          LV_EVENT_CLICKED, (void *)(uintptr_t)queued[i].id);
    }
  }
  lv_obj_t *t = text(historyList, answerCount ? "Earlier" : "", F_SMALL, MIST);
  lv_obj_set_style_margin_left(t, 22, 0);
  lv_obj_set_style_margin_top(t, 6, 0);
  lv_obj_set_style_margin_bottom(t, 4, 0);
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
  } else if (s == Screen::Ask) {
    rebuildHistory();
  } else if (s == Screen::Remote) {
    remoteBegin();
  } else if (s == Screen::Wifi) {
    startScan();
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
    if (c == Card::Queue) show(Screen::Ask);
    else if (c == Card::Offline) openControl();
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
// Sizes are set once from the position (no per-frame transforms, which are expensive).
void fisheye() {
  for (lv_obj_t *icon : icons) {
    const float cx = lv_obj_get_x(icon) + lv_obj_get_width(icon) / 2.0f;
    const float cy = lv_obj_get_y(icon) + lv_obj_get_height(icon) / 2.0f;
    const float d = sqrtf(powf((cx - 120) / 120, 2) + powf((cy - 142) / 142, 2));
    const int size = (int)(62 * constrain(1.25f - d * 0.55f, 0.7f, 1.0f));
    lv_obj_set_size(icon, size, size);
    lv_obj_set_pos(icon, (int)cx - size / 2, (int)cy - size / 2);
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
      s, 85, 128, "Remote", ICON_BG, [] { show(Screen::Remote); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_KEYBOARD, INK); });
  appIcon(
      s, 155, 128, "Settings", ICON_BG, [] { show(Screen::Settings); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_SETTINGS, INK); });
  appIcon(
      s, 120, 194, "Gestures", ICON_DIM, [] { planned("Gestures"); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_EYE_OPEN, MIST); });
  lv_obj_t *spacer = plain(s);  // room to scroll, as more apps arrive
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, 330);
  iconName = text(s, "", F_SMALL, INK);
  lv_obj_align(iconName, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN);
  lv_obj_update_layout(s);
  fisheye();
}

void buildCamera() {
  lv_obj_t *s = scr[(int)Screen::Camera] = screenBase();
  viewfinder = lv_image_create(s);
  lv_image_set_src(viewfinder, &liveDsc);
  cameraOffLabel = text(s, "Camera is off.\nTurn it on in Control Center.", F_BODY, MIST);
  lv_obj_set_style_text_align(cameraOffLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(cameraOffLabel, LV_ALIGN_CENTER, 0, -20);
  // Shutter: one ring. Press fills it, so the feedback lands on touch-down.
  shutter = circle(s, 60, INK, 4, INK, LV_OPA_TRANSP);
  lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  lv_obj_set_style_bg_opa(shutter, 215, LV_STATE_PRESSED);
  onClick(shutter, [] {
    String problem;
    if (requestCapture(problem)) flashOnce();
    else notice(problem, Screen::Camera);
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

// One app for asking and for what you asked before: the question is the first screen,
// earlier answers continue below it (scroll down).
void buildAsk() {
  lv_obj_t *s = scr[(int)Screen::Ask] = screenBase();
  askScroll = plain(s);
  lv_obj_set_size(askScroll, W, H);
  lv_obj_add_flag(askScroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(askScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(askScroll, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(askScroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(askScroll, 56, 0);
  askHero = plain(askScroll);
  lv_obj_set_size(askHero, W, H);
  lv_obj_set_style_clip_corner(askHero, false, 0);
  lv_obj_t *hero = askHero;
  askPhoto = lv_image_create(hero);
  lv_image_set_src(askPhoto, &photoDsc);
  askEmpty = text(hero, "No photo yet.", F_BODY, MIST);
  lv_obj_align(askEmpty, LV_ALIGN_CENTER, 0, -30);
  scrimBottom(hero, 120);
  askOffline = text(hero, "Offline", F_SMALL, INK);
  lv_obj_align(askOffline, LV_ALIGN_TOP_MID, 0, 26);
  askHint = text(hero, "Hold for a new photo", F_SMALL, MIST);
  lv_obj_align(askHint, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME - 56);
  historyList = plain(askScroll);
  lv_obj_set_size(historyList, W, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(historyList, LV_FLEX_FLOW_COLUMN);
  askPill = pressedFeedback(plain(hero));
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
  // Long-press the photo: new photo, then ask. Replaces the tiny corner icon of the old UI.
  lv_obj_add_flag(askHero, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      askHero,
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
  // Earlier answers live inside Ask now; this layer stays empty.
  scr[(int)Screen::History] = screenBase();
}

// ---------- Wi-Fi: choose a network and type its password on the device ----------
void startScan() {
  networkScanStart();
  scanShown = false;
  scanStartedAt = millis();
  setText(wifiStatus, networkConnected() ? ("Connected to " + networkName()).c_str() : "Searching...");
}
void joinNetwork(const String &ssid, const String &password) {
  networkSave(ssid, password);
  joiningSsid = ssid;
  toast(("Joining " + ssid).c_str());
}
void renderScan() {
  static ScanResult results[12];
  const int n = networkScanResults(results, 12);
  if (n < 0 || scanShown) return;
  scanShown = true;
  lv_obj_clean(wifiList);
  const String current = networkName();
  setText(wifiStatus, current.length() ? ("Connected to " + current).c_str()
                      : n              ? "Choose a network"
                                       : "No networks found");
  for (int i = 0; i < n; ++i) {
    lv_obj_t *r = pressedFeedback(plain(wifiList));
    lv_obj_set_size(r, W, 50);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, LINE, 0);
    lv_obj_t *name = text(r, results[i].ssid.c_str(), F_BODY, INK);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 160, 20);
    lv_obj_set_pos(name, 22, 7);
    const bool connected = results[i].ssid == current, saved = networkIsSaved(results[i].ssid);
    const char *note = connected ? "Connected" : saved ? "Saved" : results[i].secured ? "" : "Open";
    lv_obj_t *sub = text(r, note, F_SMALL, connected ? LENS : MIST);
    lv_obj_set_pos(sub, 22, 29);
    // Signal: the Wi-Fi glyph, brighter when stronger.
    lv_obj_t *bars = text(r, LV_SYMBOL_WIFI, F_BODY, INK);
    lv_obj_set_style_text_opa(bars,
                              results[i].rssi > -60   ? LV_OPA_COVER
                              : results[i].rssi > -75 ? LV_OPA_70
                                                      : LV_OPA_40,
                              0);
    lv_obj_align(bars, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    // The row keeps its SSID and security in a small heap record for the callbacks.
    struct Row {
      String ssid;
      bool secured;
    };
    Row *row = new Row{results[i].ssid, results[i].secured};
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          Row *row = (Row *)lv_event_get_user_data(e);
          if (!row->secured || networkIsSaved(row->ssid)) {
            joinNetwork(row->ssid, "");
            return;
          }
          joiningSsid = row->ssid;
          setText(passwordTitle, row->ssid.c_str());
          lv_textarea_set_text(passwordField, "");
          show(Screen::Password);
        },
        LV_EVENT_CLICKED, row);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          Row *row = (Row *)lv_event_get_user_data(e);
          if (!networkIsSaved(row->ssid)) return;
          networkForget(row->ssid);
          toast(("Forgot " + row->ssid).c_str());
          startScan();
        },
        LV_EVENT_LONG_PRESSED, row);
    lv_obj_add_event_cb(r, [](lv_event_t *e) { delete (Row *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, row);
  }
}
void buildWifi() {
  lv_obj_t *s = scr[(int)Screen::Wifi] = screenBase();
  lv_obj_set_x(title(s, "Wi-Fi"), 58);
  wifiStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_pos(wifiStatus, 22, 58);
  wifiList = plain(s);
  lv_obj_set_size(wifiList, W, H - 80);
  lv_obj_set_pos(wifiList, 0, 80);
  lv_obj_add_flag(wifiList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(wifiList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(wifiList, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(wifiList, 56, 0);
}
// Keyboard: three layouts, keys as large as 240 px allows, kept above the home strip.
const char *KB_LOWER[] = {"q",  "w",   "e", "r",          "t", "y", "u", "i", "o", "p",
                          "\n", "a",   "s", "d",          "f", "g", "h", "j", "k", "l",
                          "\n", "ABC", "z", "x",          "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  " ", LV_SYMBOL_OK, ""};
const char *KB_UPPER[] = {"Q",  "W",   "E", "R",          "T", "Y", "U", "I", "O", "P",
                          "\n", "A",   "S", "D",          "F", "G", "H", "J", "K", "L",
                          "\n", "abc", "Z", "X",          "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  " ", LV_SYMBOL_OK, ""};
const char *KB_SPECIAL[] = {
    "1",  "2",   "3", "4",          "5",  "6",  "7", "8", "9", "0", "\n", "-", "_", "/", ":", ";",
    "(",  ")",   "@", "&",          "\"", "\n", "#", "%", "*", "+", "=",  ".", ",", "?", "!", LV_SYMBOL_BACKSPACE,
    "\n", "abc", " ", LV_SYMBOL_OK, ""};
const lv_buttonmatrix_ctrl_t KB_CTRL_LETTERS[] = {(lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(4),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(4),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(9),
                                                  (lv_buttonmatrix_ctrl_t)(3)};
const lv_buttonmatrix_ctrl_t KB_CTRL_SPECIAL[] = {(lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(9),
                                                  (lv_buttonmatrix_ctrl_t)(3)};
void buildPassword() {
  lv_obj_t *s = scr[(int)Screen::Password] = screenBase();
  passwordTitle = text(s, "", F_BODY, INK);
  lv_label_set_long_mode(passwordTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_size(passwordTitle, W - 80, 20);
  lv_obj_set_pos(passwordTitle, 58, 22);
  passwordField = lv_textarea_create(s);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Password");
  lv_obj_set_size(passwordField, 166, 36);
  lv_obj_set_pos(passwordField, 14, 54);
  lv_obj_set_style_bg_color(passwordField, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(passwordField, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(passwordField, 0, 0);
  lv_obj_set_style_radius(passwordField, 12, 0);
  lv_obj_set_style_text_color(passwordField, INK, 0);
  lv_obj_set_style_text_font(passwordField, F_BODY, 0);
  lv_obj_set_style_pad_all(passwordField, 8, 0);
  lv_obj_set_style_bg_color(passwordField, LENS, LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(passwordField, LV_OPA_COVER, LV_PART_CURSOR);
  lv_obj_set_style_text_color(passwordField, MIST, LV_PART_TEXTAREA_PLACEHOLDER);
  // Show or hide what was typed.
  lv_obj_t *eye = circle(s, 36, ICON_BG, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_set_pos(eye, 188, 54);
  lv_obj_set_style_opa(eye, LV_OPA_60, LV_STATE_PRESSED);
  static lv_obj_t *eyeGlyph = nullptr;
  eyeGlyph = text(eye, LV_SYMBOL_EYE_OPEN, F_BODY, INK);
  lv_obj_center(eyeGlyph);
  onClick(eye, [] {
    const bool hidden = lv_textarea_get_password_mode(passwordField);
    lv_textarea_set_password_mode(passwordField, !hidden);
    lv_label_set_text(eyeGlyph, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  });
  keyboard = lv_keyboard_create(s);
  lv_keyboard_set_textarea(keyboard, passwordField);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, KB_LOWER, KB_CTRL_LETTERS);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, KB_UPPER, KB_CTRL_LETTERS);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, KB_SPECIAL, KB_CTRL_SPECIAL);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(keyboard, W - 8, HOME_ZONE - 98);
  // Keyboards are bottom-anchored by default; place it under the field, above the home strip.
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 4, 96);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(keyboard, 0, 0);
  lv_obj_set_style_pad_gap(keyboard, 3, 0);
  lv_obj_set_style_border_width(keyboard, 0, 0);
  lv_obj_set_style_bg_color(keyboard, ICON_BG, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 7, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, INK, LV_PART_ITEMS);
  lv_obj_set_style_text_font(keyboard, F_BODY, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, INK, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(keyboard, VOID_, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(keyboard, GRAPHITE, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *) {
        const String password = lv_textarea_get_text(passwordField);
        if (password.length() < 8) {
          toast("Wi-Fi passwords have at least 8 characters");
          return;
        }
        joinNetwork(joiningSsid, password);
        lv_textarea_set_text(passwordField, "");
        goBack();
      },
      LV_EVENT_READY, nullptr);
}

// ---------- Remote: Bluetooth keyboard for slides and media ----------
lv_obj_t *remoteZone(lv_obj_t *parent, int x, int w, const char *symbol, void (*fn)()) {
  lv_obj_t *z = plain(parent);
  lv_obj_set_size(z, w, 146);
  lv_obj_set_pos(z, x, 64);
  lv_obj_set_style_radius(z, 24, 0);
  lv_obj_set_style_bg_color(z, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(z, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(z, ICON_BG, LV_STATE_PRESSED);
  lv_obj_center(text(z, symbol, F_LARGE, INK));
  onClick(z, fn);
  return z;
}
lv_obj_t *remoteButton(lv_obj_t *parent, int x, int y, int size, const char *symbol, void (*fn)()) {
  lv_obj_t *b = circle(parent, size, ICON_BG, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_set_pos(b, x - size / 2, y - size / 2);
  lv_obj_set_style_bg_color(b, INK, LV_STATE_PRESSED);
  lv_obj_t *l = text(b, symbol, size > 56 ? F_LARGE : F_BODY, INK);
  lv_obj_set_style_text_color(l, VOID_, LV_STATE_PRESSED);
  lv_obj_center(l);
  onClick(b, fn);
  return b;
}
void setRemoteMode(bool media) {
  remoteMediaMode = media;
  hide(remoteSlides, media);
  hide(remoteMediaPanel, !media);
  lv_obj_set_style_text_color(remoteModeLabel[0], media ? MIST : INK, 0);
  lv_obj_set_style_text_color(remoteModeLabel[1], media ? INK : MIST, 0);
}
void buildRemote() {
  lv_obj_t *s = scr[(int)Screen::Remote] = screenBase();
  remoteStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_width(remoteStatus, W - 70);
  lv_obj_set_style_text_align(remoteStatus, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(remoteStatus, LV_LABEL_LONG_WRAP);
  lv_obj_align(remoteStatus, LV_ALIGN_TOP_MID, 0, 14);
  // Slides: two big halves. Previous on the left, next on the right.
  remoteSlides = plain(s);
  lv_obj_set_size(remoteSlides, W, 220);
  remoteZone(remoteSlides, 14, 102, LV_SYMBOL_LEFT, [] { remoteKey(RemoteKey::Left); });
  remoteZone(remoteSlides, 124, 102, LV_SYMBOL_RIGHT, [] { remoteKey(RemoteKey::Right); });
  // Media: play/pause in the middle, tracks either side, volume below.
  remoteMediaPanel = plain(s);
  lv_obj_set_size(remoteMediaPanel, W, 220);
  remoteButton(remoteMediaPanel, 120, 112, 72, LV_SYMBOL_PLAY, [] { remoteMedia(RemoteMedia::PlayPause); });
  remoteButton(remoteMediaPanel, 46, 112, 52, LV_SYMBOL_PREV, [] { remoteMedia(RemoteMedia::Previous); });
  remoteButton(remoteMediaPanel, 194, 112, 52, LV_SYMBOL_NEXT, [] { remoteMedia(RemoteMedia::Next); });
  remoteButton(remoteMediaPanel, 86, 180, 44, LV_SYMBOL_MINUS, [] { remoteMedia(RemoteMedia::VolumeDown); });
  remoteButton(remoteMediaPanel, 154, 180, 44, LV_SYMBOL_PLUS, [] { remoteMedia(RemoteMedia::VolumeUp); });
  // Mode switch: one quiet segmented pill.
  lv_obj_t *mode = plain(s);
  lv_obj_set_size(mode, 170, 36);
  lv_obj_align(mode, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME + 6);
  lv_obj_set_style_radius(mode, 18, 0);
  lv_obj_set_style_bg_color(mode, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(mode, LV_OPA_COVER, 0);
  const char *names[2] = {"Slides", "Media"};
  for (int i = 0; i < 2; ++i) {
    lv_obj_t *half = pressedFeedback(plain(mode));
    lv_obj_set_size(half, 85, 36);
    lv_obj_set_pos(half, i * 85, 0);
    remoteModeLabel[i] = text(half, names[i], F_BODY, MIST);
    lv_obj_center(remoteModeLabel[i]);
    lv_obj_add_flag(half, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        half, [](lv_event_t *e) { setRemoteMode(lv_event_get_user_data(e) != nullptr); }, LV_EVENT_CLICKED,
        i ? (void *)1 : nullptr);
  }
  setRemoteMode(false);
}

void buildSettings() {
  lv_obj_t *s = scr[(int)Screen::Settings] = screenBase();
  lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
  title(s, "Settings");
  lv_obj_t *wifiRow = row(s, 58, "Wi-Fi", &wifiValue);
  onClick(wifiRow, [] { show(Screen::Wifi); });
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
  lv_obj_set_x(title(s, "Model"), 58);
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
          goBack();
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
  onClick(ok, [] { goBack(); });
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
  // Hold Wi-Fi for the network list.
  lv_obj_add_event_cb(
      ccWifi,
      [](lv_event_t *) {
        closeControl();
        show(Screen::Wifi);
      },
      LV_EVENT_LONG_PRESSED, nullptr);
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
// Animation probe: timestamps of completed frames, for the 'F' command.
uint32_t frameStamps[64];
volatile int frameCount = 0;
bool probing = false;
void flush(lv_display_t *, const lv_area_t *area, uint8_t *pixels) {
  const int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  // Keep the full-screen copy for the USB mirror (little-endian, before the swap).
  for (int y = 0; y < h; ++y) memcpy(framebuffer + (area->y1 + y) * W + area->x1, pixels + y * w * 2, w * 2);
  // The panel wants big-endian pixels. Send by DMA and return: LVGL renders the next
  // chunk into the other buffer meanwhile; transferDone() releases this one.
  lv_draw_sw_rgb565_swap(pixels, w * h);
  displayDraw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, reinterpret_cast<uint16_t *>(pixels));
  if (probing && lv_display_flush_is_last(display) && frameCount < 64) frameStamps[frameCount++] = micros();
}
void drawDone() { lv_display_flush_ready(display); }
void readTouch(lv_indev_t *, lv_indev_data_t *data) {
  int x = 0, y = 0;
  bool pressed;
  if (injecting) {
    x = injectX;
    y = injectY;
    pressed = true;
  } else pressed = touchRead(x, y);
  static unsigned long moveAt = 0;
  static int speed = 0;   // px/s, smoothed; positive = upward
  static int speedX = 0;  // px/s, smoothed; positive = rightward
  if (pressed) {
    const unsigned long now = millis();
    if (!fingerDown) {
      fingerDown = true;
      downX = x;
      downY = y;
      speed = speedX = 0;
      // Edges belong to the system: bottom drags home, top pulls Control Center,
      // left drags back one level.
      edge = injecting                                                   ? Edge::None
             : downY >= HOME_ZONE                                        ? Edge::Home
             : (downY < TOP_ZONE && !ccOpen)                             ? Edge::Control
             : (downX < BACK_ZONE && !ccOpen && current != Screen::Face) ? Edge::Back
                                                                         : Edge::None;
    } else if (now > moveAt) {
      speed = (speed + (lastY - y) * 1000 / (int)(now - moveAt)) / 2;
      speedX = (speedX + (x - lastX) * 1000 / (int)(now - moveAt)) / 2;
    }
    moveAt = now;
    lastX = x;
    lastY = y;
    if (edge == Edge::Home && downY - y > 6) {
      if (ccOpen) closeControl();
      else dragHome(downY - y);
    }
    if (edge == Edge::Control && y - downY > 6) dragControl(y - downY);
    if (edge == Edge::Back && x - downX > 6) dragBack(x - downX);
  } else if (fingerDown) {
    fingerDown = false;
    if (edge == Edge::Home) releaseHome(max(0, downY - lastY), speed);
    if (edge == Edge::Control) releaseControl(max(0, lastY - downY), speed);
    if (edge == Edge::Back) {
      // A tap on the left edge falls through to the app (so edge controls still work).
      if (lastX - downX <= 6 && abs(lastY - downY) <= 6) {
        edge = Edge::None;
        data->point.x = lastX;
        data->point.y = lastY;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
      }
      releaseBack(max(0, lastX - downX), speedX);
    }
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
    case Screen::Remote: return "remote";
    case Screen::Wifi: return "wifi";
    case Screen::Password: return "password";
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
  previewWanted = false;
  if (!cameraSwitch(CameraMode::Still)) return;
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
  if (!ensurePreviewMode()) {
    Serial.println("CAPTURE FAILED Preview setup failed");
    return;
  }
  xSemaphoreTake(camLock, portMAX_DELAY);
  camera_fb_t *f = esp_camera_fb_get();
  if (!f) {
    xSemaphoreGive(camLock);
    Serial.println("CAPTURE FAILED No preview frame");
    return;
  }
  // Preview frames are raw pixels; compress one for the PC mirror.
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  const bool raw = f->format != PIXFORMAT_JPEG;
  const bool encoded = raw ? frame2jpg(f, 60, &jpeg, &len) : true;
  if (!raw) {
    len = f->len;
    jpeg = (uint8_t *)ps_malloc(len);
    if (jpeg) memcpy(jpeg, f->buf, len);
  }
  esp_camera_fb_return(f);
  xSemaphoreGive(camLock);
  if (!encoded || !jpeg) {
    Serial.println("CAPTURE FAILED Preview encode failed");
    return;
  }
  Serial.printf("JPEG_BEGIN %u\n", len);
  const bool sent = sendAcknowledged(jpeg, len);
  free(jpeg);
  Serial.println(sent ? "\nJPEG_END" : "\nCAPTURE FAILED Preview transfer failed");
}
void restoreLatestPhoto() {
  // The newest stored photo comes back after a reboot, so Ask and the face have it.
  refreshPhotos();
  if (!photoCount) return;
  uint8_t *jpeg;
  size_t len;
  if (!loadPhoto(photoIds[0], jpeg, len)) return;
  if (loadPhotoInto(photoIds[0], photoPixels)) {
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
  for (auto &b : liveBuf) b = (uint16_t *)ps_calloc(W * H, 2);
  livePixels = liveBuf[0];
  camLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(previewTask, "viewfinder", 4096, nullptr, 2, nullptr, 0);
  saveQueue = xQueueCreate(3, sizeof(SaveJob *));
  xTaskCreatePinnedToCore(saveTask, "photo-save", 4096, nullptr, 1, nullptr, 0);
  photoPixels = (uint16_t *)ps_calloc(W * H, 2);
  captureScreen = (uint16_t *)ps_calloc(W * H, 2);
  galleryPixels = (uint16_t *)ps_calloc(W * H, 2);
  historyThumbs = (uint16_t *)ps_calloc(ANSWER_KEEP * THUMB * THUMB, 2);
  // Screen link speed is stored so it can be tuned for the wiring without reflashing ('Y').
  spiMhz = constrain(settings.getUChar("lcd-mhz", 10), 5, 80);
  const bool lcd = displayBegin(spiMhz * 1000000UL, drawDone);
  for (auto &b : drawBuffers)
    b = (uint16_t *)heap_caps_malloc(W * DISPLAY_CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  for (int y = 0; y < H; ++y) {
    // Viewfinder shading: none above the bottom 110 px, then easing to ~33% brightness.
    const int t = y - (H - 110);
    scrimRow[y] = t <= 0 ? 255 : (uint8_t)(255 - (170 * min(t * 100 / 60, 100)) / 100);
  }
  Serial.printf("DISPLAY lcd=%d touch=%d spi=%uMHz\n", lcd, touchAvailable(), spiMhz);
  if (!framebuffer || !captureScreen || !liveBuf[1] || !liveBuf[2] || !livePixels || !photoPixels || !galleryPixels ||
      !historyThumbs || !drawBuffers[0] || !drawBuffers[1]) {
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
  queuedCount = queueList(queued, 16);

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
  lv_display_set_buffers(display, drawBuffers[0], drawBuffers[1], W * DISPLAY_CHUNK_ROWS * 2,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
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
  buildRemote();
  buildWifi();
  buildPassword();
  buildControl();
  // Back arrow for screens two levels deep (Answer from History, Model from Settings...).
  backButton = plain(lv_layer_top());
  lv_obj_set_size(backButton, 34, 34);
  lv_obj_set_pos(backButton, 14, 14);
  lv_obj_set_style_radius(backButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(backButton, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(backButton, 220, 0);
  lv_obj_set_ext_click_area(backButton, 10);
  lv_obj_set_style_opa(backButton, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(backButton, LV_SYMBOL_LEFT, F_BODY, INK));
  onClick(backButton, [] { goBack(); });
  lv_obj_add_flag(backButton, LV_OBJ_FLAG_HIDDEN);
  // First run only: how to move around. Three gestures cover everything.
  if (!settings.getBool("guide-seen2", false)) {
    lv_obj_t *guide = plain(lv_layer_top());
    lv_obj_set_size(guide, W, H);
    lv_obj_set_style_bg_color(guide, VOID_, 0);
    lv_obj_set_style_bg_opa(guide, 245, 0);
    lv_obj_add_flag(guide, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = plain(guide);
    lv_obj_set_size(col, W - 44, LV_SIZE_CONTENT);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);
    const char *lines[][2] = {{LV_SYMBOL_UP "  Swipe up", "on the clock: apps"},
                              {LV_SYMBOL_UP "  Up from the bottom", "home, from anywhere"},
                              {LV_SYMBOL_RIGHT "  Right from the left", "back one step"}};
    for (auto &line : lines) {
      lv_obj_t *a = text(col, line[0], F_BODY, INK);
      lv_obj_t *b = text(col, line[1], F_SMALL, MIST);
      lv_obj_set_style_margin_top(b, -10, 0);
      (void)a;
    }
    lv_obj_t *ok = pill(guide, "Got it", 130);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
    lv_obj_add_event_cb(
        ok,
        [](lv_event_t *e) {
          settings.putBool("guide-seen2", true);
          lv_obj_delete(lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e)));
        },
        LV_EVENT_CLICKED, nullptr);
  }
  toastBox = plain(lv_layer_top());
  lv_obj_set_size(toastBox, LV_SIZE_CONTENT, 30);
  lv_obj_set_style_pad_hor(toastBox, 14, 0);
  lv_obj_set_style_radius(toastBox, 15, 0);
  lv_obj_set_style_bg_color(toastBox, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(toastBox, 240, 0);
  lv_obj_align(toastBox, LV_ALIGN_TOP_MID, 0, 22);
  toastLabel = text(toastBox, "", F_SMALL, INK);
  lv_obj_center(toastLabel);
  lv_obj_add_flag(toastBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(toastBox, LV_OBJ_FLAG_CLICKABLE);
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
    keyCache = -1;
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
    Serial.printf("BENCH full_frame_ms=%.1f fps=%.1f spi=%uMHz\n", total / 10000.0f, 1e7f / total, spiMhz);
    return;
  }
  if (command == 'F') {
    auto run = [](const char *name, void (*start)()) {
      frameCount = 0;
      probing = true;
      const uint32_t t0 = micros();
      start();
      while (micros() - t0 < 450000) {
        lv_timer_handler();
        delay(1);
      }
      probing = false;
      uint32_t worst = 0;
      for (int i = 1; i < frameCount; ++i) worst = max(worst, frameStamps[i] - frameStamps[i - 1]);
      const float span = frameCount > 1 ? (frameStamps[frameCount - 1] - frameStamps[0]) / 1000.0f : 0;
      Serial.printf("ANIM %s frames=%d fps=%.1f worst_gap=%lums first_frame_after=%lums\n", name, frameCount,
                    frameCount > 1 ? (frameCount - 1) * 1000.0f / span : 0, (unsigned long)(worst / 1000),
                    (unsigned long)(frameCount ? (frameStamps[0] - t0) / 1000 : 0));
    };
    run("open_apps", [] { openApps(); });
    run("open_settings", [] { show(Screen::Settings); });
    run("back", [] { goBack(); });
    run("home", [] { leaveTo(Screen::Face, 0); });
    run("open_photos", [] { show(Screen::Photos); });
    uint32_t t = micros();
    showPhoto(photoIndex + 1);
    Serial.printf("PHOTO_SWIPE %lums (of %d photos)\n", (unsigned long)((micros() - t) / 1000), photoCount);
    goBack();
    historyDirty = true;
    run("open_ask", [] { show(Screen::Ask); });
    if (answerCount) {
      t = micros();
      showAnswer(answerIds[0]);
      Serial.printf("OPEN_ANSWER %lums (of %d answers)\n", (unsigned long)((micros() - t) / 1000), answerCount);
    }
    leaveTo(Screen::Face, 0);
    t = micros();
    refreshDynamic();
    Serial.printf("REFRESH_DYNAMIC %lums\n", (unsigned long)((micros() - t) / 1000));
    t = micros();
    volatile size_t freeBytes = storageFreeBytes();
    (void)freeBytes;
    Serial.printf("STORAGE_FREE_QUERY %lums\n", (unsigned long)((micros() - t) / 1000));
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
    case 'H':
      show(Screen::Ask);
      if (answerCount) lv_obj_scroll_to_y(askScroll, H - ASK_PEEK - 30, LV_ANIM_OFF);
      break;
    case 'R': show(Screen::Remote); break;
    case 'W': show(Screen::Wifi); break;
    case 'U':  // developer: clear the offline queue (so test questions are never sent)
      for (int i = 0; i < queuedCount; ++i) queueRemove(queued[i].id);
      refreshQueue();
      rebuildHistory();
      Serial.println("QUEUE_CLEARED");
      break;
    case 'V':  // developer: password screen layout check without a real network
      joiningSsid = "Layout check";
      setText(passwordTitle, joiningSsid.c_str());
      show(Screen::Password);
      break;
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
  if (ai == AiState::Done && queueInFlight) {
    // A queued question came back.
    static uint16_t *qth = (uint16_t *)ps_malloc(THUMB * THUMB * 2);
    uint32_t photoId = 0;
    bool gemini = true;
    for (int i = 0; i < queuedCount; ++i)
      if (queued[i].id == queueInFlight) {
        photoId = queued[i].photoId;
        gemini = queued[i].gemini;
      }
    const bool haveThumb = photoId && qth && loadPhotoThumb(photoId, qth);
    saveAnswer(result, gemini, photoId, clockKnown() ? (uint32_t)time(nullptr) : 0, haveThumb ? qth : nullptr);
    queueRemove(queueInFlight);
    queueInFlight = 0;
    refreshQueue();
    loadLatestAnswer();
    if (current == Screen::Ask) rebuildHistory();
    toast("Answer ready");
    refreshDynamic();
  } else if (ai == AiState::Failed && queueInFlight) {
    queueSetFailed(queueInFlight, true);
    queueInFlight = 0;
    queueRetryAt = millis() + 60000;
    refreshQueue();
    if (current == Screen::Ask) rebuildHistory();
  } else if (ai == AiState::Done) {
    if (pendingPhotoId == UINT32_MAX) pendingPhotoId = latestPhotoId;
    static uint16_t *th = (uint16_t *)ps_malloc(THUMB * THUMB * 2);  // PSRAM: internal RAM is for Wi-Fi/TLS
    const bool haveThumb = pendingPhotoId && th && loadPhotoThumb(pendingPhotoId, th);
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
  if (captureReady && !captureRequested) {
    String problem;
    if (!acceptCapture(problem)) notice(problem, current == Screen::Camera ? Screen::Camera : Screen::Face);
  }
  if (savedPhotoId && savedPhotoId != latestPhotoId) {
    latestPhotoId = savedPhotoId;
    refreshPhotos();
    if (queueWhenSaved) {
      queueWhenSaved = false;
      queueAdd(latestPhotoId, useGemini, clockKnown() ? (uint32_t)time(nullptr) : 0);
      refreshQueue();
      if (current == Screen::Ask) rebuildHistory();
    }
    refreshDynamic();
  }
  // Ask waiting questions once Wi-Fi is back, one at a time, never while you are asking.
  if (queuedCount && !queueInFlight && networkConnected() && !aiBusy() && millis() > queueRetryAt) {
    for (int i = 0; i < queuedCount; ++i) {
      if (queued[i].failed) continue;
      uint8_t *jpeg;
      size_t len;
      if (!loadPhoto(queued[i].photoId, jpeg, len)) {
        queueSetFailed(queued[i].id, true);
        refreshQueue();
        break;
      }
      const String key = settings.getString(queued[i].gemini ? "gemini-key" : "gpt-key", "");
      if (aiStart(queued[i].gemini, key, jpeg, len)) {
        queueInFlight = queued[i].id;
        historyDirty = true;
        if (current == Screen::Ask) rebuildHistory();
      }
      free(jpeg);
      break;
    }
  }
  static unsigned long lastRefresh = 0;
  if (now - lastRefresh > 1000 && !dragging && !fingerDown) {
    lastRefresh = now;
    refreshDynamic();
  }
  // Live viewfinder: the camera task converts frames; the UI shows the newest one.
  static unsigned long lastFpsReport = 0;
  static uint32_t shown = 0, convertedAtReport = 0;
  if (current == Screen::Camera && !cameraOff && !ccOpen && now - lastSerialMs > 3000) {
    if (!previewWanted) ensurePreviewMode();
    if (takeLiveFrame()) {
      lv_image_cache_drop(&liveDsc);
      lv_obj_invalidate(viewfinder);
      ++shown;
    }
    if (now - lastFpsReport >= 5000) {
      const uint32_t converted = framesConverted;
      if (lastFpsReport && converted > convertedAtReport)
        Serial.printf("PREVIEW_FPS %.1f camera=%.1f convert=%lums\n", shown * 1000.0f / (now - lastFpsReport),
                      (converted - convertedAtReport) * 1000.0f / (now - lastFpsReport),
                      (unsigned long)(convertMicros / (converted - convertedAtReport) / 1000));
      lastFpsReport = now;
      shown = 0;
      convertedAtReport = converted;
      convertMicros = 0;
    }
  } else {
    lastFpsReport = 0;
    shown = 0;
    if (current != Screen::Camera || cameraOff) stopPreview();
  }
  lv_timer_handler();
}
