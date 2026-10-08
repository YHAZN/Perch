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
void wakeScreen() {
  if (screenAsleep) Serial.println("SCREEN wake");
  screenAsleep = false;
  screenDim = false;
  lastActivity = millis();
  displayBrightness(brightnessPct * 255 / 100);
}
void sleepScreen() {
  screenAsleep = true;
  displayBrightness(0);
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
