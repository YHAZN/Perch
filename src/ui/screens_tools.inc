// Part of device_ui.cpp (included there, one translation unit). Screens: Gestures and Remote.
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
