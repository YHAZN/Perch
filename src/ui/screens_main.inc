// Part of device_ui.cpp (included there, one translation unit). Screens: face, app grid, camera, Ask, answer, photos.
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
  // Labelled, not an icon: it is the only way to start over, so it must be found.
  newChatBtn = chip(pageBar, "New chat");
  lv_obj_set_height(newChatBtn, 38);
  lv_obj_set_style_radius(newChatBtn, 19, 0);
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
