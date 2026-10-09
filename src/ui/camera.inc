// Part of device_ui.cpp (included there, one translation unit). Camera task: viewfinder frames, gesture analysis, mode
// changes.
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
void convertPreview(const camera_fb_t *f, uint16_t *out);
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
// Camera frame -> 240x284 screen: scale to height and centre-crop the width.
// Accepts JPEG (decoded here) or raw RGB565.
uint8_t *previewScratch = nullptr;  // owned by the viewfinder task
void convertPreview(const camera_fb_t *f, uint16_t *out) {
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
    uint16_t *dst = out + y * W;
    for (int x = 0; x < W; ++x) {
      const uint8_t *px = row + (left + x * cropW / W) * 2;
      dst[x] = bigEndian ? (px[0] << 8) | px[1] : px[0] | (px[1] << 8);
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
