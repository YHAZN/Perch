// Part of device_ui.cpp (included there, one translation unit). Start-up: settings, display, LVGL, tasks, screens.
void deviceNoteSerial() { lastSerialMs = millis(); }

void initDeviceUi() {
  settings.begin("tiny-ai", false);
  useGemini = settings.getBool("gemini", true);
  bluetoothOn = settings.getBool("bt-on", false);
  confirmWords = settings.getBool("words-first", true);
  sleepChoice = settings.getUChar("sleep", 1) % (sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]));
  dailyChoice = settings.getUChar("daily", 0) % (sizeof(DAILY_CHOICES) / sizeof(DAILY_CHOICES[0]));
  aiSetDailyLimit(DAILY_CHOICES[dailyChoice]);
  // Privacy light: the board's orange LED (active low) is on while a photo or sound is being
  // recorded or sent.
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  batteryBegin();
  updaterBegin();
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
  const uint32_t bootT0 = millis();
  loadLatestAnswer();
  const uint32_t bootT1 = millis();
  restoreLatestPhoto();
  const uint32_t bootT2 = millis();
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
  const uint32_t bootT3 = millis();
  buildFace();
  buildApps();
  buildCamera();
  buildAsk();
  buildChats();
  buildPicker();
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

  Serial.printf("BOOT answers %lu ms, photo %lu ms, queue+lvgl %lu ms, screens %lu ms\n",
                (unsigned long)(bootT1 - bootT0), (unsigned long)(bootT2 - bootT1), (unsigned long)(bootT3 - bootT2),
                (unsigned long)(millis() - bootT3));
  lv_obj_remove_flag(layer(Screen::Face), LV_OBJ_FLAG_HIDDEN);
  enter(Screen::Face);
  const uint32_t start = micros();
  lv_refr_now(display);
  Serial.printf("DISPLAY first frame %lu us\n", (unsigned long)(micros() - start));
  if (lcd) displayBrightness(brightnessPct * 255 / 100);
}
