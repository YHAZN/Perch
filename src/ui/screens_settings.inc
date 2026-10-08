// Part of device_ui.cpp (included there, one translation unit). Screens: Settings, AI settings, notice, Control Center.
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
  lv_obj_t *sleepRow = row(s, 214, "Screen off", &sleepValue);
  onClick(sleepRow, [] {
    sleepChoice = (sleepChoice + 1) % (sizeof(SLEEP_CHOICES) / sizeof(SLEEP_CHOICES[0]));
    settings.putUChar("sleep", sleepChoice);
    refreshDynamic();
  });
  lv_obj_t *storage = row(s, 266, "Storage", &storageValue, 64);
  lv_obj_align(lv_obj_get_child(storage, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_align(storageValue, LV_ALIGN_TOP_RIGHT, -20, 12);
  storageSub = text(storage, "", F_SMALL, MIST);
  lv_obj_set_pos(storageSub, 22, 36);
  lv_obj_t *forget = row(s, 330, "Forget last hour", nullptr, 64);
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
  lv_obj_t *about = row(s, 394, "About", nullptr, 64);
  lv_obj_align(lv_obj_get_child(about, 0), LV_ALIGN_TOP_LEFT, 22, 12);
  lv_obj_t *version = text(about, "Perch 0.5, built " __DATE__, F_SMALL, MIST);
  lv_obj_set_pos(version, 22, 36);
  lv_obj_t *spacer = plain(s);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_pos(spacer, 0, 514);
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
