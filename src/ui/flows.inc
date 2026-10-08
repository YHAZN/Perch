// Part of device_ui.cpp (included there, one translation unit). Actions: capture pipeline and saving, Ask flows, chat,
// settings changes.
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
// Burst-and-pick-sharpest. Sharpness = variance of a Laplacian
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
    int64_t lastStamp = 0;
    for (int shot = 0; shot < BURST; ++shot) {
      camera_fb_t *frame = esp_camera_fb_get();
      if (!frame) continue;
      const int64_t stamp = (int64_t)frame->timestamp.tv_sec * 1000000 + frame->timestamp.tv_usec;
      if (lastStamp) {
        Serial.printf("BURST frame time %lu us\n", (unsigned long)(stamp - lastStamp));
        cameraNoteFrameTime((uint32_t)(stamp - lastStamp));
      }
      lastStamp = stamp;
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
// Wait for the previous photo to reach flash: its id is what a new page links back to.
void waitForSave() {
  const unsigned long start = millis();
  while (photoSavePending && millis() - start < 10000) {
    lv_timer_handler();
    delay(10);
  }
}
void refreshQueue() {
  queuedCount = queueList(queued, 16);
  historyDirty = true;
}
void cancelAsk() {
  aiCancel();
  refreshDynamic();
}
void setModel(bool gemini) {
  useGemini = gemini;
  settings.putBool("gemini", useGemini);
  refreshDynamic();
}
void wakeScreen() {
  if (getCpuFrequencyMhz() != 240) setCpuFrequencyMhz(240);
  if (screenAsleep) Serial.println("SCREEN wake");
  screenAsleep = false;
  screenDim = false;
  lastActivity = millis();
  displayBrightness(brightnessPct * 255 / 100);
}
void sleepScreen() {
  screenAsleep = true;
  displayBrightness(0);
  // Nothing to draw: run the processor at 80 MHz (the lowest Wi-Fi allows) until woken.
  setCpuFrequencyMhz(80);
  Serial.println("SCREEN sleep");
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
void rebuildHistory() {}
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
    if (textMode == TextMode::Question) draft = lv_textarea_get_text(passwordField);  // back from the keyboard
    refreshChat();
  } else if (s == Screen::Chats) {
    refreshChats();
  } else if (s == Screen::Picker) {
    refreshPicker();
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
