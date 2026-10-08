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
#include <ArduinoJson.h>
#include "ai_client.h"
#include "audio.h"
#include "battery.h"
#include "updater.h"
#include "version.h"
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
// Home swipes start on the middle of the bottom edge, under the home bar; the bottom corners
// belong to the app.
constexpr int HOME_HALF_WIDTH = 64;
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
  Chats,
  Picker,
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
bool streaming = false;  // the answer screen is showing an answer as it arrives
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
// Screen sleep: dim 10 s before, then backlight off and camera off. The first touch only wakes.
const uint16_t SLEEP_CHOICES[] = {30, 60, 120, 0};  // seconds; 0 = never
uint8_t sleepChoice = 1;
unsigned long lastActivity = 0;
bool screenAsleep = false, screenDim = false, swallowTouch = false;
lv_obj_t *sleepValue = nullptr;
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
lv_obj_t *modelValue, *wifiValue, *brightSlider, *storageValue, *storageSub, *softwareSub;
lv_obj_t *modelCheck[2], *modelSub[2];
lv_obj_t *noticeText;
// Ask: pages and "what you said"
lv_obj_t *typeBtn, *passwordEye;
lv_obj_t *pageBar, *plusBtn, *plusBadge, *newChatBtn, *pageBanner, *micBtn;
lv_obj_t *heardSheet, *heardLabel;
bool pageCamera = false;  // the camera was opened from Ask ("New photo" / "+ Page"); return after the shot
void newPhoto();
void refreshChat();
void refreshChats();
void refreshPicker();
lv_obj_t *title(lv_obj_t *s, const char *value);
String draft;              // the chat's message box (src/ui/chat.inc)
uint32_t nextTimerMs = 1;  // when LVGL next needs to run (main loop rest)
void chatPhotoTaken();
void chatPhotoSaved(uint32_t id);
void pauseBluetooth();
void setBluetooth(bool on);
void wakeScreen();
void showHeard(const String &words);
void addPage();
int pageCount();
void refreshAiScreen();
// AI settings screen
lv_obj_t *aiUseCheck[2], *aiUseSub[2], *wordsCheck, *wordsSub, *aiUsageLabel;
lv_obj_t *gemModelCheck[8], *gptModelCheck[8];
lv_obj_t *effortSeg[2][AI_EFFORT_COUNT];
String aiScreenSignature;
lv_obj_t *homeBar, *topBar, *holdStill;
lv_obj_t *ccBt, *ccBtLabel, *ccWifiState, *ccBtState, *ccCameraState, *ccModelState;
lv_obj_t *cc, *ccWifi, *ccWifiLabel, *ccBright, *ccModel, *ccModelLabel, *ccCamera, *ccCameraLabel;
bool ccOpen = false;

// Touch
bool injecting = false;
bool injectEdges = false;  // injected drags exercise the system edges like a finger
int injectX = 0, injectY = 0;
bool fingerDown = false;
// Corner: a touch that starts in a bottom corner. A tap goes to the control above; a slide is
// ignored, so a finger dragged up from the corner cannot trip the buttons it passes over.
enum class Edge { None, Home, Control, Back, Corner } edge = Edge::None;
int downX = 0, downY = 0, lastX = 0, lastY = 0;

#include "ui/helpers.inc"
#include "ui/images.inc"
#include "ui/camera.inc"
#include "ui/navigation.inc"
#include "ui/answers.inc"
#include "ui/flows.inc"
#include "ui/chat.inc"
#include "ui/screens_main.inc"
#include "ui/screens_wifi.inc"
#include "ui/screens_tools.inc"
#include "ui/screens_settings.inc"
#include "ui/glue.inc"
}  // namespace

#include "ui/init.inc"
#include "ui/commands.inc"
#include "ui/tick.inc"
