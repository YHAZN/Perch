// Perch OS shell on LVGL 9. Design source of truth: design/index.html.
// Face (clock + Smart Stack), honeycomb app grid, Control Center, Camera, Ask, Answer,
// Photos gallery, History, Settings, Model picker. All apps are layers on one screen.
#include "device_ui.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include "ai_client.h"
#include "audio.h"
#include "battery.h"
#include "camera.h"
#include "clock.h"
#include "board_pins.h"
#include "display.h"
#include "remote.h"
#include "storage.h"

namespace {
constexpr int W = 240, H = 284;
// System edges: drag up from the bottom line (where the home bar is) for home, down from the
// top line for Control Center, right from the left edge for back. An edge swipe starts only
// when the finger moves in that direction; anything else goes to the app underneath.
constexpr int HOME_ZONE = 262;
constexpr int TOP_ZONE = 22;
// Distance from the bottom edge to the lowest tappable control (clear of the home bar).
constexpr int ABOVE_HOME = 30;
// Drag this far (or flick) to commit an edge gesture; less springs back.
constexpr int COMMIT_DRAG = 70;
// When Ask has earlier answers, its first screen ends this short so the list peeks in.
constexpr int ASK_PEEK = 26;
constexpr int MAX_PHOTOS = 12;
// Left strip: drag right from here to go back one level.
constexpr int BACK_ZONE = 24;

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
  Gestures,
  Count
};
Screen current = Screen::Face;
Screen noticeReturn = Screen::Ask;

Preferences settings;
bool useGemini = true;
// AI settings, saved per provider: model and effort (indexes into ai_client's lists).
int geminiModel = 0, geminiEffort = 0, gptModel = 0, gptEffort = 0;
// Ask is a conversation: every question keeps the chat's earlier photos (contextIds, oldest
// first) and earlier answers as context, until "New chat". Works the same for every provider.
std::vector<uint32_t> contextIds;
std::vector<uint32_t> sessionAnswers;  // answer ids in this chat, oldest first
std::vector<String> sessionQuestions;  // what was asked for each ("" = about the photo)
String pendingQuestion;
bool chatFresh = false;  // "New chat" pressed: the next photo starts clean
bool appendingPage = false;
// What the running AI request is for: an answer, or writing down what was said.
enum class Pending { None, Answer, Transcribe } pending = Pending::None;
String heardText;
// Text entry is shared: Wi-Fi password or a typed question. The Talk key dictates into it.
enum class TextMode { Password, Question } textMode = TextMode::Password;
bool dictating = false;    // the Talk key is held / its words are on their way
bool confirmWords = true;  // show what was said before asking (Settings > AI)
bool sendAfterTranscribe = false;
bool cameraOff = false;
// Bluetooth is a system setting (Control Center). It pauses only while an AI request runs:
// with it on, the ESP32-S3 has too little internal RAM left for HTTPS (measured: ~50 KB free,
// uploads failed). It comes back by itself and a paired computer reconnects.
bool bluetoothOn = false;
bool btPaused = false;
uint8_t brightnessPct = 100;  // 5..100, continuous (Control Center and Settings sliders)
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
lv_obj_t *faceWifi, *faceBt, *faceBattery;
lv_obj_t *clockLabel, *dateLabel, *faceOffline, *card, *cardImage, *cardScrim, *cardRing, *cardKey, *cardValue,
    *cardDots;
lv_obj_t *iconName;
std::vector<lv_obj_t *> icons;
lv_obj_t *viewfinder, *thumb, *flash, *shutter, *cameraOffLabel;
lv_obj_t *askPhoto, *askEmpty, *askPill, *askButtonLabel, *askOffline, *busy, *busyRing, *busyLabel, *askSheet;
lv_obj_t *answerScroll, *answerPhoto, *answerFlow, *answerMeta;
lv_obj_t *photosImage, *photosEmpty, *photoCounter, *photoPrev, *photoNext, *photoDelete;
// Zoom: the touch panel (CST816D) senses one finger, so no pinch. Double-tap decodes the full
// photo at half resolution (2.7x the screen) and shows it in a window you drag around.
lv_obj_t *zoomView, *zoomImage, *zoomBlur, *zoomExit;
// The sharp version is decoded on a background task (~3 s for a 2048x1536 photo); until it
// arrives the screen copy is shown enlarged at once, around the point you tapped.
volatile int zoomJob = 0;  // 0 idle, 1 decoding, 2 ready, 3 failed
uint32_t zoomPhotoId = 0;
int zoomTapX = 0, zoomTapY = 0;
uint16_t *zoomPixels = nullptr;
lv_image_dsc_t zoomDsc;
int zoomW = 0, zoomH = 0;
unsigned long lastPhotoTap = 0;
lv_obj_t *historyList, *historyEmpty, *askScroll, *askHero;
lv_obj_t *remoteStatus, *remoteSlides, *remoteMediaPanel, *remoteModeLabel[2];
bool remoteMediaMode = false;
// Offline queue state (see storage.h). queueInFlight is the item being asked right now.
QueuedAsk queued[16];
int queuedCount = 0;
uint32_t queueInFlight = 0;
bool queueWhenSaved = false;
lv_obj_t *recordingDot = nullptr;
lv_obj_t *listenOverlay = nullptr, *listenRing = nullptr, *listenTime = nullptr;
unsigned long queueRetryAt = 0;
void refreshQueue();
void rebuildHistory();
void restoreLatestPhoto();
void refreshPhotos();
extern String cardSignature;
lv_obj_t *wifiList, *wifiStatus, *passwordTitle, *passwordField, *keyboard;
String joiningSsid;
bool scanShown = false;
unsigned long scanStartedAt = 0;
lv_obj_t *modelValue, *wifiValue, *brightSlider, *storageValue, *storageSub;
lv_obj_t *modelCheck[2], *modelSub[2];
lv_obj_t *noticeText;
// Ask: pages and "what you said"
lv_obj_t *typeBtn, *passwordEye;
lv_obj_t *pageBar, *plusBtn, *plusBadge, *newChatBtn, *pageBanner, *micBtn;
lv_obj_t *heardSheet, *heardLabel;
bool pageCamera = false;  // the camera was opened from Ask ("New photo" / "+ Page"); return after the shot
void newPhoto();
void pauseBluetooth();
void setBluetooth(bool on);
void showHeard(const String &words);
void addPage();
int pageCount();
void refreshAiScreen();
// AI settings screen
lv_obj_t *aiUseCheck[2], *aiUseSub[2], *wordsCheck, *wordsSub;
lv_obj_t *gemModelCheck[8], *gptModelCheck[8];
lv_obj_t *effortSeg[2][AI_EFFORT_COUNT];
String aiScreenSignature;
lv_obj_t *homeBar, *topBar;
lv_obj_t *ccBt, *ccBtLabel, *ccWifiState, *ccBtState, *ccCameraState, *ccModelState;
lv_obj_t *cc, *ccWifi, *ccWifiLabel, *ccBright, *ccModel, *ccModelLabel, *ccCamera, *ccCameraLabel;
bool ccOpen = false;

// Touch
bool injecting = false;
bool injectEdges = false;  // injected drags exercise the system edges like a finger
int injectX = 0, injectY = 0;
bool fingerDown = false;
enum class Edge { None, Home, Control, Back } edge = Edge::None;
int downX = 0, downY = 0, lastX = 0, lastY = 0;

// ---------- widget helpers ----------
// Plain objects are decoration by default: not clickable, so a tap on an icon's glyph or a
// row's label reaches the button underneath. Anything that reacts to touch opts in.
lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
// Scrolling lists must receive presses on their empty space to scroll.
void scrollable(lv_obj_t *o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
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
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
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
// Confirmation sheet for destructive actions: reached by holding something, confirmed by a
// tap on the red action. Tapping outside cancels.
lv_obj_t *confirmLayer = nullptr, *confirmTitle = nullptr, *confirmAction = nullptr;
void (*confirmFn)(uint32_t) = nullptr;
uint32_t confirmArg = 0;
void confirm(const char *title, const char *action, void (*fn)(uint32_t), uint32_t arg) {
  if (!confirmLayer) return;
  lv_label_set_text(confirmTitle, title);
  lv_label_set_text(confirmAction, action);
  confirmFn = fn;
  confirmArg = arg;
  lv_obj_remove_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(confirmLayer);
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
// Compact pill for secondary actions on top of a photo: 34 px tall, 46 px touch target.
lv_obj_t *chip(lv_obj_t *parent, const char *label) {
  lv_obj_t *c = pressedFeedback(plain(parent));
  lv_obj_set_size(c, LV_SIZE_CONTENT, 34);
  lv_obj_set_style_pad_hor(c, 14, 0);
  lv_obj_set_style_radius(c, 17, 0);
  lv_obj_set_style_bg_color(c, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(c, 230, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  text(c, label, F_SMALL, INK);
  lv_obj_set_ext_click_area(c, 6);
  return c;
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
// Gestures without a PC or a model: a 32x24 brightness grid of each preview frame.
//  - background: learned while the area is clear (and slowly afterwards where no hand is)
//  - hand present: enough cells differ from the background
//  - arm gate: hand present and still for 300 ms -> armed for 3 s (avoids the "Midas touch")
//  - swipe: while armed, the centre of motion travels > 40% of the width within 500 ms
constexpr int GX = 32, GY = 24, GCELLS = GX * GY;
volatile bool gestureActive = false;
volatile int gestureCalibrating = 0;  // frames left to learn the background
volatile int gestureState = 0;        // 0 no hand, 1 hand, 2 armed
volatile int gestureSwipe = 0;        // -1 left, +1 right (consumed by the UI)
volatile uint32_t gestureSeq = 0;
int16_t gBackground[GCELLS], gPrevious[GCELLS];
void analyseGesture(const camera_fb_t *f) {
  if (f->format != PIXFORMAT_RGB565) return;
  const int sw = f->width, sh = f->height;
  static int16_t lum[GCELLS];
  for (int gy = 0; gy < GY; ++gy)
    for (int gx = 0; gx < GX; ++gx) {
      const uint8_t *px = f->buf + ((gy * sh / GY + sh / GY / 2) * sw + gx * sw / GX + sw / GX / 2) * 2;
      const uint16_t v = (px[0] << 8) | px[1];
      lum[gy * GX + gx] = ((v >> 11) << 1) + ((v >> 5) & 63) + ((v & 31) << 1);
    }
  const uint32_t now = millis();
  if (gestureCalibrating > 0) {
    for (int i = 0; i < GCELLS; ++i)
      gBackground[i] = gestureCalibrating == 12 ? lum[i] : (gBackground[i] * 3 + lum[i]) / 4;
    memcpy(gPrevious, lum, sizeof(lum));
    --gestureCalibrating;
    return;
  }
  int foreground = 0, motion = 0, motionX = 0;
  for (int i = 0; i < GCELLS; ++i) {
    const bool fg = abs(lum[i] - gBackground[i]) > 18;
    foreground += fg;
    if (abs(lum[i] - gPrevious[i]) > 15) {
      ++motion;
      motionX += i % GX;
    }
    // Learn slowly where there is no hand, so lighting drift does not look like a hand.
    if (!fg) gBackground[i] += (lum[i] - gBackground[i]) / 16;
  }
  memcpy(gPrevious, lum, sizeof(lum));
  static uint32_t stillSince = 0, armedUntil = 0, cooldownUntil = 0, trackStart = 0;
  static int trackStartX = -1, lastX = -1;
  const bool hand = foreground > GCELLS / 20 && foreground < GCELLS * 6 / 10;
  const bool still = motion < GCELLS / 33;
  if (hand && still) {
    if (!stillSince) stillSince = now;
    if (now - stillSince > 300) armedUntil = now + 3000;
  } else {
    stillSince = 0;
  }
  const bool armed = now < armedUntil;
  if (armed && motion > GCELLS / 25 && now > cooldownUntil) {
    const int x = motionX / motion;
    if (trackStartX < 0 || now - trackStart > 500) {
      trackStartX = x;
      trackStart = now;
    }
    lastX = x;
    if (abs(lastX - trackStartX) > GX * 4 / 10) {
      gestureSwipe = lastX > trackStartX ? 1 : -1;
      ++gestureSeq;
      Serial.printf("GESTURE swipe=%d\n", gestureSwipe);
      cooldownUntil = now + 700;
      armedUntil = now + 1500;  // stay armed briefly for chains
      trackStartX = -1;
    }
  } else if (!armed || motion <= GCELLS / 25) {
    trackStartX = -1;
  }
  const int state = armed ? 2 : hand ? 1 : 0;
  if (state != gestureState) Serial.printf("GESTURE state=%d fg=%d motion=%d\n", state, foreground, motion);
  gestureState = state;
}
volatile bool captureRequested = false;
volatile bool captureProcessing = false;  // the save task is preparing the last photo
volatile bool cameraFailed = false;       // the driver did not start (shown, retried every 2 s)
bool cameraOffSetting();
// The camera task owns the camera: it starts and stops the driver (0.3 s, longer the first
// time) so the UI never waits for it. The UI only says what it wants (previewWanted).
void previewTask(void *) {
  // Load the autofocus firmware once at boot, in the background (~5 s of SCCB writes): it
  // stays in the sensor while it is powered, so the first live view starts quickly.
  if (!cameraOffSetting()) {
    xSemaphoreTake(camLock, portMAX_DELAY);
    if (cameraSetMode(CameraMode::Preview) && !previewWanted) cameraSetMode(CameraMode::Off);
    xSemaphoreGive(camLock);
  }
  for (;;) {
    if (captureRequested) {
      xSemaphoreTake(camLock, portMAX_DELAY);
      captureInTask();
      xSemaphoreGive(camLock);
      captureRequested = false;
      continue;
    }
    const bool want = previewWanted;
    if (want != (cameraMode() == CameraMode::Preview)) {
      xSemaphoreTake(camLock, portMAX_DELAY);
      const bool ok = cameraSetMode(want ? CameraMode::Preview : CameraMode::Off);
      xSemaphoreGive(camLock);
      cameraFailed = want && !ok;
      if (cameraFailed) vTaskDelay(pdMS_TO_TICKS(2000));  // say so on screen; retry calmly
      continue;
    }
    if (!want) {
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
        if (gestureActive) analyseGesture(f);
        convertPreview(f, liveBuf[back]);
        convertMicros += micros() - t;
        esp_camera_fb_return(f);
        got = true;
      }
    }
    xSemaphoreGive(camLock);
    if (!got) {
      static uint32_t misses = 0;
      if (++misses % 100 == 1) Serial.printf("PREVIEW no frame (%lu)\n", (unsigned long)misses);
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
// Non-blocking: the camera task switches modes.
void stopPreview() { previewWanted = false; }
bool ensurePreviewMode(uint32_t waitMs = 0) {
  previewWanted = true;
  const unsigned long start = millis();
  while (waitMs && cameraMode() != CameraMode::Preview && millis() - start < waitMs) delay(20);
  return !waitMs || cameraMode() == CameraMode::Preview;
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
extern std::vector<std::pair<lv_obj_t *, lv_obj_t *>> screenTitles;
lv_obj_t *backButton = nullptr;
void enter(Screen next) {
  // Automatic Wi-Fi retries would cancel scans; pause them while choosing a network.
  networkPauseRetries(next == Screen::Wifi || next == Screen::Password);
  if (next != Screen::Camera) stopPreview();
  current = next;
  onEnter(next);
  refreshDynamic();
  hide(homeBar, false);  // the bottom line always does something: grid, home, or close
  // The back arrow appears on screens you reached from inside another app (two levels deep).
  const bool showBack = !backStack.empty() && backStack.back() != Screen::Face && backStack.back() != Screen::Apps &&
                        next != Screen::Notice;
  if (backButton) hide(backButton, !showBack);
  for (auto &pair : screenTitles)
    if (pair.first == layer(next)) lv_obj_set_x(pair.second, showBack ? 58 : 22);
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
// Open an app the way the grid does: its back history starts at the grid.
void launch(Screen app) {
  backStack.clear();
  if (current != Screen::Apps && current != Screen::Face) {
    lv_obj_add_flag(layer(current), LV_OBJ_FLAG_HIDDEN);
    current = Screen::Apps;
  }
  show(app);
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
void ccClosed(lv_anim_t *) {
  lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
  if (topBar) lv_obj_remove_flag(topBar, LV_OBJ_FLAG_HIDDEN);
}
void openControl(int fromY = -H) {
  ccOpen = true;
  if (topBar) lv_obj_add_flag(topBar, LV_OBJ_FLAG_HIDDEN);
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
void dragControlClosed(int up) {
  if (!ccOpen) return;
  lv_obj_set_y(cc, -max(0, up));
}
void releaseControlClosed(int up, int speed) {
  if (up > COMMIT_DRAG / 2 || (up > 16 && speed > 600)) closeControl(-up);
  else animY(cc, -up, 0, 200, lv_anim_path_ease_out, nullptr);
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
// ---------- answer formatting ----------
// Models write Markdown-ish text. The answer screen turns it into blocks: the answer in large
// type, numbered steps with a badge, bullets, formulas on a tinted panel, headings in small
// caps, and **key terms** in amber. LaTeX that slips through becomes plain ASCII math.
int closingBrace(const String &t, int open) {
  int depth = 0;
  for (int i = open; i < (int)t.length(); ++i) {
    if (t[i] == '{') ++depth;
    else if (t[i] == '}' && --depth == 0) return i;
  }
  return -1;
}
String cleanMath(String t) {
  static const char *const pairs[][2] = {{"\\left", ""},
                                         {"\\right", ""},
                                         {"\\(", ""},
                                         {"\\)", ""},
                                         {"\\[", ""},
                                         {"\\]", ""},
                                         {"$$", ""},
                                         {"\\cdot", "*"},
                                         {"\\times", "x"},
                                         {"\\div", "/"},
                                         {"\\leq", "<="},
                                         {"\\geq", ">="},
                                         {"\\le", "<="},
                                         {"\\ge", ">="},
                                         {"\\neq", "!="},
                                         {"\\ne", "!="},
                                         {"\\pm", "+/-"},
                                         {"\\approx", "~="},
                                         {"\\pi", "pi"},
                                         {"\\infty", "inf"},
                                         {"\\theta", "theta"},
                                         {"\\alpha", "alpha"},
                                         {"\\beta", "beta"},
                                         {"\\Delta", "Delta"},
                                         {"\\log", "log"},
                                         {"\\ln", "ln"},
                                         {"\\sin", "sin"},
                                         {"\\cos", "cos"},
                                         {"\\tan", "tan"},
                                         {"\\to", "->"},
                                         {"\\rightarrow", "->"},
                                         {"\\Rightarrow", "=>"},
                                         {"\\,", " "},
                                         {"\\;", " "},
                                         {"\\quad", "  "},
                                         {"\\!", ""},
                                         {"\\text{", "{"},
                                         {"\\mathrm{", "{"},
                                         {"\\mathbf{", "{"},
                                         {"\\displaystyle", ""},
                                         {"\\dfrac{", "\\frac{"},
                                         {"\\tfrac{", "\\frac{"}};
  for (const auto &p : pairs) t.replace(p[0], p[1]);
  // A single token needs no brackets: 1/2, x^2; anything longer keeps them: (x+1)/(2), x^(n+1).
  auto group = [](const String &g) -> String {
    for (size_t i = 0; i < g.length(); ++i)
      if (!isalnum((unsigned char)g[i]) && g[i] != '.') return "(" + g + ")";
    return g;
  };
  // \frac{a}{b} -> a/b, \sqrt{x} -> sqrt(x)
  for (int guard = 0; guard < 40; ++guard) {
    const int at = t.indexOf("\\frac{");
    if (at < 0) break;
    const int a1 = closingBrace(t, at + 5);
    if (a1 < 0 || a1 + 1 >= (int)t.length() || t[a1 + 1] != '{') break;
    const int b1 = closingBrace(t, a1 + 1);
    if (b1 < 0) break;
    t = t.substring(0, at) + group(t.substring(at + 6, a1)) + "/" + group(t.substring(a1 + 2, b1)) +
        t.substring(b1 + 1);
  }
  for (int guard = 0; guard < 40; ++guard) {
    const int at = t.indexOf("\\sqrt{");
    if (at < 0) break;
    const int b = closingBrace(t, at + 5);
    if (b < 0) break;
    t = t.substring(0, at) + "sqrt(" + t.substring(at + 6, b) + ")" + t.substring(b + 1);
  }
  // x^{2} -> x^2, x^{n+1} -> x^(n+1); the same for subscripts.
  for (const char *mark : {"^{", "_{"}) {
    for (int guard = 0; guard < 40; ++guard) {
      const int at = t.indexOf(mark);
      if (at < 0) break;
      const int b = closingBrace(t, at + 1);
      if (b < 0) break;
      t = t.substring(0, at + 1) + group(t.substring(at + 2, b)) + t.substring(b + 1);
    }
  }
  t.replace("{", "");
  t.replace("}", "");
  t.replace("$", "");
  t.replace("`", "");
  return t;
}
String stripMarks(String t) {
  t.replace("**", "");
  t.replace("__", "");
  return t;
}
// "Q2: c", "Question 3a: 12", "Q4) yes": one question's answer. Fills number and answer.
bool questionLine(const String &line, String &number, String &answer) {
  String t = stripMarks(line);
  t.trim();
  String lower = t;
  lower.toLowerCase();
  int i = 0;
  if (lower.startsWith("question")) i = 8;
  else if (lower.startsWith("q")) i = 1;
  else return false;
  while (i < (int)t.length() && t[i] == ' ') ++i;
  const int start = i;
  while (i < (int)t.length() && isdigit((unsigned char)t[i])) ++i;
  if (i == start) return false;
  while (i < (int)t.length() && isalpha((unsigned char)t[i]) && i - start < 4) ++i;  // 3a, 3b
  number = t.substring(start, i);
  if (i >= (int)t.length() || (t[i] != ':' && t[i] != '.' && t[i] != ')')) return false;
  answer = t.substring(i + 1);
  answer.trim();
  return answer.length() > 0;
}
// The one-line answer: the "Answer:" line if there is one, else the first line. For the
// card on the clock face and the Ask history.
String answerHeadline(const String &raw) {
  String body = cleanMath(raw);
  int from = 0;
  String first, answers;
  while (from <= (int)body.length()) {
    int end = body.indexOf('\n', from);
    if (end < 0) end = body.length();
    String line = stripMarks(body.substring(from, end));
    line.trim();
    from = end + 1;
    if (line.isEmpty()) continue;
    String lower = line;
    lower.toLowerCase();
    String number, value;
    if (questionLine(line, number, value)) {
      answers += (answers.length() ? "  " : "") + String("Q") + number + " " + value;
      continue;
    }
    if (lower.startsWith("answer:") || lower.startsWith("final answer:")) {
      value = line.substring(line.indexOf(':') + 1);
      value.trim();
      answers += (answers.length() ? "  " : "") + value;
      continue;
    }
    if (first.isEmpty()) first = line;
  }
  if (answers.length()) return answers;
  while (first.startsWith("#")) first = first.substring(1);
  first.trim();
  return first;
}
// Body text that may contain **bold**: a label, or a span group when there is emphasis.
lv_obj_t *flowText(lv_obj_t *parent, const String &line, const lv_font_t *font, lv_color_t color, int width) {
  if (line.indexOf("**") < 0) {
    lv_obj_t *l = text(parent, line.c_str(), font, color);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 4, 0);
    return l;
  }
  lv_obj_t *sg = lv_spangroup_create(parent);
  lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_width(sg, width);
  lv_obj_set_height(sg, LV_SIZE_CONTENT);
  lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
  lv_obj_set_style_text_font(sg, font, 0);
  lv_obj_set_style_text_color(sg, color, 0);
  lv_obj_set_style_text_line_space(sg, 4, 0);
  int from = 0;
  bool bold = false;
  while (true) {
    const int at = line.indexOf("**", from);
    const String seg = line.substring(from, at < 0 ? line.length() : at);
    if (seg.length()) {
      lv_span_t *sp = lv_spangroup_new_span(sg);
      lv_span_set_text(sp, seg.c_str());
      if (bold) lv_style_set_text_color(lv_span_get_style(sp), LENS);
    }
    if (at < 0) break;
    bold = !bold;
    from = at + 2;
  }
  lv_spangroup_refr_mode(sg);
  return sg;
}
// A badge (number) or dot beside wrapped text, with a hanging indent.
lv_obj_t *markedRow(const char *mark, const String &rest) {
  lv_obj_t *r = plain(answerFlow);
  lv_obj_set_size(r, W - 40, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(r, 10, 0);
  if (mark) {
    lv_obj_t *badge = circle(r, 24, LINE, 0, GRAPHITE, LV_OPA_COVER);
    lv_obj_center(text(badge, mark, F_SMALL, INK));
  } else {
    lv_obj_t *dot = circle(r, 6, MIST, 0, MIST, LV_OPA_COVER);
    lv_obj_set_style_margin_top(dot, 8, 0);
    lv_obj_set_style_margin_left(dot, 9, 0);
    lv_obj_set_style_margin_right(dot, 9, 0);
  }
  lv_obj_t *t = flowText(r, rest, F_BODY, INK, W - 40 - 34);
  if (mark) lv_obj_set_style_margin_top(t, 2, 0);
  return r;
}
lv_obj_t *panel(const String &content) {
  lv_obj_t *b = plain(answerFlow);
  lv_obj_set_size(b, W - 40, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(b, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_pad_all(b, 10, 0);
  flowText(b, content, F_BODY, INK, W - 60);
  return b;
}
void heading(const String &t) {
  String h = stripMarks(t);
  while (h.startsWith("#")) h = h.substring(1);
  if (h.endsWith(":")) h = h.substring(0, h.length() - 1);
  h.trim();
  h.toUpperCase();
  lv_obj_t *l = text(answerFlow, h.c_str(), F_SMALL, MIST);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  lv_obj_set_style_margin_top(l, 6, 0);
}
// Mostly symbols and numbers, few words: show it as a formula.
bool looksLikeFormula(const String &t) {
  if (t.length() > 70 || (t.indexOf('=') < 0 && t.indexOf('^') < 0)) return false;
  int words = 0, run = 0;
  for (size_t i = 0; i <= t.length(); ++i) {
    const char c = i < t.length() ? t[i] : ' ';
    if (isalpha((unsigned char)c)) ++run;
    else {
      if (run > 3) ++words;
      run = 0;
    }
  }
  return words <= 2;
}
void renderAnswer(const String &raw) {
  lv_obj_clean(answerFlow);
  String body = cleanMath(raw);
  body.replace("\r", "");
  bool sawAnswer = false, firstBlock = true, inCode = false;
  String code;
  int from = 0;
  while (from <= (int)body.length()) {
    int end = body.indexOf('\n', from);
    if (end < 0) end = body.length();
    const String line = body.substring(from, end);
    from = end + 1;
    String t = line;
    t.trim();
    if (t.startsWith("```")) {
      if (inCode && code.length()) panel(code);
      code = "";
      inCode = !inCode;
      continue;
    }
    if (inCode) {
      code += (code.length() ? "\n" : "") + line;
      continue;
    }
    if (t.isEmpty() || t == "---" || t == "***") continue;
    String lower = stripMarks(t);
    lower.toLowerCase();
    // A question's answer (pages often have several): a label with its number, then large type.
    String qNumber, qAnswer;
    if (questionLine(t, qNumber, qAnswer)) {
      lv_obj_t *label = text(answerFlow, (String("QUESTION ") + qNumber).c_str(), F_SMALL, LENS);
      lv_obj_set_style_text_letter_space(label, 1, 0);
      if (sawAnswer) lv_obj_set_style_margin_top(label, 10, 0);
      flowText(answerFlow, qAnswer, F_LARGE, INK, W - 40);
      sawAnswer = true;
      firstBlock = false;
      continue;
    }
    // The answer itself: large, under a small label (every one, not only the first).
    if (lower.startsWith("answer:") || lower.startsWith("final answer:")) {
      String value = stripMarks(t);
      value = value.substring(value.indexOf(':') + 1);
      value.trim();
      lv_obj_t *label = text(answerFlow, "ANSWER", F_SMALL, LENS);
      lv_obj_set_style_text_letter_space(label, 1, 0);
      flowText(answerFlow, value, F_LARGE, INK, W - 40);
      sawAnswer = true;
      firstBlock = false;
      continue;
    }
    if (t.startsWith("#") || (t.endsWith(":") && t.length() < 34 && !isdigit((unsigned char)t[0]))) {
      heading(t);
      firstBlock = false;
      continue;
    }
    // "1." / "1)" steps
    int digits = 0;
    while (digits < (int)t.length() && digits < 3 && isdigit((unsigned char)t[digits])) ++digits;
    if (digits && digits + 1 < (int)t.length() && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ') {
      String rest = t.substring(digits + 2);
      rest.trim();
      markedRow(t.substring(0, digits).c_str(), rest);
      firstBlock = false;
      continue;
    }
    if ((t.startsWith("- ") || t.startsWith("* ") || t.startsWith("+ ")) && t.length() > 2) {
      String rest = t.substring(2);
      rest.trim();
      markedRow(nullptr, rest);
      firstBlock = false;
      continue;
    }
    if (t.startsWith("|")) {
      if (t.indexOf("---") >= 0) continue;  // table rule
      String cells = t.substring(1, t.endsWith("|") ? t.length() - 1 : t.length());
      cells.replace("|", "  -  ");
      cells.trim();
      t = cells;
    }
    if (t.startsWith("[Answer cut off]")) {
      text(answerFlow, "The answer was cut off.", F_SMALL, MIST);
      continue;
    }
    if (looksLikeFormula(stripMarks(t))) {
      panel(t);
      firstBlock = false;
      continue;
    }
    // Without an "Answer:" line, a short first sentence still leads in large type.
    if (firstBlock && !sawAnswer && t.length() <= 120) {
      flowText(answerFlow, t, F_LARGE, INK, W - 40);
      firstBlock = false;
      continue;
    }
    flowText(answerFlow, t, F_BODY, INK, W - 40);
    firstBlock = false;
  }
  if (inCode && code.length()) panel(code);
}

void showAnswer(uint32_t id) {
  String body;
  AnswerInfo info;
  if (!loadAnswer(id, body, info)) {
    notice("That answer could not be read.", Screen::Ask);
    return;
  }
  renderAnswer(body);
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
String cardSignature = "";
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
    setText(cardValue, answerHeadline(lastAnswer).c_str());
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
volatile bool freeKbStale = false;
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
  {
    const lv_color_t wifiColor = online ? INK : MIST;
    if (!lv_color_eq(lv_obj_get_style_text_color(faceWifi, 0), wifiColor))
      lv_obj_set_style_text_color(faceWifi, wifiColor, 0);
    const lv_color_t btColor = remoteConnected() ? INK : MIST;
    if (!lv_color_eq(lv_obj_get_style_text_color(faceBt, 0), btColor)) lv_obj_set_style_text_color(faceBt, btColor, 0);
    hide(faceBt, !bluetoothOn);
    const int battery = batteryPercent();
    hide(faceBattery, battery < 0);
    if (battery >= 0) {
      setText(faceBattery, (String(battery) + "%").c_str());
      const lv_color_t c = battery <= 15 ? lv_color_hex(0xFF5A4E) : INK;
      if (!lv_color_eq(lv_obj_get_style_text_color(faceBattery, 0), c)) lv_obj_set_style_text_color(faceBattery, c, 0);
    }
  }
  hide(askOffline, online || !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN));
  renderCard();
  // Ask: offline is stated on the button itself, not hidden.
  const bool photo = jpegBytes > 0;
  hide(thumb, !photo || cameraOff);
  hide(askPhoto, !photo);
  hide(askEmpty, photo);
  const int pages = pageCount();
  setText(askButtonLabel, !photo ? "Photo" : "Ask");
  hide(micBtn, !photo);
  hide(typeBtn, !photo);
  hide(plusBtn, !photo);
  hide(newChatBtn, !photo || (sessionAnswers.empty() && contextIds.empty()));
  hide(plusBadge, pages < 2);
  if (pages > 1) setText(lv_obj_get_child(plusBadge, 0), String(pages).c_str());
  hide(pageBanner, !pageCamera);
  const lv_opa_t pillOpa = online ? LV_OPA_COVER : LV_OPA_60;
  if (lv_obj_get_style_opa(askPill, 0) != pillOpa) lv_obj_set_style_opa(askPill, pillOpa, 0);
  // Camera privacy
  hide(viewfinder, cameraOff);
  hide(shutter, cameraOff);
  hide(cameraOffLabel, !cameraOff && !cameraFailed);
  setText(cameraOffLabel,
          cameraOff ? "Camera is off.\nTurn it on in Control Center." : "The camera did not start.\nTrying again...");
  // Settings and Model
  setText(modelValue, useGemini ? GEMINI_MODELS[constrain(geminiModel, 0, GEMINI_MODEL_COUNT - 1)].label
                                : GPT_MODELS[constrain(gptModel, 0, GPT_MODEL_COUNT - 1)].label);
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
  if (current == Screen::Model) refreshAiScreen();
  setText(storageValue, freeKbCache ? (String(freeKbCache) + " KB free").c_str() : "");
  setText(storageSub, (String(photoCount) + " photos, " + answerCount + " answers on the device").c_str());
  setText(remoteStatus, !remoteStarted()    ? ""
                        : remoteConnected() ? "Connected"
                                            : "Pair \"Perch\" in your computer's Bluetooth");
  lv_obj_set_style_opa(remoteSlides, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_set_style_opa(remoteMediaPanel, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  // Control Center: restyle only when a toggle actually changed.
  const String cc =
      String(networkEnabled()) + online + cameraOff + useGemini + bluetoothOn + remoteConnected() + btPaused;
  if (cc != ccSignature) {
    ccSignature = cc;
    // On: amber disc, dark glyph. Off: dark disc, light glyph. The label always says which.
    auto toggle = [](lv_obj_t *t, bool on) {
      lv_obj_set_style_bg_color(t, on ? LENS : ICON_BG, 0);
      lv_obj_t *glyph = lv_obj_get_child(t, 0);
      if (glyph) lv_obj_set_style_text_color(glyph, on ? VOID_ : INK, 0);
    };
    toggle(ccWifi, networkEnabled());
    setText(ccWifiState, !networkEnabled() ? "Off" : online ? "On" : "No net");
    toggle(ccBt, bluetoothOn);
    setText(ccBtState, !bluetoothOn ? "Off" : btPaused ? "Paused" : remoteConnected() ? "Linked" : "On");
    toggle(ccCamera, !cameraOff);
    setText(lv_obj_get_child(ccCamera, 0), cameraOff ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    setText(ccCameraState, cameraOff ? "Off" : "On");
    toggle(ccModel, false);
    setText(lv_obj_get_child(ccModel, 0), useGemini ? "G" : "GPT");
    setText(ccModelState, useGemini ? "Gemini" : "GPT");
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
  uint32_t previousPhoto = 0;  // the photo saved just before this one (joins the chat as context)
};
// Burst capture, keeping the sharpest frame. Sharpness = variance of a Laplacian
// over the centre of a 1/8-scale decode, where text usually is. Below the floor even the
// best frame is soft, and the user is told to hold still.
constexpr int BURST = 3;
constexpr uint32_t BLUR_FLOOR = 35;
int lastLuma = 0;  // mean brightness of the last frame scored, 0..187
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
  int64_t sum = 0, sumSq = 0, lumaSum = 0;
  int n = 0;
  for (int y = h / 4; y < h * 3 / 4; ++y)
    for (int x = w / 4; x < w * 3 / 4; ++x) {
      lumaSum += gray(x, y);
      const int lap = 4 * gray(x, y) - gray(x - 1, y) - gray(x + 1, y) - gray(x, y - 1) - gray(x, y + 1);
      sum += lap;
      sumSq += (int64_t)lap * lap;
      ++n;
    }
  if (!n) return 0;
  lastLuma = lumaSum / n;
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
  bool blurry;
  uint8_t *small;
  size_t smallLen;
  uint16_t thumb[THUMB * THUMB];
};
uint8_t *saveScratch = nullptr;
size_t saveScratchCapacity = 0;
// After a shot: the screen copy, thumbnail and small JPEG are made here, off the camera task,
// so the live view resumes as soon as the frames are taken. Then the photo goes to flash.
void processShot(SaveJob *job) {
  CaptureResult r;
  r.jpeg = (uint8_t *)ps_malloc(job->len);
  if (r.jpeg) {
    memcpy(r.jpeg, job->jpeg, job->len);
    r.len = job->len;
  }
  r.blurry = job->blurry;
  // Jobs run in order, so the last photo saved is the one this shot follows.
  r.previousPhoto = savedPhotoId ? (uint32_t)savedPhotoId : latestPhotoId;
  const bool haveScreen = decodeStoredWith(job->jpeg, job->len, captureScreen, saveScratch, saveScratchCapacity);
  if (!haveScreen) memset(captureScreen, 0, W * H * 2);
  makeThumb(captureScreen, job->thumb);
  uint16_t *swapped = (uint16_t *)ps_malloc(W * H * 2);
  if (swapped) {
    for (int i = 0; i < W * H; ++i) swapped[i] = __builtin_bswap16(captureScreen[i]);
    fmt2jpg((uint8_t *)swapped, W * H * 2, W, H, PIXFORMAT_RGB565, 80, &job->small, &job->smallLen);
    free(swapped);
  }
  if (!r.jpeg) r.problem = "Not enough memory for the photo.";
  captureOut = r;
  captureReady = true;
  captureProcessing = false;
}
QueueHandle_t saveQueue = nullptr;
void saveTask(void *) {
  freeKbCache = storageFreeBytes() / 1024;
  for (;;) {
    SaveJob *job = nullptr;
    if (xQueueReceive(saveQueue, &job, pdMS_TO_TICKS(1000)) != pdTRUE || !job) {
      if (freeKbStale) {
        freeKbStale = false;
        freeKbCache = storageFreeBytes() / 1024;
      }
      continue;
    }
    const uint32_t t = millis();
    processShot(job);
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
  // The on-screen copy is decoded from the photo itself below: the fast preview frame is
  // darker and its colours differ from what is saved and sent.
  if (!cameraSetMode(CameraMode::Still)) {
    r.problem = "Camera is unavailable.";
  } else {
    // A freshly started sensor needs a couple of frames for exposure to settle.
    // Measured on the OV5640: brightness is steady from the second frame after the switch.
    {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) esp_camera_fb_return(f);
    }
    // Close-up pages need the lens moved; then drop the frame exposed while it was moving.
    if (cameraFocus(2000)) {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) esp_camera_fb_return(f);
    }
    vTaskDelay(1);
    uint32_t best = 0;
    for (int shot = 0; shot < BURST; ++shot) {
      camera_fb_t *frame = esp_camera_fb_get();
      if (!frame) continue;
      const bool valid = frame->format == PIXFORMAT_JPEG && frame->len >= 4 && frame->buf[0] == 0xff &&
                         frame->buf[1] == 0xd8 && frame->buf[frame->len - 2] == 0xff &&
                         frame->buf[frame->len - 1] == 0xd9;
      const uint32_t score = valid ? sharpness(frame->buf, frame->len, frame->width, frame->height) : 0;
      Serial.printf("BURST %d sharpness=%lu luma=%d\n", shot, (unsigned long)score, lastLuma);
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
      vTaskDelay(1);  // each step is long; let core 0's idle task run so the watchdog stays fed
    }
    cameraReport();
    if (!r.jpeg) r.problem = "No usable photo. Try again.";
    r.blurry = r.jpeg && best < BLUR_FLOOR;
  }
  const uint32_t t1 = millis();
  // Back to the live view (or off) right away: never leave the sensor streaming full-size
  // frames (heat, core 0), and the user sees the viewfinder again at once.
  cameraSetMode(previewWanted ? CameraMode::Preview : CameraMode::Off);
  SaveJob *job = r.jpeg ? (SaveJob *)ps_malloc(sizeof(SaveJob)) : nullptr;
  if (job) {
    job->jpeg = r.jpeg;  // the save task owns it now
    job->len = r.len;
    job->blurry = r.blurry;
    job->small = nullptr;
    job->smallLen = 0;
    r.jpeg = nullptr;
    captureProcessing = true;
    ++savesPending;
    if (xQueueSend(saveQueue, &job, 0) != pdTRUE) {
      --savesPending;
      captureProcessing = false;
      free(job->jpeg);
      free(job);
      job = nullptr;
      r.problem = "Could not keep the photo. Try again.";
    }
  }
  if (!job) {
    free(r.jpeg);
    r.jpeg = nullptr;
    if (r.problem.isEmpty()) r.problem = "No usable photo. Try again.";
    captureOut = r;
    captureReady = true;
  }
  Serial.printf("SHUTTER frames=%lums back-to-live=%lums\n", (unsigned long)(t1 - t0), (unsigned long)(millis() - t1));
}
bool requestCapture(String &problem) {
  if (cameraOff) {
    problem = "Camera is off. Turn it on in Control Center.";
    return false;
  }
  if (captureRequested || captureReady || captureProcessing) {
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
  // "+ Page" keeps the previous photo as context; any other photo starts a new question.
  // A new photo joins the conversation; the previous one becomes context. After "New chat"
  // the next photo starts clean.
  const uint32_t previous = captureOut.previousPhoto ? captureOut.previousPhoto : latestPhotoId;
  if (!chatFresh && previous && jpegBytes) {
    contextIds.push_back(previous);
    while ((int)contextIds.size() > AI_MAX_PAGES - 1) contextIds.erase(contextIds.begin());
  }
  chatFresh = false;
  appendingPage = false;
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
AiOptions aiOptions(bool gemini) {
  AiOptions o;
  o.gemini = gemini;
  o.key = settings.getString(gemini ? "gemini-key" : "gpt-key", "");
  o.model = gemini ? GEMINI_MODELS[constrain(geminiModel, 0, GEMINI_MODEL_COUNT - 1)].id
                   : GPT_MODELS[constrain(gptModel, 0, GPT_MODEL_COUNT - 1)].id;
  o.effort = AI_EFFORTS[constrain(gemini ? geminiEffort : gptEffort, 0, AI_EFFORT_COUNT - 1)];
  return o;
}
int pageCount() { return jpegBytes ? (int)contextIds.size() + 1 : 0; }
void startAsk(const String &question = "", const uint8_t *wav = nullptr, size_t wavLength = 0) {
  if (!jpegBytes) {
    notice("Take a photo first.", Screen::Ask);
    return;
  }
  if (wav && !useGemini) {
    toast("Voice needs Gemini. Asking about the photo");
    wav = nullptr;
    wavLength = 0;
  }
  if (!networkConnected()) {
    // Badge mode: keep the question and answer it when Wi-Fi returns.
    if (photoSavePending) {
      queueWhenSaved = true;
    } else if (latestPhotoId) {
      const uint32_t q = queueAdd(latestPhotoId, useGemini, clockKnown() ? (uint32_t)time(nullptr) : 0);
      if (q && wav) queueSaveAudio(q, wav, wavLength);
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
  // Earlier pages come from flash; the newest is the photo in memory.
  const uint8_t *jpegs[AI_MAX_PAGES];
  size_t lengths[AI_MAX_PAGES];
  uint8_t *loaded[AI_MAX_PAGES] = {nullptr};
  int pages = 0;
  for (uint32_t id : contextIds) {
    uint8_t *j = nullptr;
    size_t l = 0;
    if (pages < AI_MAX_PAGES - 1 && loadPhoto(id, j, l)) {
      loaded[pages] = j;
      jpegs[pages] = j;
      lengths[pages] = l;
      ++pages;
    }
  }
  jpegs[pages] = savedJpeg;
  lengths[pages] = jpegBytes;
  ++pages;
  // Earlier questions and answers of this chat (the last three, trimmed) travel as context.
  String history;
  const int from = max(0, (int)sessionAnswers.size() - 3);
  for (int i = from; i < (int)sessionAnswers.size(); ++i) {
    String body;
    AnswerInfo info;
    if (!loadAnswer(sessionAnswers[i], body, info)) continue;
    if (body.length() > 700) body = body.substring(0, 700) + "...";
    history += "Q: " + (sessionQuestions[i].length() ? sessionQuestions[i] : String("(about the photo)")) +
               "\nA: " + body + "\n";
  }
  pendingQuestion = question;
  AiOptions options = aiOptions(useGemini);
  pauseBluetooth();  // Bluetooth leaves too little memory for HTTPS; it comes back after
  const bool started = aiStart(options, jpegs, lengths, pages, question, history, wav, wavLength);
  options.key = "";
  for (uint8_t *l : loaded) free(l);
  if (!started) {
    notice("Could not start the request. Not enough memory.", Screen::Ask);
    return;
  }
  pending = Pending::Answer;
  pendingGemini = useGemini;
  pendingPhotoId = photoSavePending ? UINT32_MAX : latestPhotoId;
  const String who = useGemini ? "Gemini" : "GPT";
  lv_label_set_text(busyLabel, (pages > 1                  ? "Asking " + who + " about " + pages + " pages"
                                : question.length() || wav ? "Asking " + who + " your question"
                                                           : "Asking " + who)
                                   .c_str());
  lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
  show(Screen::Ask);
}
// Hold-to-talk released: write down what was said, show it, and let the user send it.
// Offline there is nothing to transcribe with; the recording is kept with the question.
void startListenBack(const uint8_t *wav, size_t length) {
  if (!networkConnected()) {
    startAsk("", wav, length);
    return;
  }
  // Preview off: Gemini takes the recording with the photo in one request; GPT cannot hear
  // audio, so its words are written down first and sent without stopping.
  if (!confirmWords && useGemini) {
    startAsk("", wav, length);
    return;
  }
  sendAfterTranscribe = !confirmWords;
  if (aiBusy()) {
    notice("The previous request is still finishing. Try again in a moment.", Screen::Ask);
    return;
  }
  AiOptions options = aiOptions(useGemini);
  pauseBluetooth();
  const bool started = aiTranscribe(options, wav, length);
  options.key = "";
  if (!started) {
    notice("Could not start. Not enough memory.", Screen::Ask);
    return;
  }
  pending = Pending::Transcribe;
  lv_label_set_text(busyLabel, "Writing down what you said");
  lv_obj_remove_flag(busy, LV_OBJ_FLAG_HIDDEN);
}
// Wait for the previous photo to reach flash: its id is what a new page links back to.
void waitForSave() {
  const unsigned long start = millis();
  while (photoSavePending && millis() - start < 10000) {
    lv_timer_handler();
    delay(10);
  }
}
void typeQuestion() {
  if (!jpegBytes) {
    toast("Take a photo first");
    return;
  }
  textMode = TextMode::Question;
  setText(passwordTitle, "Your question");
  lv_textarea_set_password_mode(passwordField, false);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Type, or hold Talk");
  hide(passwordEye, true);
  lv_obj_set_width(passwordField, W - 28);
  lv_textarea_set_text(passwordField, "");
  show(Screen::Password);
}
void showHeard(const String &words) {
  heardText = words;
  setText(heardLabel, words.c_str());
  lv_obj_remove_flag(heardSheet, LV_OBJ_FLAG_HIDDEN);
  show(Screen::Ask);
}
// "+": open the camera; the shot joins this chat and Ask comes back.
void addPage() {
  if (cameraOff) {
    toast("Camera is off. Turn it on in Control Center");
    return;
  }
  pageCamera = true;
  setText(pageBanner, jpegBytes ? "Add a photo to this chat" : "Photo for Ask");
  show(Screen::Camera);
}
void newPhoto() { addPage(); }
void newChat() {
  contextIds.clear();
  sessionAnswers.clear();
  sessionQuestions.clear();
  chatFresh = true;
  refreshDynamic();
  toast("New chat");
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
// Brightness follows the finger; it is saved when the finger lifts (flash wears on writes).
void setBrightness(int pct, bool save) {
  brightnessPct = constrain(pct, 5, 100);
  displayBrightness(brightnessPct * 255 / 100);
  if (brightSlider && lv_slider_get_value(brightSlider) != brightnessPct)
    lv_slider_set_value(brightSlider, brightnessPct, LV_ANIM_OFF);
  if (ccBright && lv_slider_get_value(ccBright) != brightnessPct)
    lv_slider_set_value(ccBright, brightnessPct, LV_ANIM_OFF);
  if (save) settings.putUChar("bright-pct", brightnessPct);
}
// A wide, thick slider: the whole track is the touch target and fills as you drag.
lv_obj_t *brightnessSlider(lv_obj_t *parent, int width, int height) {
  lv_obj_t *sl = lv_slider_create(parent);
  lv_obj_set_size(sl, width, height);
  lv_slider_set_range(sl, 5, 100);
  lv_obj_set_style_radius(sl, height / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, GRAPHITE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sl, height / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, INK, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
  lv_obj_set_ext_click_area(sl, 10);
  lv_obj_remove_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_remove_flag(sl, LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_add_event_cb(
      sl, [](lv_event_t *e) { setBrightness(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)), false); },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sl, [](lv_event_t *) { setBrightness(brightnessPct, true); }, LV_EVENT_RELEASED, nullptr);
  return sl;
}
void setBluetooth(bool on) {
  bluetoothOn = on;
  settings.putBool("bt-on", on);
  if (on && !btPaused) remoteBegin();
  if (!on) {
    remoteEnd();
    btPaused = false;
  }
  refreshDynamic();
}
void pauseBluetooth() {
  if (remoteStarted()) {
    remoteEnd();
    btPaused = true;
  }
}
void resumeBluetooth() {
  btPaused = false;
  if (bluetoothOn && !remoteStarted()) remoteBegin();
}
bool cameraOffSetting() { return cameraOff; }
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
  hide(photoPrev, photoIndex == 0);
  hide(photoNext, photoIndex >= photoCount - 1);
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
    lv_obj_t *one = text(r, answerHeadline(body).c_str(), F_BODY, INK);
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
        r, [](lv_event_t *e) { showAnswer((uint32_t)(uintptr_t)lv_event_get_user_data(e)); }, LV_EVENT_SHORT_CLICKED,
        (void *)(uintptr_t)answerIds[i]);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          confirm(
              "Delete this answer?", "Delete answer",
              [](uint32_t id) {
                deleteAnswer(id);
                freeKbStale = true;
                loadLatestAnswer();
                cardSignature = "";
                refreshDynamic();
                rebuildHistory();
                toast("Deleted");
              },
              (uint32_t)(uintptr_t)lv_event_get_user_data(e));
        },
        LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)answerIds[i]);
  }
}
void onEnter(Screen s) {
  // Bluetooth only runs inside the apps that use it: it costs ~100 KB of RAM HTTPS needs.
  if (s != Screen::Camera && pageCamera && !captureRequested && !captureReady) {
    pageCamera = false;
    appendingPage = false;
  }
  if (s == Screen::Model) {
    aiScreenSignature = "";
    lv_obj_scroll_to_y(lv_obj_get_child(layer(Screen::Model), 1), 0, LV_ANIM_OFF);
  }
  if (s == Screen::Settings) lv_obj_scroll_to_y(layer(Screen::Settings), 0, LV_ANIM_OFF);
  if (s == Screen::Photos) {
    if (zoomView) lv_obj_add_flag(zoomView, LV_OBJ_FLAG_HIDDEN);
    hide(photoDelete, false);
    hide(photosImage, photoCount == 0);
    hide(photosEmpty, photoCount != 0);
    showPhoto(0);
  } else if (s == Screen::Ask) {
    rebuildHistory();
  } else if (s == Screen::Remote) {
    if (!bluetoothOn) {
      setBluetooth(true);
      toast("Bluetooth on");
    }
  } else if (s == Screen::Wifi) {
    startScan();
  } else if (s == Screen::Gestures) {
    if (!bluetoothOn) {
      setBluetooth(true);
      toast("Bluetooth on");
    }
    gestureCalibrating = 12;  // ~1 s of frames to learn the empty scene
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
  // Status: bright when connected, grey when on but not connected or off.
  lv_obj_t *status = plain(s);
  lv_obj_set_size(status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status, 8, 0);
  lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -24, 30);
  faceBt = text(status, LV_SYMBOL_BLUETOOTH, F_BODY, MIST);
  faceWifi = text(status, LV_SYMBOL_WIFI, F_BODY, MIST);
  faceBattery = text(status, "", F_SMALL, MIST);
  lv_obj_add_flag(faceBattery, LV_OBJ_FLAG_HIDDEN);  // shown once the battery sense is wired
  faceOffline = text(s, "", F_SMALL, MIST);
  lv_obj_add_flag(faceOffline, LV_OBJ_FLAG_HIDDEN);
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
}

// App grid: every icon has its name under it. No hidden labels, no guessing.
void appIcon(lv_obj_t *parent, int x, int y, const char *name, lv_color_t bg, void (*open)(),
             void (*glyph)(lv_obj_t *)) {
  lv_obj_t *col = plain(parent);
  lv_obj_set_size(col, 76, 82);
  lv_obj_set_pos(col, x - 38, y - 29);
  lv_obj_t *b = circle(col, 58, bg, 0, bg, LV_OPA_COVER);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
  glyph(b);
  lv_obj_t *label = text(col, name, F_SMALL, INK);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, 0);
  // The whole column (icon and name) is the button.
  onClick(col, open);
  lv_obj_add_event_cb(
      col,
      [](lv_event_t *e) {
        lv_obj_set_state(lv_obj_get_child((lv_obj_t *)lv_event_get_target(e), 0), LV_STATE_PRESSED, true);
      },
      LV_EVENT_PRESSED, nullptr);
  auto clear = [](lv_event_t *e) {
    lv_obj_set_state(lv_obj_get_child((lv_obj_t *)lv_event_get_target(e), 0), LV_STATE_PRESSED, false);
  };
  lv_obj_add_event_cb(col, clear, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(col, clear, LV_EVENT_PRESS_LOST, nullptr);
  icons.push_back(col);
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
  // Two rows of three, the main app first.
  appIcon(s, 46, 66, "Ask", LENS, [] { show(Screen::Ask); }, glyphAsk);
  appIcon(s, 120, 66, "Camera", ICON_BG, [] { show(Screen::Camera); }, glyphCamera);
  appIcon(
      s, 194, 66, "Photos", ICON_BG, [] { show(Screen::Photos); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_IMAGE, INK); });
  appIcon(
      s, 46, 164, "Remote", ICON_BG, [] { show(Screen::Remote); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_KEYBOARD, INK); });
  appIcon(
      s, 120, 164, "Gestures", ICON_BG, [] { show(Screen::Gestures); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_EYE_OPEN, INK); });
  appIcon(
      s, 194, 164, "Settings", ICON_BG, [] { show(Screen::Settings); },
      [](lv_obj_t *b) { glyphSymbol(b, LV_SYMBOL_SETTINGS, INK); });
  iconName = text(s, "", F_SMALL, INK);  // kept for older code paths; unused
  lv_obj_add_flag(iconName, LV_OBJ_FLAG_HIDDEN);
}

void buildCamera() {
  lv_obj_t *s = scr[(int)Screen::Camera] = screenBase();
  viewfinder = lv_image_create(s);
  lv_image_set_src(viewfinder, &liveDsc);
  cameraOffLabel = text(s, "Camera is off.\nTurn it on in Control Center.", F_BODY, MIST);
  lv_obj_set_style_text_align(cameraOffLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(cameraOffLabel, LV_ALIGN_CENTER, 0, -20);
  pageBanner = text(s, "", F_SMALL, INK);
  lv_obj_set_style_bg_color(pageBanner, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(pageBanner, 220, 0);
  lv_obj_set_style_radius(pageBanner, 12, 0);
  lv_obj_set_style_pad_hor(pageBanner, 12, 0);
  lv_obj_set_style_pad_ver(pageBanner, 5, 0);
  lv_obj_align(pageBanner, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_add_flag(pageBanner, LV_OBJ_FLAG_HIDDEN);
  // Shutter: one ring. Press fills it, so the feedback lands on touch-down.
  shutter = circle(s, 60, INK, 4, INK, LV_OPA_TRANSP);
  lv_obj_set_style_outline_width(shutter, 2, 0);
  lv_obj_set_style_outline_color(shutter, VOID_, 0);
  lv_obj_set_style_outline_opa(shutter, 140, 0);
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
  lv_obj_set_style_outline_width(thumb, 1, 0);
  lv_obj_set_style_outline_color(thumb, VOID_, 0);
  lv_obj_set_style_outline_opa(thumb, 140, 0);
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
  scrollable(askScroll);
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
  scrimBottom(hero, 160);  // dark enough under the hint and the buttons on any photo
  askOffline = text(hero, "Offline", F_SMALL, INK);
  lv_obj_align(askOffline, LV_ALIGN_TOP_MID, 0, 26);
  // Top right, small so the photo stays visible: new chat, and + to add a photo to this chat.
  pageBar = plain(hero);
  lv_obj_set_size(pageBar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pageBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(pageBar, 10, 0);
  lv_obj_set_style_pad_all(pageBar, 4, 0);
  lv_obj_align(pageBar, LV_ALIGN_TOP_RIGHT, -10, 24);
  newChatBtn = circle(pageBar, 38, INK, 0, GRAPHITE, 210);
  lv_obj_set_style_opa(newChatBtn, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(newChatBtn, LV_SYMBOL_EDIT, F_BODY, INK));
  lv_obj_set_ext_click_area(newChatBtn, 5);
  onClick(newChatBtn, newChat);
  plusBtn = circle(pageBar, 38, INK, 0, GRAPHITE, 210);
  lv_obj_set_style_opa(plusBtn, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(plusBtn, LV_SYMBOL_PLUS, F_BODY, INK));
  lv_obj_set_ext_click_area(plusBtn, 5);
  onClick(plusBtn, addPage);
  // Photos in this chat, when more than one.
  plusBadge = circle(hero, 18, VOID_, 0, LENS, LV_OPA_COVER);
  lv_obj_center(text(plusBadge, "", F_SMALL, VOID_));
  lv_obj_align(plusBadge, LV_ALIGN_TOP_RIGHT, -8, 22);
  lv_obj_remove_flag(plusBadge, LV_OBJ_FLAG_CLICKABLE);
  historyList = plain(askScroll);
  lv_obj_set_size(historyList, W, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(historyList, LV_FLEX_FLOW_COLUMN);
  // Bottom: the main action, and a separate mic you hold while you speak.
  askPill = pressedFeedback(plain(hero));
  lv_obj_set_size(askPill, 106, 48);
  lv_obj_align(askPill, LV_ALIGN_BOTTOM_LEFT, 12, -ABOVE_HOME);
  lv_obj_set_style_radius(askPill, 24, 0);
  lv_obj_set_style_bg_color(askPill, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(askPill, 235, 0);
  lv_obj_t *ring = circle(askPill, 26, LENS, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_align(ring, LV_ALIGN_LEFT_MID, 11, 0);
  askButtonLabel = text(askPill, "", F_BODY, INK);
  lv_obj_align(askButtonLabel, LV_ALIGN_LEFT_MID, 46, 0);
  onClick(askPill, [] {
    if (micRecording()) return;
    if (jpegBytes) startAsk();
    else newPhoto();
  });
  typeBtn = circle(hero, 48, INK, 0, GRAPHITE, 235);
  lv_obj_align(typeBtn, LV_ALIGN_BOTTOM_RIGHT, -66, -ABOVE_HOME);
  lv_obj_set_style_opa(typeBtn, LV_OPA_60, LV_STATE_PRESSED);
  glyphSymbol(typeBtn, LV_SYMBOL_KEYBOARD, INK);
  lv_obj_set_style_text_font(lv_obj_get_child(typeBtn, 0), F_BODY, 0);
  lv_obj_set_ext_click_area(typeBtn, 4);
  onClick(typeBtn, typeQuestion);
  micBtn = circle(hero, 48, INK, 0, GRAPHITE, 235);
  lv_obj_align(micBtn, LV_ALIGN_BOTTOM_RIGHT, -12, -ABOVE_HOME);
  lv_obj_set_style_bg_color(micBtn, LENS, LV_STATE_PRESSED);
  {
    // Microphone glyph: capsule, cradle, stem.
    lv_obj_t *cap = plain(micBtn);
    lv_obj_set_size(cap, 12, 20);
    lv_obj_set_style_radius(cap, 6, 0);
    lv_obj_set_style_bg_color(cap, INK, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -4);
    lv_obj_t *cradle = plain(micBtn);
    lv_obj_set_size(cradle, 20, 14);
    lv_obj_set_style_radius(cradle, 10, 0);
    lv_obj_set_style_border_width(cradle, 2, 0);
    lv_obj_set_style_border_color(cradle, INK, 0);
    lv_obj_set_style_border_side(cradle, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_align(cradle, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *stem = plain(micBtn);
    lv_obj_set_size(stem, 2, 5);
    lv_obj_set_style_bg_color(stem, INK, 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 10);
  }
  lv_obj_add_flag(micBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(micBtn, 6);
  // Press starts listening at once; release writes down what was said.
  lv_obj_add_event_cb(
      micBtn,
      [](lv_event_t *) {
        if (!jpegBytes) {
          toast("Take a photo first");
          return;
        }
        if (!lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN) || micRecording()) return;
        if (!micStart()) {
          toast("Microphone unavailable");
          return;
        }
        lv_obj_remove_flag(listenOverlay, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_PRESSED, nullptr);
  auto release = [](lv_event_t *) {
    if (!micRecording()) return;
    micStop();
    lv_obj_add_flag(listenOverlay, LV_OBJ_FLAG_HIDDEN);
    size_t len = 0;
    const uint8_t *wav = micWav(len);
    if (micSeconds() < 0.6f) {
      toast("Hold the mic while you speak");
      return;
    }
    startListenBack(wav, len);
  };
  lv_obj_add_event_cb(micBtn, release, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(micBtn, release, LV_EVENT_PRESS_LOST, nullptr);

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

  // Listening: the amber ring follows your voice. Release the button to ask.
  listenOverlay = plain(s);
  lv_obj_set_size(listenOverlay, W, H);
  lv_obj_set_style_bg_color(listenOverlay, VOID_, 0);
  lv_obj_set_style_bg_opa(listenOverlay, 170, 0);
  lv_obj_add_flag(listenOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(listenOverlay, LV_OBJ_FLAG_CLICKABLE);
  listenRing = circle(listenOverlay, 56, LENS, 3, LENS, LV_OPA_20);
  lv_obj_align(listenRing, LV_ALIGN_CENTER, 0, -40);
  lv_obj_t *listenLabel = text(listenOverlay, "Listening", F_BODY, INK);
  lv_obj_align(listenLabel, LV_ALIGN_CENTER, 0, 20);
  listenTime = text(listenOverlay, "Let go when you are done", F_SMALL, MIST);
  lv_obj_align(listenTime, LV_ALIGN_CENTER, 0, 44);

  // What you said: shown before anything is asked, so a mishearing is caught first.
  heardSheet = plain(s);
  lv_obj_set_size(heardSheet, W, H);
  lv_obj_set_style_bg_color(heardSheet, VOID_, 0);
  lv_obj_set_style_bg_opa(heardSheet, 240, 0);
  lv_obj_add_flag(heardSheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(heardSheet, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *said = text(heardSheet, "YOU SAID", F_SMALL, MIST);
  lv_obj_set_pos(said, 22, 30);
  lv_obj_t *heardBox = plain(heardSheet);
  lv_obj_set_size(heardBox, W - 44, H - 52 - ABOVE_HOME - 58);
  lv_obj_set_pos(heardBox, 22, 52);
  scrollable(heardBox);
  heardLabel = text(heardBox, "", F_LARGE, INK);
  lv_obj_set_width(heardLabel, W - 44);
  lv_label_set_long_mode(heardLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(heardLabel, 4, 0);
  lv_obj_t *again = pill(heardSheet, "Again", 96);
  lv_obj_align(again, LV_ALIGN_BOTTOM_LEFT, 18, -ABOVE_HOME);
  onClick(again, [] {
    lv_obj_add_flag(heardSheet, LV_OBJ_FLAG_HIDDEN);
    toast("Hold the button and speak");
  });
  lv_obj_t *send = pill(heardSheet, "Ask", 110);
  lv_obj_set_style_bg_color(send, LENS, 0);
  lv_obj_set_style_text_color(lv_obj_get_child(send, 0), VOID_, 0);
  lv_obj_align(send, LV_ALIGN_BOTTOM_RIGHT, -18, -ABOVE_HOME);
  onClick(send, [] {
    lv_obj_add_flag(heardSheet, LV_OBJ_FLAG_HIDDEN);
    startAsk(heardText);
  });

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
  scrollable(answerScroll);
  lv_obj_set_flex_flow(answerScroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(answerScroll, 56, 0);
  lv_obj_set_style_pad_row(answerScroll, 10, 0);
  answerPhoto = lv_image_create(answerScroll);
  lv_image_set_src(answerPhoto, &galleryDsc);
  lv_obj_set_size(answerPhoto, W, 84);
  lv_image_set_inner_align(answerPhoto, LV_IMAGE_ALIGN_CENTER);  // middle band of the photo
  lv_obj_set_style_image_opa(answerPhoto, LV_OPA_80, 0);
  answerFlow = plain(answerScroll);
  lv_obj_set_size(answerFlow, W, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(answerFlow, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(answerFlow, 20, 0);
  lv_obj_set_style_pad_row(answerFlow, 12, 0);
  answerMeta = text(answerScroll, "", F_SMALL, MIST);
  lv_obj_set_width(answerMeta, W - 40);
  lv_obj_set_style_margin_left(answerMeta, 20, 0);
  lv_obj_set_style_margin_top(answerMeta, 8, 0);
}

void deleteShownPhoto() {
  if (!photoCount) return;
  confirm(
      "Delete this photo?", "Delete photo",
      [](uint32_t id) {
        if (!deletePhoto(id)) {
          toast("Waiting to be asked: kept");
          return;
        }
        const bool wasLatest = id == latestPhotoId;
        refreshPhotos();
        if (wasLatest) {
          free(savedJpeg);
          savedJpeg = nullptr;
          jpegBytes = jpegCapacity = 0;
          latestPhotoId = 0;
          contextIds.clear();
          restoreLatestPhoto();
        }
        cardSignature = "";
        freeKbStale = true;
        refreshDynamic();
        hide(photosImage, photoCount == 0);
        hide(photosEmpty, photoCount != 0);
        if (photoCount) showPhoto(min(photoIndex, photoCount - 1));
        toast("Deleted");
      },
      photoIds[photoIndex]);
}
lv_obj_t *roundButton(lv_obj_t *parent, const char *symbol, void (*fn)()) {
  lv_obj_t *b = circle(parent, 46, INK, 0, GRAPHITE, 230);
  lv_obj_set_style_opa(b, LV_OPA_60, LV_STATE_PRESSED);
  glyphSymbol(b, symbol, INK);
  onClick(b, fn);
  lv_obj_set_ext_click_area(b, 4);
  return b;
}
void zoomOut() {
  lv_obj_add_flag(zoomView, LV_OBJ_FLAG_HIDDEN);
  hide(photoDelete, false);
  showPhoto(photoIndex);
}
void zoomTask(void *) {
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  int w = 0, h = 0;
  bool ok = loadPhoto(zoomPhotoId, jpeg, len) && jpegSize(jpeg, len, w, h);
  if (ok) {
    const size_t needed = (size_t)(w / 2) * (h / 2) * 2;
    static size_t capacity = 0;
    if (needed > capacity) {
      free(zoomPixels);
      zoomPixels = (uint16_t *)ps_malloc(needed);
      capacity = zoomPixels ? needed : 0;
    }
    const uint32_t t = millis();
    ok = zoomPixels && jpg2rgb565(jpeg, len, (uint8_t *)zoomPixels, JPG_SCALE_2X);
    Serial.printf("ZOOM decode %lums\n", (unsigned long)(millis() - t));
    zoomW = w / 2;
    zoomH = h / 2;
  }
  free(jpeg);
  zoomJob = ok ? 2 : 3;
  vTaskDelete(nullptr);
}
// Double-tap at (x, y): that spot grows under the finger at once; the sharp full photo
// replaces the enlarged screen copy when it has been decoded.
void zoomIn(int x, int y) {
  if (!photoCount || zoomJob == 1) return;
  zoomTapX = x;
  zoomTapY = y;
  zoomPhotoId = photoIds[photoIndex];
  lv_image_set_src(zoomBlur, &galleryDsc);
  lv_obj_set_size(zoomBlur, W, H);
  lv_obj_set_pos(zoomBlur, 0, 0);
  lv_image_set_pivot(zoomBlur, x, y);
  lv_image_set_scale(zoomBlur, 256 * 27 / 10);
  lv_obj_remove_flag(zoomBlur, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(zoomImage, LV_OBJ_FLAG_HIDDEN);
  setText(lv_obj_get_child(zoomExit, 0), "Sharpening");
  lv_obj_remove_flag(zoomView, LV_OBJ_FLAG_HIDDEN);
  hide(photoPrev, true);
  hide(photoNext, true);
  hide(photoDelete, true);
  zoomJob = 1;
  if (xTaskCreatePinnedToCore(zoomTask, "zoom", 8192, nullptr, 1, nullptr, 0) != pdPASS) zoomJob = 3;
}
// Called from deviceTick: swap in the sharp photo when it is ready.
void zoomTick() {
  if (zoomJob < 2) return;
  const int job = zoomJob;
  zoomJob = 0;
  if (lv_obj_has_flag(zoomView, LV_OBJ_FLAG_HIDDEN)) return;  // closed meanwhile
  if (job == 3) {
    setText(lv_obj_get_child(zoomExit, 0), "1x");
    toast("Could not open the full photo");
    return;
  }
  setupImage(zoomDsc, zoomPixels, zoomW, zoomH);
  lv_image_cache_drop(&zoomDsc);
  lv_image_set_src(zoomImage, &zoomDsc);
  lv_obj_set_size(zoomImage, zoomW, zoomH);
  // The screen copy is the photo scaled to the screen height with the width centre-cropped;
  // keep the tapped point where it is on screen.
  const int cropW = zoomH * W / H, left = (zoomW - cropW) / 2;
  const int px = left + zoomTapX * cropW / W, py = zoomTapY * zoomH / H;
  lv_obj_set_pos(zoomImage, -constrain(px - zoomTapX, 0, max(0, zoomW - W)),
                 -constrain(py - zoomTapY, 0, max(0, zoomH - H)));
  lv_obj_remove_flag(zoomImage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(zoomBlur, LV_OBJ_FLAG_HIDDEN);
  setText(lv_obj_get_child(zoomExit, 0), "1x");
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
  lv_obj_add_event_cb(
      photosImage,
      [](lv_event_t *) {
        const unsigned long now = millis();
        if (now - lastPhotoTap < 380) {
          lastPhotoTap = 0;
          lv_point_t at;
          lv_indev_get_point(lv_indev_active(), &at);
          zoomIn(at.x, at.y);
          return;
        }
        lastPhotoTap = now;
      },
      LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(photosImage, [](lv_event_t *) { deleteShownPhoto(); }, LV_EVENT_LONG_PRESSED, nullptr);
  photoCounter = text(s, "", F_SMALL, INK);
  lv_obj_align(photoCounter, LV_ALIGN_TOP_MID, 0, 18);
  // Visible controls (swiping works too): newer, delete, older.
  photoPrev = roundButton(s, LV_SYMBOL_LEFT, [] { showPhoto(photoIndex - 1); });
  lv_obj_align(photoPrev, LV_ALIGN_BOTTOM_LEFT, 18, -ABOVE_HOME);
  photoNext = roundButton(s, LV_SYMBOL_RIGHT, [] { showPhoto(photoIndex + 1); });
  lv_obj_align(photoNext, LV_ALIGN_BOTTOM_RIGHT, -18, -ABOVE_HOME);
  photoDelete = pill(s, "Delete", 100);
  lv_obj_align(photoDelete, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  onClick(photoDelete, deleteShownPhoto);
  zoomView = plain(s);
  lv_obj_set_size(zoomView, W, H);
  lv_obj_set_style_bg_color(zoomView, VOID_, 0);
  lv_obj_set_style_bg_opa(zoomView, LV_OPA_COVER, 0);
  lv_obj_set_style_clip_corner(zoomView, true, 0);
  lv_obj_add_flag(zoomView, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(zoomView, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(zoomView, LV_OBJ_FLAG_HIDDEN);
  zoomBlur = lv_image_create(zoomView);
  lv_obj_remove_flag(zoomBlur, LV_OBJ_FLAG_CLICKABLE);
  zoomImage = lv_image_create(zoomView);
  lv_obj_remove_flag(zoomImage, LV_OBJ_FLAG_CLICKABLE);
  // Drag moves the window over the photo.
  lv_obj_add_event_cb(
      zoomView,
      [](lv_event_t *) {
        if (lv_obj_has_flag(zoomImage, LV_OBJ_FLAG_HIDDEN)) return;  // pan once sharp
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        const int x = constrain((int)lv_obj_get_x(zoomImage) + v.x, -max(0, zoomW - W), 0);
        const int y = constrain((int)lv_obj_get_y(zoomImage) + v.y, -max(0, zoomH - H), 0);
        lv_obj_set_pos(zoomImage, x, y);
      },
      LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(
      zoomView,
      [](lv_event_t *) {
        const unsigned long now = millis();
        if (now - lastPhotoTap < 380) {
          lastPhotoTap = 0;
          zoomOut();
          return;
        }
        lastPhotoTap = now;
      },
      LV_EVENT_SHORT_CLICKED, nullptr);
  zoomExit = chip(zoomView, "1x");
  lv_obj_align(zoomExit, LV_ALIGN_TOP_RIGHT, -14, 24);
  onClick(zoomExit, zoomOut);
  photosEmpty = plain(s);
  lv_obj_set_size(photosEmpty, W, H);
  lv_obj_align(text(photosEmpty, "No photos yet.", F_BODY, MIST), LV_ALIGN_CENTER, 0, -30);
  lv_obj_t *open = pill(photosEmpty, "Open Camera", 150);
  lv_obj_align(open, LV_ALIGN_CENTER, 0, 20);
  onClick(open, [] { show(Screen::Camera); });
}

std::vector<std::pair<lv_obj_t *, lv_obj_t *>> screenTitles;  // (screen layer, title)
lv_obj_t *title(lv_obj_t *s, const char *value) {
  lv_obj_t *t = text(s, value, F_LARGE, INK);
  lv_obj_set_pos(t, 22, 18);
  screenTitles.push_back({s, t});
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
          textMode = TextMode::Password;
          lv_textarea_set_password_mode(passwordField, true);
          lv_textarea_set_placeholder_text(passwordField, "Password");
          hide(passwordEye, false);
          lv_obj_set_width(passwordField, 166);
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
  title(s, "Wi-Fi");
  wifiStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_pos(wifiStatus, 22, 58);
  wifiList = plain(s);
  lv_obj_set_size(wifiList, W, H - 80);
  lv_obj_set_pos(wifiList, 0, 80);
  scrollable(wifiList);
  lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(wifiList, 56, 0);
}
// Keyboard: three layouts, keys as large as 240 px allows, kept above the home strip.
const char *KB_LOWER[] = {"q",  "w",   "e",    "r", "t",          "y", "u", "i", "o", "p",
                          "\n", "a",   "s",    "d", "f",          "g", "h", "j", "k", "l",
                          "\n", "ABC", "z",    "x", "c",          "v", "b", "n", "m", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  "Talk", " ", LV_SYMBOL_OK, ""};
const char *KB_UPPER[] = {"Q",  "W",   "E",    "R", "T",          "Y", "U", "I", "O", "P",
                          "\n", "A",   "S",    "D", "F",          "G", "H", "J", "K", "L",
                          "\n", "abc", "Z",    "X", "C",          "V", "B", "N", "M", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  "Talk", " ", LV_SYMBOL_OK, ""};
const char *KB_SPECIAL[] = {
    "1",  "2",   "3",    "4", "5",          "6",  "7", "8", "9", "0", "\n", "-", "_", "/", ":", ";",
    "(",  ")",   "@",    "&", "\"",         "\n", "#", "%", "*", "+", "=",  ".", ",", "?", "!", LV_SYMBOL_BACKSPACE,
    "\n", "abc", "Talk", " ", LV_SYMBOL_OK, ""};
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
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(6),
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
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(6),
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
  lv_obj_t *eye = passwordEye = circle(s, 36, ICON_BG, 0, ICON_BG, LV_OPA_COVER);
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
        const String typed = lv_textarea_get_text(passwordField);
        if (textMode == TextMode::Question) {
          String q = typed;
          q.trim();
          if (q.isEmpty()) {
            toast("Type a question, or hold Talk");
            return;
          }
          lv_textarea_set_text(passwordField, "");
          goBack();
          startAsk(q);
          return;
        }
        if (typed.length() < 8) {
          toast("Wi-Fi passwords have at least 8 characters");
          return;
        }
        joinNetwork(joiningSsid, typed);
        lv_textarea_set_text(passwordField, "");
        goBack();
      },
      LV_EVENT_READY, nullptr);
  // Talk: hold the key and speak; the words are typed into the field.
  lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
        const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
        const char *label = id == LV_BUTTONMATRIX_BUTTON_NONE ? nullptr : lv_buttonmatrix_get_button_text(kb, id);
        if (label && strcmp(label, "Talk") == 0) return;  // handled on press / release
        lv_keyboard_def_event_cb(e);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
        const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
        const char *label = id == LV_BUTTONMATRIX_BUTTON_NONE ? nullptr : lv_buttonmatrix_get_button_text(kb, id);
        if (!label || strcmp(label, "Talk") != 0 || micRecording() || aiBusy()) return;
        if (!networkConnected()) {
          toast("Talk needs Wi-Fi");
          return;
        }
        if (micStart()) {
          dictating = true;
          toast("Listening. Let go when done");
        }
      },
      LV_EVENT_PRESSED, nullptr);
  auto talkUp = [](lv_event_t *) {
    if (!dictating || !micRecording()) return;
    micStop();
    size_t len = 0;
    const uint8_t *wav = micWav(len);
    if (micSeconds() < 0.6f) {
      dictating = false;
      toast("Hold Talk while you speak");
      return;
    }
    AiOptions options = aiOptions(useGemini);
    pauseBluetooth();
    const bool started = aiTranscribe(options, wav, len);
    options.key = "";
    if (!started) {
      dictating = false;
      toast("Could not start");
      return;
    }
    pending = Pending::Transcribe;
    toast("Writing it down");
  };
  lv_obj_add_event_cb(keyboard, talkUp, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(keyboard, talkUp, LV_EVENT_PRESS_LOST, nullptr);
}

// ---------- Gestures: wave to change slides on a paired computer ----------
lv_obj_t *gestureView, *gestureRing, *gestureLabel, *gestureSub, *gestureArrow, *gestureFlipLabel;
bool gestureFlip = false;
void buildGestures() {
  lv_obj_t *s = scr[(int)Screen::Gestures] = screenBase();
  // The camera view stays dim: it is there to show the area Perch watches, not to read.
  gestureView = lv_image_create(s);
  lv_image_set_src(gestureView, &liveDsc);
  lv_obj_set_style_image_opa(gestureView, LV_OPA_40, 0);
  gestureRing = circle(s, 80, MIST, 3, VOID_, LV_OPA_TRANSP);
  lv_obj_align(gestureRing, LV_ALIGN_CENTER, 0, -46);
  gestureArrow = text(gestureRing, "", F_LARGE, INK);
  lv_obj_center(gestureArrow);
  gestureLabel = text(s, "", F_BODY, INK);
  lv_obj_align(gestureLabel, LV_ALIGN_CENTER, 0, 10);
  gestureSub = text(s, "", F_SMALL, MIST);
  lv_obj_set_width(gestureSub, W - 24);
  lv_obj_set_style_text_align(gestureSub, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(gestureSub, LV_LABEL_LONG_WRAP);
  lv_obj_align(gestureSub, LV_ALIGN_CENTER, 0, 32);
  lv_obj_t *flip = pill(s, "Reverse", 110);
  lv_obj_set_height(flip, 34);
  gestureFlipLabel = lv_obj_get_child(flip, 0);
  lv_obj_align(flip, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME + 10);
  onClick(flip, [] {
    gestureFlip = !gestureFlip;
    settings.putBool("gesture-flip", gestureFlip);
    toast(gestureFlip ? "Directions reversed" : "Directions normal");
  });
}
void renderGestures() {
  static int shownState = -1;
  static uint32_t seenSeq = 0, previewUntil = 0;
  const uint32_t now = millis();
  if (gestureSeq != seenSeq) {
    seenSeq = gestureSeq;
    const int dir = gestureSwipe * (gestureFlip ? -1 : 1);
    // Ghost preview first, then the key: you always see what is about to happen.
    setText(gestureArrow, dir > 0 ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
    setText(gestureLabel, dir > 0 ? "Next slide" : "Previous slide");
    lv_obj_set_style_border_color(gestureRing, LENS, 0);
    lv_obj_set_style_bg_color(gestureRing, LENS, 0);
    lv_obj_set_style_bg_opa(gestureRing, LV_OPA_30, 0);
    lv_refr_now(display);
    if (remoteConnected()) remoteKey(dir > 0 ? RemoteKey::Right : RemoteKey::Left);
    previewUntil = now + 600;
    shownState = -1;
    return;
  }
  if (now < previewUntil) return;
  const int state = gestureCalibrating > 0 ? 3 : gestureState;
  if (state == shownState) return;
  shownState = state;
  lv_obj_set_style_bg_opa(gestureRing, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(gestureRing, state == 2 ? LENS : MIST, 0);
  setText(gestureArrow, state == 2 ? LV_SYMBOL_LEFT "  " LV_SYMBOL_RIGHT : "");
  setText(gestureLabel, state == 3   ? "Learning the scene"
                        : state == 2 ? "Ready"
                        : state == 1 ? "Hold still"
                                     : "Show your hand");
  setText(gestureSub, state == 3           ? "Keep your hand out of view"
                      : state == 2         ? "Swipe left or right"
                      : !remoteConnected() ? "Pair a computer in Remote first"
                                           : "Hold your hand still to start");
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
  remoteButton(remoteMediaPanel, 120, 104, 72, LV_SYMBOL_PLAY, [] { remoteMedia(RemoteMedia::PlayPause); });
  remoteButton(remoteMediaPanel, 46, 104, 52, LV_SYMBOL_PREV, [] { remoteMedia(RemoteMedia::Previous); });
  remoteButton(remoteMediaPanel, 194, 104, 52, LV_SYMBOL_NEXT, [] { remoteMedia(RemoteMedia::Next); });
  remoteButton(remoteMediaPanel, 92, 164, 40, LV_SYMBOL_MINUS, [] { remoteMedia(RemoteMedia::VolumeDown); });
  remoteButton(remoteMediaPanel, 148, 164, 40, LV_SYMBOL_PLUS, [] { remoteMedia(RemoteMedia::VolumeUp); });
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
  lv_obj_t *model = row(s, 110, "AI", &modelValue);
  onClick(model, [] { show(Screen::Model); });
  lv_obj_t *bright = row(s, 162, "Brightness", nullptr);
  brightSlider = brightnessSlider(bright, 104, 22);
  lv_obj_align(brightSlider, LV_ALIGN_RIGHT_MID, -20, 0);
  lv_obj_remove_flag(bright, LV_OBJ_FLAG_CLICKABLE);  // the row itself does nothing; the slider does
  lv_obj_t *storage = row(s, 214, "Storage", &storageValue, 64);
  lv_obj_align(lv_obj_get_child(storage, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_align(storageValue, LV_ALIGN_TOP_RIGHT, -20, 12);
  storageSub = text(storage, "", F_SMALL, MIST);
  lv_obj_set_pos(storageSub, 22, 36);
  lv_obj_t *forget = row(s, 278, "Forget last hour", nullptr, 64);
  lv_obj_align(lv_obj_get_child(forget, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_t *forgetSub = text(forget, "Hold to delete answers and photos", F_SMALL, MIST);
  lv_obj_set_pos(forgetSub, 22, 36);
  lv_obj_add_flag(forget, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      forget, [](lv_event_t *) { toast("Hold to forget the last hour"); }, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(
      forget,
      [](lv_event_t *) {
        if (!clockKnown()) {
          toast("Clock not set yet: nothing to match");
          return;
        }
        const int removed = storageForgetSince(time(nullptr) - 3600);
        freeKbStale = true;
        loadLatestAnswer();
        refreshPhotos();
        refreshQueue();
        // If the newest photo was removed, the on-screen photo goes too.
        if (latestPhotoId && (!photoCount || photoIds[0] != latestPhotoId)) {
          free(savedJpeg);
          savedJpeg = nullptr;
          jpegBytes = jpegCapacity = 0;
          latestPhotoId = 0;
          restoreLatestPhoto();
        }
        cardSignature = "";
        refreshDynamic();
        toast(removed ? "Forgot the last hour" : "Nothing from the last hour");
      },
      LV_EVENT_LONG_PRESSED, nullptr);
  lv_obj_t *about = row(s, 342, "About", nullptr, 64);
  lv_obj_align(lv_obj_get_child(about, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_t *version = text(about, "Perch 0.5, built " __DATE__, F_SMALL, MIST);
  lv_obj_set_pos(version, 22, 36);
  lv_obj_t *spacer = plain(s);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, 462);
}

// AI settings: which provider answers, and for each provider its model and effort.
void saveAi() {
  settings.putUChar("gm-model", geminiModel);
  settings.putUChar("gm-effort", geminiEffort);
  settings.putUChar("gp-model", gptModel);
  settings.putUChar("gp-effort", gptEffort);
  aiScreenSignature = "";
  refreshDynamic();
}
lv_obj_t *sectionLabel(lv_obj_t *list, const char *label) {
  lv_obj_t *l = text(list, label, F_SMALL, MIST);
  lv_obj_set_style_margin_left(l, 22, 0);
  lv_obj_set_style_margin_top(l, 16, 0);
  lv_obj_set_style_margin_bottom(l, 4, 0);
  return l;
}
lv_obj_t *choiceRow(lv_obj_t *list, const char *label, const char *note, lv_obj_t **check, lv_obj_t **noteOut,
                    lv_event_cb_t cb, intptr_t value) {
  lv_obj_t *r = pressedFeedback(plain(list));
  lv_obj_set_size(r, W, 56);
  lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_set_style_border_color(r, LINE, 0);
  lv_obj_t *l = text(r, label, F_BODY, INK);
  lv_obj_set_pos(l, 22, 8);
  lv_obj_t *n = text(r, note, F_SMALL, MIST);
  lv_obj_set_pos(n, 22, 32);
  if (noteOut) *noteOut = n;
  *check = text(r, "", F_BODY, LENS);
  lv_obj_align(*check, LV_ALIGN_RIGHT_MID, -20, 0);
  lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, (void *)value);
  return r;
}
void segmented(lv_obj_t *list, lv_obj_t **segs, lv_event_cb_t cb, int provider) {
  lv_obj_t *bar = plain(list);
  lv_obj_set_size(bar, W - 40, 40);
  lv_obj_set_style_margin_left(bar, 20, 0);
  lv_obj_set_style_radius(bar, 20, 0);
  lv_obj_set_style_bg_color(bar, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(bar, 3, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  static const char *names[AI_EFFORT_COUNT] = {"Low", "Medium", "High"};
  for (int i = 0; i < AI_EFFORT_COUNT; ++i) {
    lv_obj_t *b = plain(bar);
    lv_obj_set_height(b, 34);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_style_radius(b, 17, 0);
    lv_obj_center(text(b, names[i], F_SMALL, INK));
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)(provider * 10 + i));
    segs[i] = b;
  }
}
void refreshAiScreen() {
  const String sig = String(useGemini) + geminiModel + geminiEffort + gptModel + gptEffort + keyCache + confirmWords;
  if (sig == aiScreenSignature) return;
  aiScreenSignature = sig;
  for (int i = 0; i < 2; ++i) {
    setText(aiUseCheck[i], (i == 0) == useGemini ? LV_SYMBOL_OK : "");
    setText(aiUseSub[i], (keyCache >> i) & 1 ? "Key saved" : "No key: add it in PC setup");
  }
  setText(wordsCheck, confirmWords ? LV_SYMBOL_OK : "");
  setText(wordsSub, confirmWords ? "On: you check the words, then Ask" : "Off: your voice goes straight in");
  for (int i = 0; i < GEMINI_MODEL_COUNT; ++i) setText(gemModelCheck[i], i == geminiModel ? LV_SYMBOL_OK : "");
  for (int i = 0; i < GPT_MODEL_COUNT; ++i) setText(gptModelCheck[i], i == gptModel ? LV_SYMBOL_OK : "");
  for (int p = 0; p < 2; ++p)
    for (int i = 0; i < AI_EFFORT_COUNT; ++i) {
      const bool on = i == (p == 0 ? geminiEffort : gptEffort);
      lv_obj_set_style_bg_color(effortSeg[p][i], INK, 0);
      lv_obj_set_style_bg_opa(effortSeg[p][i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lv_obj_get_child(effortSeg[p][i], 0), on ? VOID_ : INK, 0);
    }
}
void buildModel() {
  lv_obj_t *s = scr[(int)Screen::Model] = screenBase();
  title(s, "AI");
  lv_obj_t *list = plain(s);
  lv_obj_set_size(list, W, H - 52);
  lv_obj_set_pos(list, 0, 52);
  scrollable(list);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(list, 70, 0);
  sectionLabel(list, "ANSWERS FROM");
  const char *providers[2] = {"Gemini", "GPT"};
  for (int i = 0; i < 2; ++i)
    choiceRow(
        list, providers[i], "", &aiUseCheck[i], &aiUseSub[i],
        [](lv_event_t *e) {
          setModel(lv_event_get_user_data(e) == nullptr);
          aiScreenSignature = "";
          refreshDynamic();
        },
        i);
  sectionLabel(list, "GEMINI MODEL");
  for (int i = 0; i < GEMINI_MODEL_COUNT; ++i)
    choiceRow(
        list, GEMINI_MODELS[i].label, GEMINI_MODELS[i].note, &gemModelCheck[i], nullptr,
        [](lv_event_t *e) {
          geminiModel = (intptr_t)lv_event_get_user_data(e);
          saveAi();
        },
        i);
  sectionLabel(list, "GEMINI THINKING");
  segmented(
      list, effortSeg[0],
      [](lv_event_t *e) {
        const int v = (intptr_t)lv_event_get_user_data(e);
        geminiEffort = v % 10;
        saveAi();
      },
      0);
  sectionLabel(list, "GPT MODEL");
  for (int i = 0; i < GPT_MODEL_COUNT; ++i)
    choiceRow(
        list, GPT_MODELS[i].label, GPT_MODELS[i].note, &gptModelCheck[i], nullptr,
        [](lv_event_t *e) {
          gptModel = (intptr_t)lv_event_get_user_data(e);
          saveAi();
        },
        i);
  sectionLabel(list, "GPT REASONING");
  segmented(
      list, effortSeg[1],
      [](lv_event_t *e) {
        const int v = (intptr_t)lv_event_get_user_data(e);
        gptEffort = v % 10;
        saveAi();
      },
      1);
  sectionLabel(list, "VOICE");
  choiceRow(
      list, "Show words first", "Check what you said before asking", &wordsCheck, &wordsSub,
      [](lv_event_t *) {
        confirmWords = !confirmWords;
        settings.putBool("words-first", confirmWords);
        aiScreenSignature = "";
        refreshDynamic();
      },
      0);
  lv_obj_t *note = text(list,
                        "Showing your words first uses one extra short request (free on Gemini's daily limit). "
                        "Off: Gemini hears your voice with the photo in one request. "
                        "More thinking is slower, and costs more with GPT. When a free Gemini limit runs out, "
                        "the next Gemini model answers instead.",
                        F_SMALL, MIST);
  lv_obj_set_width(note, W - 44);
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_margin_left(note, 22, 0);
  lv_obj_set_style_margin_top(note, 14, 0);
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

// One control: disc (amber = on), its name, and its state in plain words underneath.
lv_obj_t *ccToggle(lv_obj_t *parent, const char *label, lv_obj_t **labelOut, lv_obj_t **stateOut = nullptr) {
  lv_obj_t *col = plain(parent);
  lv_obj_set_size(col, 58, 92);
  lv_obj_t *t = circle(col, 50, INK, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_opa(t, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_ext_click_area(t, 4);
  lv_obj_t *l = text(col, label, F_SMALL, INK);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 56);
  if (labelOut) *labelOut = l;
  lv_obj_t *st = text(col, "", F_SMALL, MIST);
  lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 74);
  if (stateOut) *stateOut = st;
  return t;
}
void buildControl() {
  cc = plain(root);
  lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);  // tap outside the controls closes it
  lv_obj_set_size(cc, W, H);
  lv_obj_set_style_bg_color(cc, VOID_, 0);
  lv_obj_set_style_bg_opa(cc, 248, 0);
  lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(cc, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_y(cc, -H);
  lv_obj_t *grid = plain(cc);
  lv_obj_set_size(grid, 236, 96);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  ccWifi = ccToggle(grid, "Wi-Fi", &ccWifiLabel, &ccWifiState);
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
  ccBt = ccToggle(grid, "BT", &ccBtLabel, &ccBtState);
  glyphSymbol(ccBt, LV_SYMBOL_BLUETOOTH, INK);
  onClick(ccBt, [] {
    setBluetooth(!bluetoothOn);
    toast(bluetoothOn ? "Bluetooth on" : "Bluetooth off");
  });
  ccCamera = ccToggle(grid, "Camera", &ccCameraLabel, &ccCameraState);
  glyphSymbol(ccCamera, LV_SYMBOL_EYE_OPEN, INK);
  onClick(ccCamera, [] {
    setCameraOff(!cameraOff);
    toast(cameraOff ? "Camera off" : "Camera on");
  });
  ccModel = ccToggle(grid, "AI", &ccModelLabel, &ccModelState);
  lv_obj_t *modelGlyph = text(ccModel, "G", F_BODY, INK);
  lv_obj_center(modelGlyph);
  onClick(ccModel, [] {
    setModel(!useGemini);
    toast(useGemini ? "Answers from Gemini" : "Answers from GPT");
  });
  // Brightness: drag along the bar.
  lv_obj_t *brightTitle = text(cc, "Brightness", F_SMALL, MIST);
  lv_obj_align(brightTitle, LV_ALIGN_TOP_LEFT, 22, 140);
  ccBright = brightnessSlider(cc, W - 44, 44);
  lv_obj_align(ccBright, LV_ALIGN_TOP_MID, 0, 160);
  // Sun mark: grey reads on both the filled and the empty part of the bar.
  lv_obj_t *sun = circle(ccBright, 10, MIST, 2, VOID_, LV_OPA_TRANSP);
  lv_obj_align(sun, LV_ALIGN_LEFT_MID, 22, 0);
  for (int i = 0; i < 8; ++i) {
    lv_obj_t *ray = circle(ccBright, 3, MIST, 0, MIST, LV_OPA_COVER);
    lv_obj_align(ray, LV_ALIGN_LEFT_MID, 22 + 3 + (int)(10 * cosf(i * PI / 4)), (int)(10 * sinf(i * PI / 4)));
  }
  lv_obj_t *hint = text(cc, "Swipe up from the bottom to close", F_SMALL, MIST);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);
  // Tap outside the controls closes it; so does swiping up from the bottom line.
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
  // An edge touch is a candidate until the finger shows its direction. Until then the app
  // sees nothing; if it turns out to be a scroll or a tap, the app gets it after all.
  static bool committed = false;
  static bool passThrough = false;  // candidate rejected: this touch belongs to the app
  static int tapPending = 0;        // 2 = report a press at (tapX, tapY), 1 = then a release
  static int tapX = 0, tapY = 0;
  if (tapPending && !pressed) {
    data->point.x = tapX;
    data->point.y = tapY;
    data->state = tapPending-- == 2 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return;
  }
  if (pressed) {
    const unsigned long now = millis();
    if (!fingerDown) {
      fingerDown = true;
      downX = x;
      downY = y;
      speed = speedX = 0;
      committed = passThrough = false;
      edge = (injecting && !injectEdges)                                 ? Edge::None
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
    const int dx = x - downX, dy = y - downY;
    if (edge != Edge::None && !committed) {
      // Commit when the movement matches the edge; give the touch to the app when it does not.
      if (edge == Edge::Home && -dy > 8 && -dy > abs(dx)) committed = true;
      else if (edge == Edge::Control && dy > 8 && dy > abs(dx)) committed = true;
      else if (edge == Edge::Back && dx > 12 && dx > 2 * abs(dy)) committed = true;
      else if ((edge == Edge::Back && abs(dy) > 10 && abs(dy) > dx) ||
               ((edge == Edge::Home || edge == Edge::Control) && abs(dx) > 12 && abs(dx) > abs(dy))) {
        edge = Edge::None;
        passThrough = true;
      }
    }
    if (committed) {
      if (edge == Edge::Home) {
        if (ccOpen) dragControlClosed(-dy);
        else dragHome(-dy);
      }
      if (edge == Edge::Control) dragControl(dy);
      if (edge == Edge::Back) dragBack(dx);
    }
  } else if (fingerDown) {
    fingerDown = false;
    if (committed) {
      if (edge == Edge::Home) {
        if (ccOpen) releaseControlClosed(max(0, downY - lastY), speed);
        else releaseHome(max(0, downY - lastY), speed);
      }
      if (edge == Edge::Control) releaseControl(max(0, lastY - downY), speed);
      if (edge == Edge::Back) releaseBack(max(0, lastX - downX), speedX);
    } else if (edge != Edge::None) {
      // A tap that started on an edge: deliver it to the app (back arrow, edge buttons).
      tapPending = 2;
      tapX = downX;
      tapY = downY;
    }
    edge = Edge::None;
    committed = false;
  }
  if (edge != Edge::None) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  (void)passThrough;
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
    case Screen::Gestures: return "gestures";
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
void injectTap(int x, int y, int holdMs = 80) {
  injectX = x;
  injectY = y;
  injecting = true;
  const unsigned long start = millis();
  do {
    lv_timer_handler();
    delay(20);
  } while (millis() - start < (unsigned long)holdMs);
  injecting = false;
  for (int i = 0; i < 4; ++i) {
    lv_timer_handler();
    delay(20);
  }
}
// A finger drag from (x1,y1) to (x2,y2) over `ms`, then release. Edges apply.
void injectDrag(int x1, int y1, int x2, int y2, int ms) {
  injecting = true;
  injectEdges = true;
  const int steps = max(4, ms / 20);
  for (int i = 0; i <= steps; ++i) {
    injectX = x1 + (x2 - x1) * i / steps;
    injectY = y1 + (y2 - y1) * i / steps;
    lv_timer_handler();
    delay(20);
  }
  injecting = false;
  for (int i = 0; i < 6; ++i) {
    lv_timer_handler();
    delay(20);
  }
  injectEdges = false;
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
  if (!ensurePreviewMode(8000)) {
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
  bluetoothOn = settings.getBool("bt-on", false);
  confirmWords = settings.getBool("words-first", true);
  batteryBegin();
  geminiModel = constrain(settings.getUChar("gm-model", 0), 0, GEMINI_MODEL_COUNT - 1);
  geminiEffort = constrain(settings.getUChar("gm-effort", 0), 0, AI_EFFORT_COUNT - 1);
  gptModel = constrain(settings.getUChar("gp-model", 0), 0, GPT_MODEL_COUNT - 1);
  gptEffort = constrain(settings.getUChar("gp-effort", 0), 0, AI_EFFORT_COUNT - 1);
  cameraOff = settings.getBool("camera-off", false);
  brightnessPct = constrain(settings.getUChar("bright-pct", 100), 5, 100);
  gestureFlip = settings.getBool("gesture-flip", false);
  if (!storageBegin()) Serial.println("STORAGE_ERROR Flash filesystem unavailable");
  framebuffer = (uint16_t *)ps_malloc(W * H * 2);
  for (auto &b : liveBuf) b = (uint16_t *)ps_calloc(W * H, 2);
  livePixels = liveBuf[0];
  camLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(previewTask, "viewfinder", 4096, nullptr, 2, nullptr, 0);
  saveQueue = xQueueCreate(3, sizeof(SaveJob *));
  xTaskCreatePinnedToCore(saveTask, "photo-save", 8192, nullptr, 1, nullptr, 0);
  photoPixels = (uint16_t *)ps_calloc(W * H, 2);
  captureScreen = (uint16_t *)ps_calloc(W * H, 2);
  galleryPixels = (uint16_t *)ps_calloc(W * H, 2);
  historyThumbs = (uint16_t *)ps_calloc(ANSWER_KEEP * THUMB * THUMB, 2);
  // Screen link speed is stored so it can be tuned for the wiring without reflashing ('Y').
  // 40 MHz works on both boards over jumper wires; 80 halves tearing if the wiring allows.
  aiSetFlashBusy([] { return savesPending > 0; });
  spiMhz = constrain(settings.getUChar("lcd-mhz", 40), 5, 80);
  const bool lcd = displayBegin(spiMhz * 1000000UL, drawDone);
  for (auto &b : drawBuffers)
    b = (uint16_t *)heap_caps_malloc(W * DISPLAY_CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  for (int y = 0; y < H; ++y) {
    // No shading baked into the viewfinder: it showed as a seam across the picture. The
    // controls carry their own dark outline instead, so they read on any scene.
    scrimRow[y] = 255;
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
  buildGestures();
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
  if (!settings.getBool("guide-seen3", false)) {
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
    const char *lines[][2] = {{LV_SYMBOL_UP "  Up from the bottom", "apps, or back home"},
                              {LV_SYMBOL_DOWN "  Down from the top", "Control Center"},
                              {LV_SYMBOL_RIGHT "  Right from the left", "back one step"}};
    for (auto &line : lines) {
      lv_obj_t *a = text(col, line[0], F_BODY, INK);
      lv_obj_t *b = text(col, line[1], F_SMALL, MIST);
      lv_obj_set_style_margin_top(b, -10, 0);
      (void)a;
    }
    lv_obj_t *ok = pill(guide, "Got it", 130);
    lv_obj_add_flag(ok, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
    lv_obj_add_event_cb(
        ok,
        [](lv_event_t *e) {
          settings.putBool("guide-seen3", true);
          lv_obj_delete(lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e)));
        },
        LV_EVENT_CLICKED, nullptr);
  }
  // Recording indicator: amber dot whenever a photo or your voice is leaving the device.
  recordingDot = circle(lv_layer_top(), 8, LENS, 0, LENS, LV_OPA_COVER);
  lv_obj_align(recordingDot, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_add_flag(recordingDot, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(recordingDot, LV_OBJ_FLAG_CLICKABLE);
  confirmLayer = plain(lv_layer_top());
  lv_obj_set_size(confirmLayer, W, H);
  lv_obj_set_style_bg_color(confirmLayer, VOID_, 0);
  lv_obj_set_style_bg_opa(confirmLayer, 170, 0);
  lv_obj_add_flag(confirmLayer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      confirmLayer,
      [](lv_event_t *e) {
        if (lv_event_get_target(e) == confirmLayer) lv_obj_add_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sheet = plain(confirmLayer);
  lv_obj_set_size(sheet, 212, 140);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME + 18);
  lv_obj_set_style_radius(sheet, 24, 0);
  lv_obj_set_style_bg_color(sheet, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
  confirmTitle = text(sheet, "", F_SMALL, MIST);
  lv_obj_align(confirmTitle, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_t *act = pressedFeedback(plain(sheet));
  lv_obj_set_size(act, 212, 46);
  lv_obj_set_pos(act, 0, 40);
  lv_obj_set_style_border_side(act, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(act, 1, 0);
  lv_obj_set_style_border_color(act, LINE, 0);
  confirmAction = text(act, "", F_BODY, lv_color_hex(0xff6b5e));
  lv_obj_center(confirmAction);
  onClick(act, [] {
    lv_obj_add_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN);
    if (confirmFn) confirmFn(confirmArg);
  });
  lv_obj_t *cancel = pressedFeedback(plain(sheet));
  lv_obj_set_size(cancel, 212, 46);
  lv_obj_set_pos(cancel, 0, 88);
  lv_obj_set_style_border_side(cancel, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(cancel, 1, 0);
  lv_obj_set_style_border_color(cancel, LINE, 0);
  lv_obj_center(text(cancel, "Cancel", F_BODY, INK));
  onClick(cancel, [] { lv_obj_add_flag(confirmLayer, LV_OBJ_FLAG_HIDDEN); });

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
  lv_obj_remove_flag(homeBar, LV_OBJ_FLAG_CLICKABLE);
  // The matching mark at the top: pull down from here for Control Center.
  topBar = plain(lv_layer_top());
  lv_obj_set_size(topBar, 28, 4);
  lv_obj_set_style_radius(topBar, 2, 0);
  lv_obj_set_style_bg_color(topBar, INK, 0);
  lv_obj_set_style_bg_opa(topBar, 80, 0);
  lv_obj_align(topBar, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_remove_flag(topBar, LV_OBJ_FLAG_CLICKABLE);
  setBrightness(brightnessPct, false);

  lv_obj_remove_flag(layer(Screen::Face), LV_OBJ_FLAG_HIDDEN);
  enter(Screen::Face);
  const uint32_t start = micros();
  lv_refr_now(display);
  Serial.printf("DISPLAY first frame %lu us\n", (unsigned long)(micros() - start));
  if (lcd) displayBrightness(brightnessPct * 255 / 100);
}

void handleDeviceButton(char command) {
  if (!framebuffer) return;
  if (command == 'T') {
    String xy = Serial.readStringUntil('\n');
    // "x,y" taps; "x,y,ms" holds for ms (long-press actions).
    const int comma = xy.indexOf(',');
    if (comma < 1) {
      Serial.println("SCREEN_ERROR Invalid touch");
      return;
    }
    const int comma2 = xy.indexOf(',', comma + 1);
    const int x = xy.substring(0, comma).toInt();
    const int y = xy.substring(comma + 1, comma2 > 0 ? comma2 : xy.length()).toInt();
    const int hold = comma2 > 0 ? constrain((int)xy.substring(comma2 + 1).toInt(), 0, 20000) : 80;
    if (x >= 0 && x < W && y >= TOP_ZONE && y < HOME_ZONE) injectTap(x, y, hold);
    sendFrame();
    return;
  }
  if (command == '^') {
    // Developer: "x,y" double tap (photo zoom checks), then a frame.
    const String xy = Serial.readStringUntil('\n');
    const int comma = xy.indexOf(',');
    const int x = constrain((int)xy.substring(0, comma).toInt(), 0, W - 1);
    const int y = constrain((int)xy.substring(comma + 1).toInt(), TOP_ZONE, HOME_ZONE - 1);
    injectTap(x, y, 60);
    injectTap(x, y, 60);
    sendFrame();
    return;
  }
  if (command == '!') {
    // Developer: "x1,y1,x2,y2,ms" drag, edges included (for gesture checks), then a frame.
    int v[5] = {0, 0, 0, 0, 300};
    String arg = Serial.readStringUntil('\n');
    for (int i = 0; i < 5 && arg.length(); ++i) {
      const int c = arg.indexOf(',');
      v[i] = arg.substring(0, c < 0 ? arg.length() : c).toInt();
      arg = c < 0 ? "" : arg.substring(c + 1);
    }
    injectDrag(constrain(v[0], 0, W - 1), constrain(v[1], 0, H - 1), constrain(v[2], 0, W - 1),
               constrain(v[3], 0, H - 1), constrain(v[4], 40, 3000));
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
  if (command == 'm') {
    // Developer: write one sensor register, "m3036=10" (hex), then report preview FPS.
    const String arg = Serial.readStringUntil('\n');
    const int eq = arg.indexOf('=');
    if (eq < 1) return;
    const int reg = strtol(arg.substring(0, eq).c_str(), nullptr, 16);
    const int val = strtol(arg.substring(eq + 1).c_str(), nullptr, 16);
    xSemaphoreTake(camLock, portMAX_DELAY);
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) Serial.printf("SETREG %04X=%02X -> %d\n", reg, val, sensor->set_reg(sensor, reg, 0xFF, val));
    xSemaphoreGive(camLock);
    return;
  }
  if (command == '%') {
    // Developer: build a request without sending it. "%g2" Gemini 2 pages, "%o1" GPT,
    // "%ot" / "%gt" speech-to-text. Uses the saved model and effort; never the key.
    const String arg = Serial.readStringUntil('\n');
    AiOptions o = aiOptions(arg.length() == 0 || arg[0] != 'o');
    o.key = "";
    const bool transcribe = arg.indexOf('t') >= 0;
    aiDryRun(o, transcribe ? 0 : max(1, (int)arg.substring(1).toInt()),
             transcribe ? "" : "What is the answer to question 2?", transcribe);
    return;
  }
  if (command == 'l') {
    // Developer: "l<KB>" uploads that much to Gemini without a key (rejected, never billed).
    const int kb = Serial.readStringUntil('\n').toInt();
    if (kb > 0 && kb <= 2048) aiUploadTest((size_t)kb * 1024);
    return;
  }
  if (command == '7') {
    // Developer: "7dn=4" style photo tuning (see cameraTune).
    const String arg = Serial.readStringUntil('\n');
    const int eq = arg.indexOf('=');
    const bool ok = eq > 0 && cameraTune(arg.substring(0, eq).c_str(), arg.substring(eq + 1).toInt());
    Serial.printf("TUNE %s %s\n", arg.c_str(), ok ? "ok" : "unknown");
    return;
  }
  if (command == '6') {
    // Developer: "6<ae>,<frames>" photo brightness target and longest exposure, e.g. "61,2".
    const String arg = Serial.readStringUntil('\n');
    const int comma = arg.indexOf(',');
    if (comma > 0) cameraSetStillExposure(arg.substring(0, comma).toInt(), arg.substring(comma + 1).toInt());
    Serial.printf("STILL_EXPOSURE %s\n", arg.c_str());
    return;
  }
  if (command == 'y') {
    // Developer: gain ceiling for photos, "y2" = 8x (gainceiling_t: 0 = 2x ... 6 = 128x).
    const int ceiling = Serial.readStringUntil('\n').toInt();
    if (ceiling >= 0 && ceiling <= 6) cameraSetStillGain(ceiling);
    Serial.printf("STILL_GAIN %d\n", ceiling);
    return;
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
    case 'C': launch(Screen::Camera); break;
    case 'A': launch(Screen::Ask); break;
    case 'p': launch(Screen::Photos); break;
    case 'H':
      launch(Screen::Ask);
      if (answerCount) lv_obj_scroll_to_y(askScroll, H - ASK_PEEK - 30, LV_ANIM_OFF);
      break;
    case 'R': launch(Screen::Remote); break;
    case 'W': launch(Screen::Wifi); break;
    case 'I': {  // developer: internet check (no AI). 204 = real internet; anything else = sign-in page
      WiFiClientSecure tls;
      tls.setInsecure();  // only a reachability probe; nothing secret is sent
      HTTPClient http;
      const uint32_t t = millis();
      int code = -1;
      if (http.begin(tls, "https://www.google.com/generate_204")) {
        code = http.GET();
        http.end();
      }
      Serial.printf("NET generate_204 -> %d in %lu ms\n", code, (unsigned long)(millis() - t));
      for (const char *host : {"generativelanguage.googleapis.com", "api.openai.com", "www.google.com"}) {
        IPAddress ip;
        const int ok = WiFi.hostByName(host, ip);
        Serial.printf("NET dns %s -> %d %s\n", host, ok, ip.toString().c_str());
      }
      Serial.printf("NET dns server %s\n", WiFi.dnsIP().toString().c_str());
      break;
    }
    case 'k': {  // developer: OV5640 timing registers (PLL, frame size, exposure limits)
      xSemaphoreTake(camLock, portMAX_DELAY);
      sensor_t *sensor = esp_camera_sensor_get();
      if (sensor) {
        static const uint16_t regs[] = {0x3034, 0x3035, 0x3036, 0x3037, 0x3108, 0x3824, 0x460C, 0x4837, 0x380C,
                                        0x380D, 0x380E, 0x380F, 0x3808, 0x3809, 0x380A, 0x380B, 0x3814, 0x3815,
                                        0x3A00, 0x3A02, 0x3A03, 0x3A14, 0x3A15, 0x3500, 0x3501, 0x3502, 0x350A,
                                        0x350B, 0x3503, 0x5001, 0x3031, 0x3029, 0x3023};
        for (uint16_t r : regs) Serial.printf("REG %04X=%02X\n", r, sensor->get_reg(sensor, r, 0xFF));
      }
      xSemaphoreGive(camLock);
      break;
    }
    case '8': aiKeyCheck(settings.getString("gemini-key", "")); break;  // developer: free key check
    case 'J': {  // developer: raw touch samples, to check the touch wiring
      int pressedCount = 0;
      for (int i = 0; i < 30; ++i) {
        int x = -1, y = -1;
        const bool down = touchRead(x, y);
        pressedCount += down;
        Serial.printf("TOUCH %d %d %d int=%d\n", down, x, y, digitalRead(PIN_TP_INT));
        delay(50);
      }
      Serial.printf("TOUCH pressed %d/30\n", pressedCount);
      break;
    }
    case 'S': {  // developer: a clearly labelled sample answer, for layout checks (delete it after)
      static uint16_t *th = (uint16_t *)ps_malloc(THUMB * THUMB * 2);
      const bool haveThumb = latestPhotoId && th && loadPhotoThumb(latestPhotoId, th);
      saveAnswer(
          "Q1: c. O(n^2)\n"
          "1. **.get(i)** on a LinkedList walks i nodes: O(n)\n"
          "2. The loop calls it n times: n * O(n) = O(n^2)\n\n"
          "**Q2:** b. 2n\n"
          "1. The loop runs while i < data.size() * 2\n"
          "2. So foo is called 2n times\n\n"
          "Question 3: x = 1 or x = -4\n"
          "(x + 4)(x - 1) = 0\n\n"
          "Sample written on the device for a layout check, not by a model.",
          true, latestPhotoId, clockKnown() ? (uint32_t)time(nullptr) : 0, haveThumb ? th : nullptr);
      loadLatestAnswer();
      cardSignature = "";
      refreshDynamic();
      launch(Screen::Ask);
      break;
    }
    case 'E': launch(Screen::Gestures); break;
    case 'L': {
      if (!micStart()) {
        Serial.println("MIC_ERROR start failed");
        return;
      }
      const unsigned long t = millis();
      while (millis() - t < 3000) {
        lv_timer_handler();
        delay(5);
      }
      micStop();
      size_t len = 0;
      const uint8_t *wav = micWav(len);
      int64_t sumSq = 0;
      int peak = 0;
      const int16_t *pcm = (const int16_t *)(wav + 44);
      const size_t n = len > 44 ? (len - 44) / 2 : 0;
      for (size_t i = 0; i < n; ++i) {
        sumSq += (int32_t)pcm[i] * pcm[i];
        peak = max(peak, abs((int)pcm[i]));
      }
      Serial.printf("MIC samples=%u rms=%d peak=%d seconds=%.1f\n", (unsigned)n, n ? (int)sqrt((double)sumSq / n) : 0,
                    peak, micSeconds());
      Serial.printf("WAV_BEGIN %u\n", (unsigned)len);
      sendAcknowledged(wav, len);
      Serial.println("\nWAV_END");
      return;
    }
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
    case 'i': launch(Screen::Settings); break;
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
  if ((ai == AiState::Done || ai == AiState::Failed) && pending == Pending::Transcribe) {
    pending = Pending::None;
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    if (dictating) {
      dictating = false;
      if (ai == AiState::Failed) toast("Could not hear that. Try again");
      else {
        String current = lv_textarea_get_text(passwordField);
        if (current.length() && !current.endsWith(" ")) lv_textarea_add_text(passwordField, " ");
        lv_textarea_add_text(passwordField, result.c_str());
      }
    } else if (ai == AiState::Failed) notice(result, Screen::Ask);
    else if (sendAfterTranscribe) {
      sendAfterTranscribe = false;
      startAsk(result);
    } else showHeard(result);
  } else if (ai == AiState::Cancelled && pending == Pending::Transcribe) {
    pending = Pending::None;
  } else if (ai == AiState::Done && queueInFlight) {
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
    pending = Pending::None;
    if (pendingPhotoId == UINT32_MAX) pendingPhotoId = latestPhotoId;
    static uint16_t *th = (uint16_t *)ps_malloc(THUMB * THUMB * 2);  // PSRAM: internal RAM is for Wi-Fi/TLS
    const bool haveThumb = pendingPhotoId && th && loadPhotoThumb(pendingPhotoId, th);
    const uint32_t when = clockKnown() ? (uint32_t)time(nullptr) : 0;
    const uint32_t id = saveAnswer(result, pendingGemini, pendingPhotoId, when, haveThumb ? th : nullptr);
    if (!id) Serial.println("STORAGE_ERROR Answer not saved");
    if (id) {
      sessionAnswers.push_back(id);
      sessionQuestions.push_back(pendingQuestion);
      while (sessionAnswers.size() > 6) {
        sessionAnswers.erase(sessionAnswers.begin());
        sessionQuestions.erase(sessionQuestions.begin());
      }
      chatFresh = false;
    }
    loadLatestAnswer();
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    refreshDynamic();
    if (waiting && current == Screen::Ask && id) showAnswer(id);
  } else if (ai == AiState::Failed) {
    pending = Pending::None;
    const bool waiting = !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(busy, LV_OBJ_FLAG_HIDDEN);
    if (waiting && current == Screen::Ask) notice(result, Screen::Ask);
  }
  if (btPaused && !aiBusy()) resumeBluetooth();
  zoomTick();
  static bool btStarted = false;  // start a few seconds after boot, once the UI is up
  if (!btStarted && now > 4000) {
    btStarted = true;
    if (bluetoothOn && !aiBusy()) remoteBegin();
  }
  if (captureReady && !captureRequested) {
    String problem;
    const bool forPage = pageCamera;
    if (!acceptCapture(problem)) notice(problem, current == Screen::Camera ? Screen::Camera : Screen::Face);
    else if (forPage) {
      pageCamera = false;
      show(Screen::Ask);
      if (pageCount() > 1) toast((String("Page ") + pageCount() + " added. Tap Ask").c_str());
    }
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
      uint8_t *wav = nullptr;
      size_t wavLen = 0;
      queueLoadAudio(queued[i].id, wav, wavLen);
      pauseBluetooth();  // Bluetooth leaves too little memory for HTTPS
      AiOptions options = aiOptions(queued[i].gemini);
      const uint8_t *pages[1] = {jpeg};
      const size_t lengths[1] = {len};
      const bool started = aiStart(options, pages, lengths, 1, "", "", wav, wavLen);
      options.key = "";
      free(wav);
      if (started) {
        queueInFlight = queued[i].id;
        historyDirty = true;
        if (current == Screen::Ask) rebuildHistory();
      }
      free(jpeg);
      break;
    }
  }
  if (micRecording() && listenRing) {
    // Ring grows with loudness; time left shows near the limit.
    const int size = 56 + micLevel() * 50 / 100;
    if (lv_obj_get_width(listenRing) != size) lv_obj_set_size(listenRing, size, size);
    const int left = MIC_MAX_SECONDS - (int)micSeconds();
    setText(listenTime, left <= 5 ? (String(left) + " s left").c_str() : "Let go when you are done");
    if (left <= 0) {
      micStop();
      lv_obj_add_flag(listenOverlay, LV_OBJ_FLAG_HIDDEN);
      size_t len = 0;
      const uint8_t *wav = micWav(len);
      startListenBack(wav, len);
    }
  }
  if (recordingDot) hide(recordingDot, !(aiBusy() || micRecording()));
  static unsigned long lastRefresh = 0;
  if (now - lastRefresh > 1000 && !dragging && !fingerDown) {
    lastRefresh = now;
    refreshDynamic();
  }
  // Live viewfinder: the camera task converts frames; the UI shows the newest one.
  static unsigned long lastFpsReport = 0;
  static uint32_t shown = 0, convertedAtReport = 0;
  const bool wantsCamera = current == Screen::Camera || current == Screen::Gestures;
  gestureActive = current == Screen::Gestures && !ccOpen;
  if (current == Screen::Gestures) renderGestures();
  if (wantsCamera && !cameraOff && !ccOpen && now - lastSerialMs > 3000) {
    previewWanted = true;
    if (takeLiveFrame()) {
      lv_image_cache_drop(&liveDsc);
      lv_obj_invalidate(current == Screen::Gestures ? gestureView : viewfinder);
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
    if (!wantsCamera || cameraOff) stopPreview();
  }
  lv_timer_handler();
}
