// Part of device_ui.cpp (included there, one translation unit). USB serial commands (mirror and developer tools).
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
  if (command == '$') {
    // Developer: "$<text>" prints text as the answer screen would show it ("<br>" = new line).
    String in = Serial.readStringUntil('\n');
    in.replace("<br>", "\n");
    Serial.printf("MATH %s\n", cleanMath(in).c_str());
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
  if (command == '~') {
    if (ensureSavedJpeg()) aiShrinkTest(savedJpeg, jpegBytes);
    return;
  }
  if (command == '>') {
    // Developer: ">text" sends a chat message (with any attached photos).
    String words = Serial.readStringUntil('\n');
    launch(Screen::Ask);
    sendMessage(words);
    return;
  }
  if (command == '#') {
    // Developer: "#<sha256 hex>|<base64 signature>" checks a release signature on the device.
    const String arg = Serial.readStringUntil('\n');
    const int bar = arg.indexOf('|');
    Serial.printf("SIGNATURE %s\n",
                  bar > 0 && updaterVerify(arg.substring(0, bar), arg.substring(bar + 1)) ? "valid" : "INVALID");
    return;
  }
  if (command == '=') {
    // Developer: "=<https manifest url>" sets where updates come from ("=" alone clears it).
    String url = Serial.readStringUntil('\n');
    url.trim();
    if (url == "?") {
      pauseBluetooth();
      Serial.printf("UPDATE check %s\n", updaterCheck() ? "started" : "not started");
      return;
    }
    if (url.length() && !url.startsWith("https://")) {
      Serial.println("UPDATE url must be https");
      return;
    }
    updaterSetUrl(url);
    Serial.printf("UPDATE url %s\n", url.length() ? "saved" : "cleared");
    return;
  }
  if (command == '%') {
    // Developer: build a request without sending it. "%g2" Gemini 2 pages, "%o1" GPT,
    // "%ot" / "%gt" speech-to-text. Uses the saved model and effort; never the key.
    const String arg = Serial.readStringUntil('\n');
    AiOptions o = aiOptions(arg.length() == 0 || arg[0] != 'o');
    o.key = "";
    if (arg.indexOf('c') >= 0) {
      aiDryRunChat(o);
      return;
    }
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
    if (!ensureSavedJpeg()) {
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
    case 'H': launch(Screen::Chats); break;
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
      int xs[16], ys[16];
      const int n = touchRecent(xs, ys, 16);
      for (int i = 0; i < n; ++i) Serial.printf("TOUCH recent %d,%d\n", xs[i], ys[i]);
      break;
    }
    case 'S': {  // developer: a sample exchange in the chat, for layout checks (not sent anywhere)
      ChatMsg q;
      q.text = "Sample question for a layout check";
      chat.push_back(q);
      ChatMsg a;
      a.user = false;
      a.text =
          "Q1: c. O(n^2)\n1. **.get(i)** on a LinkedList walks i nodes: O(n)\n2. The loop calls it n times\n\n"
          "Q2: b. 2n\n1. The loop runs while i < data.size() * 2\n\n"
          "Sample written on the device, not by a model.";
      chat.push_back(a);
      ChatMsg mq;
      mq.text = "And the maths one?";
      chat.push_back(mq);
      ChatMsg ma;  // LaTeX as models write it, to check the maths display
      ma.user = false;
      ma.text =
          "Answer: \\(x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}\\)\n"
          "Steps:\n1. $\\Delta = b^2 - 4ac \\geq 0$\n2. $\\int_0^1 x^2\\,dx = \\frac{1}{3}$, $\\theta = 30^\\circ$\n"
          "3. $$\\sum_{i=1}^{n} i = \\frac{n(n+1)}{2}$$";
      chat.push_back(ma);
      launch(Screen::Ask);
      rebuildChat();
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
    case 'q':  // developer: send the message box; an empty chat gets the newest photo attached
      launch(Screen::Ask);
      if (chat.empty() && attachIds.empty()) {
        refreshPhotos();
        if (photoCount) attachIds.push_back(photoIds[0]);
      }
      sendMessage(draft);
      break;
    case 'Q':
      launch(Screen::Ask);
      chatTakePhoto();
      break;
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
