// Part of device_ui.cpp (included there, one translation unit). The main loop step: AI results, camera, sleep, queue.
void deviceTick() {
  if (!framebuffer) return;
  const unsigned long now = millis();
  networkRoam(!aiBusy() && current != Screen::Wifi && updaterState() != UpdateState::Installing);
  {
    // Never sleep while something is happening; the USB mirror counts as use.
    if (aiBusy() || micRecording() || captureRequested || captureProcessing ||
        (lastSerialMs && now - lastSerialMs < 2000) || !lastActivity)
      lastActivity = now;
    const unsigned long after = SLEEP_CHOICES[sleepChoice] * 1000UL;
    if (after && !screenAsleep) {
      const unsigned long idle = now - lastActivity;
      if (idle > after) sleepScreen();
      else if (idle + 10000 > after && !screenDim) {
        screenDim = true;
        displayBrightness(max(8, brightnessPct * 255 / 100 / 4));
      }
    } else if (screenAsleep && now - lastActivity < 500) {
      wakeScreen();  // something happened (an answer, the mirror)
    }
  }
  // Collect finished requests even if the user has moved to another app.
  String result;
  const AiState ai = aiPoll(result);
  if ((ai == AiState::Done || ai == AiState::Failed) && pending == Pending::Transcribe) {
    pending = Pending::None;
    if (dictating) {
      dictating = false;
      if (ai == AiState::Failed) toast("Could not hear that. Try again");
      else {
        String current = lv_textarea_get_text(passwordField);
        if (current.length() && !current.endsWith(" ")) lv_textarea_add_text(passwordField, " ");
        lv_textarea_add_text(passwordField, result.c_str());
      }
    } else {
      chatHeard(ai == AiState::Done, result);
    }
  } else if (ai == AiState::Cancelled && pending == Pending::Transcribe) {
    pending = Pending::None;
  } else if (ai == AiState::Cancelled && pending == Pending::Answer) {
    pending = Pending::None;
    chatCancelled();
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
    // Kept for the clock card too: the newest answer, with the chat's newest photo.
    static uint16_t *th = (uint16_t *)ps_malloc(THUMB * THUMB * 2);  // PSRAM: internal RAM is for Wi-Fi/TLS
    const bool haveThumb = pendingPhotoId && th && loadPhotoThumb(pendingPhotoId, th);
    const uint32_t when = clockKnown() ? (uint32_t)time(nullptr) : 0;
    if (!saveAnswer(result, pendingGemini, pendingPhotoId, when, haveThumb ? th : nullptr))
      Serial.println("STORAGE_ERROR Answer not saved");
    loadLatestAnswer();
    chatFinished(true, result);
    refreshDynamic();
    if (screenAsleep || screenDim) wakeScreen();
  } else if (ai == AiState::Failed) {
    pending = Pending::None;
    chatFinished(false, result);
  }
  if (btPaused && !aiBusy() && updaterState() != UpdateState::Checking && updaterState() != UpdateState::Installing)
    resumeBluetooth();
  zoomTick();
  {
    // The answer grows in the chat as it streams in.
    static uint32_t seen = 0;
    static unsigned long drawnAt = 0;
    if (pending == Pending::Answer && aiBusy()) {
      String partial;
      const uint32_t v = aiPartial(partial);
      if (v != seen && partial.length() && now - drawnAt > 350) {
        seen = v;
        drawnAt = now;
        chatPartial(partial);
      }
    }
    // A message written without Wi-Fi goes out when Wi-Fi is back.
    static unsigned long retryAt = 0;
    if (!aiBusy() && !chat.empty() && chat.back().user && chat.back().pending && networkConnected() && now > retryAt) {
      retryAt = now + 10000;
      dispatchChat();
    }
  }
  static bool btStarted = false;  // start a few seconds after boot, once the UI is up
  if (!btStarted && now > 4000) {
    btStarted = true;
    if (bluetoothOn && !aiBusy()) remoteBegin();
  }
  if (captureReady && !captureRequested) {
    String problem;
    const bool forChat = pageCamera;
    if (!acceptCapture(problem)) notice(problem, current == Screen::Camera ? Screen::Camera : Screen::Face);
    else if (forChat) {
      pageCamera = false;
      chatPhotoTaken();  // back to the chat; attached once saved
    }
  }
  if (savedPhotoId && savedPhotoId != latestPhotoId) {
    latestPhotoId = savedPhotoId;
    refreshPhotos();
    chatPhotoSaved(savedPhotoId);
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
  const bool wantsCamera = (current == Screen::Camera || current == Screen::Gestures) && !screenAsleep;
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
