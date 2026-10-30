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
void remoteAction(lv_event_t *e) {
  if (!remoteConnected()) {
    toast("Connect Perch in PC Bluetooth settings");
    return;
  }
  ((void (*)())lv_event_get_user_data(e))();
}
void remoteTap(lv_obj_t *o, void (*fn)()) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, remoteAction, LV_EVENT_CLICKED, (void *)fn);
}
lv_obj_t *remoteZone(lv_obj_t *parent, int x, int w, const char *symbol, void (*fn)()) {
  lv_obj_t *z = plain(parent);
  lv_obj_set_size(z, w, 108);
  lv_obj_set_pos(z, x, 62);
  lv_obj_set_style_radius(z, 24, 0);
  lv_obj_set_style_bg_color(z, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(z, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(z, ICON_BG, LV_STATE_PRESSED);
  lv_obj_center(text(z, symbol, F_LARGE, INK));
  remoteTap(z, fn);
  return z;
}
lv_obj_t *remoteButton(lv_obj_t *parent, int x, int y, int size, const char *symbol, void (*fn)(),
                       bool repeat = false) {
  lv_obj_t *b = circle(parent, size, ICON_BG, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_set_pos(b, x - size / 2, y - size / 2);
  lv_obj_set_style_bg_color(b, INK, LV_STATE_PRESSED);
  lv_obj_t *l = text(b, symbol, size > 56 ? F_LARGE : F_BODY, INK);
  lv_obj_set_style_text_color(l, VOID_, LV_STATE_PRESSED);
  lv_obj_center(l);
  if (repeat) {
    // Volume acts immediately, repeats while held, and sends no extra click on release.
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, remoteAction, LV_EVENT_PRESSED, (void *)fn);
    lv_obj_add_event_cb(b, remoteAction, LV_EVENT_LONG_PRESSED_REPEAT, (void *)fn);
  } else remoteTap(b, fn);
  return b;
}
// The touch panel senses one finger. A separate scroll rail avoids ambiguous gestures.
struct RemotePadTouch {
  bool active = false, moved = false;
  int x = 0, y = 0, startX = 0, startY = 0, scroll = 0;
  int pendingX = 0, pendingY = 0;
  uint32_t started = 0, sent = 0;
} remotePadTouch;

void remotePointerReset() {
  remotePadTouch = {};
  if (!remoteDragging) return;
  remoteDragging = false;
  remoteMouseButtons(0);
  if (remoteDragLabel) setText(remoteDragLabel, "Drag");
}
void remotePadEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const bool scroll = lv_event_get_user_data(e) != nullptr;
  lv_indev_t *input = lv_indev_active();
  if (!input) return;
  lv_point_t point;
  lv_indev_get_point(input, &point);
  auto &t = remotePadTouch;
  const uint32_t now = millis();
  if (code == LV_EVENT_PRESSED) {
    t = {};
    if (!remoteConnected()) {
      toast("Connect Perch in PC Bluetooth settings");
      return;
    }
    t.active = true;
    t.x = t.startX = point.x;
    t.y = t.startY = point.y;
    t.started = t.sent = now;
  } else if (code == LV_EVENT_PRESSING && t.active) {
    const int dx = point.x - t.x, dy = point.y - t.y;
    t.x = point.x;
    t.y = point.y;
    if (abs(point.x - t.startX) > 5 || abs(point.y - t.startY) > 5) t.moved = true;
    if (scroll) {
      t.scroll -= dy;
      const int steps = t.scroll / 10;
      if (steps) {
        remoteMouseMove(0, 0, steps);
        t.scroll -= steps * 10;
      }
    } else {
      // Low-speed precision, faster swipes cover the desktop. Coalesce to <=40 Hz.
      const int gain = abs(dx) + abs(dy) > 9 ? 3 : 2;
      t.pendingX += dx * gain;
      t.pendingY += dy * gain;
      if (now - t.sent >= 25 && (t.pendingX || t.pendingY)) {
        remoteMouseMove(t.pendingX, t.pendingY);
        t.pendingX = t.pendingY = 0;
        t.sent = now;
      }
    }
  } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && t.active) {
    if (!scroll && (t.pendingX || t.pendingY)) remoteMouseMove(t.pendingX, t.pendingY);
    if (code == LV_EVENT_RELEASED && !scroll && !t.moved && now - t.started < 300 && !remoteDragging)
      remoteMouseClick(RemoteMouse::Left);
    t = {};
  }
}
lv_obj_t *remoteTile(lv_obj_t *p, int x, int y, int w, int h, const char *label, void (*fn)()) {
  lv_obj_t *b = pressedFeedback(plain(p));
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_bg_color(b, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_center(text(b, label, F_BODY, INK));
  remoteTap(b, fn);
  return b;
}
void setRemoteMode(uint8_t mode) {
  if (mode > 3) return;
  remotePointerReset();
  remoteMode = mode;
  hide(remotePadPanel, mode != 0);
  hide(remoteMediaPanel, mode != 1);
  hide(remoteSlides, mode != 2);
  hide(remotePcPanel, mode != 3);
  for (int i = 0; i < 4; ++i) {
    lv_obj_set_style_text_color(remoteModeLabel[i], i == mode ? INK : MIST, 0);
    lv_obj_set_style_bg_opa(remoteModeButton[i], i == mode ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  }
}
void buildRemote() {
  lv_obj_t *s = scr[(int)Screen::Remote] = screenBase();
  remoteStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_width(remoteStatus, W - 70);
  lv_obj_set_style_text_align(remoteStatus, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(remoteStatus, LV_LABEL_LONG_WRAP);
  lv_obj_align(remoteStatus, LV_ALIGN_TOP_MID, 0, 14);
  remotePadPanel = plain(s);
  lv_obj_set_size(remotePadPanel, W, 220);
  for (int i = 0; i < 2; ++i) {
    lv_obj_t *pad = plain(remotePadPanel);
    lv_obj_set_pos(pad, i ? 204 : 14, 48);
    lv_obj_set_size(pad, i ? 22 : 182, 126);
    lv_obj_set_style_radius(pad, i ? 11 : 20, 0);
    lv_obj_set_style_bg_color(pad, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
    lv_obj_t *hint = text(pad, i ? LV_SYMBOL_UP "\n\n" LV_SYMBOL_DOWN : "Tap to click", F_SMALL, MIST);
    lv_obj_center(hint);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t event : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
      lv_obj_add_event_cb(pad, remotePadEvent, event, i ? (void *)1 : nullptr);
  }
  remoteTile(remotePadPanel, 14, 184, 64, 34, "Click", [] { remoteMouseClick(RemoteMouse::Left); });
  remoteTile(remotePadPanel, 88, 184, 64, 34, "Right", [] { remoteMouseClick(RemoteMouse::Right); });
  lv_obj_t *drag = remoteTile(remotePadPanel, 162, 184, 64, 34, "Drag", [] {
    remoteDragging = !remoteDragging;
    remoteMouseButtons(remoteDragging ? RemoteMouse::Left : 0);
    setText(remoteDragLabel, remoteDragging ? "Drop" : "Drag");
  });
  remoteDragLabel = lv_obj_get_child(drag, 0);
  // Slides retain direct arrows, with explicit presentation start and exit.

  remoteSlides = plain(s);
  lv_obj_set_size(remoteSlides, W, 220);
  remoteZone(remoteSlides, 14, 102, LV_SYMBOL_LEFT, [] { remoteKey(RemoteKey::Left); });
  remoteZone(remoteSlides, 124, 102, LV_SYMBOL_RIGHT, [] { remoteKey(RemoteKey::Right); });
  remoteTile(remoteSlides, 14, 184, 102, 34, "Present", [] { remoteKey(RemoteKey::F5); });
  remoteTile(remoteSlides, 124, 184, 102, 34, "Exit", [] { remoteKey(RemoteKey::Escape); });
  remotePcPanel = plain(s);
  lv_obj_set_size(remotePcPanel, W, 220);
  lv_obj_t *windowsLabel = text(remotePcPanel, "Windows", F_SMALL, MIST);
  lv_obj_align(windowsLabel, LV_ALIGN_TOP_MID, 0, 40);
  remoteTile(remotePcPanel, 14, 62, 102, 46, "Switch app", [] { remoteKey(RemoteKey::Tab, RemoteModifier::Alt); });
  remoteTile(remotePcPanel, 124, 62, 102, 46, "Desktop", [] { remoteKey(RemoteKey::D, RemoteModifier::Windows); });
  remoteTile(remotePcPanel, 14, 116, 102, 46, "Tasks", [] { remoteKey(RemoteKey::Tab, RemoteModifier::Windows); });
  remoteTile(remotePcPanel, 124, 116, 102, 46, "Search", [] { remoteKey(RemoteKey::S, RemoteModifier::Windows); });
  remoteTile(remotePcPanel, 14, 170, 102, 46, "Copy", [] { remoteKey(RemoteKey::C, RemoteModifier::Control); });
  remoteTile(remotePcPanel, 124, 170, 102, 46, "Paste", [] { remoteKey(RemoteKey::V, RemoteModifier::Control); });
  // Media: play/pause in the middle, tracks either side, volume below.
  remoteMediaPanel = plain(s);
  lv_obj_set_size(remoteMediaPanel, W, 220);
  remoteButton(remoteMediaPanel, 120, 104, 72, LV_SYMBOL_PLAY, [] { remoteMedia(RemoteMedia::PlayPause); });
  remoteButton(remoteMediaPanel, 46, 104, 52, LV_SYMBOL_PREV, [] { remoteMedia(RemoteMedia::Previous); });
  remoteButton(remoteMediaPanel, 194, 104, 52, LV_SYMBOL_NEXT, [] { remoteMedia(RemoteMedia::Next); });
  remoteButton(remoteMediaPanel, 58, 174, 46, LV_SYMBOL_MINUS, [] { remoteMedia(RemoteMedia::VolumeDown); }, true);
  remoteButton(remoteMediaPanel, 120, 174, 46, LV_SYMBOL_MUTE, [] { remoteMedia(RemoteMedia::Mute); });
  remoteButton(remoteMediaPanel, 182, 174, 46, LV_SYMBOL_PLUS, [] { remoteMedia(RemoteMedia::VolumeUp); }, true);
  // Four views share one compact switch; each screen exposes only its own controls.
  lv_obj_t *mode = plain(s);
  lv_obj_set_size(mode, 216, 36);
  lv_obj_align(mode, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME + 6);
  lv_obj_set_style_radius(mode, 18, 0);
  lv_obj_set_style_bg_color(mode, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(mode, LV_OPA_COVER, 0);
  const char *names[4] = {"Pad", "Media", "Slides", "PC"};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *half = remoteModeButton[i] = pressedFeedback(plain(mode));
    lv_obj_set_size(half, 54, 36);
    lv_obj_set_pos(half, i * 54, 0);
    lv_obj_set_style_radius(half, 18, 0);
    lv_obj_set_style_bg_color(half, ICON_BG, 0);
    remoteModeLabel[i] = text(half, names[i], F_SMALL, MIST);
    lv_obj_center(remoteModeLabel[i]);
    lv_obj_add_flag(half, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        half, [](lv_event_t *e) { setRemoteMode(uintptr_t(lv_event_get_user_data(e))); }, LV_EVENT_CLICKED,
        (void *)(uintptr_t)i);
  }
  setRemoteMode(0);
}
