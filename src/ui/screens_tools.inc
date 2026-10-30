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
    toast("Pair Perch in Bluetooth settings");
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
// The touch panel senses one finger. Scroll mode reuses the full touch surface.
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
  if (remotePadHint) setText(remotePadHint, "");
}
void remotePadEvent(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  const bool scroll = remoteScrollMode;
  lv_indev_t *input = lv_indev_active();
  if (!input) return;
  lv_point_t point;
  lv_indev_get_point(input, &point);
  auto &t = remotePadTouch;
  const uint32_t now = millis();
  if (code == LV_EVENT_PRESSED) {
    t = {};
    if (!remoteMouseReady()) {
      toast(remotePlatform == RemotePlatform::IPhone ? "Enable AssistiveTouch, then pair Perch"
                                                     : "Remove Perch in host Bluetooth, then pair again");
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
  } else if (code == LV_EVENT_LONG_PRESSED && t.active && !scroll && !t.moved && remoteMouseReady()) {
    remoteDragging = true;
    remoteMouseButtons(RemoteMouse::Left);
    setText(remotePadHint, "Dragging");
  } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && t.active) {
    if (!scroll && (t.pendingX || t.pendingY)) remoteMouseMove(t.pendingX, t.pendingY);
    if (code == LV_EVENT_RELEASED && !scroll && !t.moved && now - t.started < 300 && !remoteDragging)
      remoteMouseClick(RemoteMouse::Left);
    remotePointerReset();
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
  const char *names[] = {"Trackpad", "Media", "Slides", "Shortcuts"};
  setText(remoteModeTitle, names[mode]);
  hide(remoteModes, true);
  remoteScrollMode = false;
  setText(remoteScrollLabel, "Scroll");
  lv_obj_set_style_bg_color(remoteScrollButton, GRAPHITE, 0);
}
void refreshRemotePlatform() {
  setText(remotePlatformLabel, remotePlatformName(remotePlatform));
  setText(remoteShortcutTitle, remotePlatformName(remotePlatform));
  for (int i = 0; i < 6; ++i) {
    const RemoteShortcut *shortcut = remoteShortcut(remotePlatform, i);
    hide(remoteShortcutButtons[i], !shortcut);
    if (shortcut) setText(lv_obj_get_child(remoteShortcutButtons[i], 0), shortcut->label);
  }
  const RemoteShortcut present = remotePresentationShortcut(remotePlatform);
  hide(remotePresentButton, !present.key);
  if (present.key) setText(lv_obj_get_child(remotePresentButton, 0), present.label);
  lv_obj_set_width(remoteExitButton, present.key ? 102 : 212);
  lv_obj_set_x(remoteExitButton, present.key ? 124 : 14);
}
void selectRemotePlatform(RemotePlatform platform) {
  if (platform >= RemotePlatform::Count) return;
  remotePointerReset();
  remotePlatform = platform;
  settings.putUChar("remote-layout", static_cast<uint8_t>(platform));
  refreshRemotePlatform();
  hide(remotePlatformSheet, true);
  if (platform == RemotePlatform::IPhone) {
    hide(remoteModes, true);
    notice(
        "On iPhone, enable AssistiveTouch for the pointer and Full Keyboard Access for navigation in Settings > "
        "Accessibility. Pair Perch from the phone.",
        Screen::Remote);
  } else toast("Key layout saved");
}
void buildRemote() {
  lv_obj_t *s = scr[(int)Screen::Remote] = screenBase();
  remoteStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_width(remoteStatus, 94);
  lv_obj_set_style_text_align(remoteStatus, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(remoteStatus, LV_LABEL_LONG_WRAP);
  lv_obj_align(remoteStatus, LV_ALIGN_TOP_RIGHT, -14, 27);
  lv_obj_t *modePicker = plain(s);
  lv_obj_set_pos(modePicker, 12, 14);
  lv_obj_set_size(modePicker, 126, 44);
  remoteModeTitle = text(modePicker, "Trackpad", F_BODY, INK);
  lv_obj_align(remoteModeTitle, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_t *chevron = text(modePicker, LV_SYMBOL_DOWN, F_SMALL, MIST);
  lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -2, 0);
  onClick(modePicker, [] {
    remotePointerReset();
    hide(remoteModes, false);
  });
  remotePadPanel = plain(s);
  lv_obj_set_size(remotePadPanel, W, 260);
  lv_obj_t *pad = plain(remotePadPanel);
  lv_obj_set_pos(pad, 12, 66);
  lv_obj_set_size(pad, 216, 154);
  lv_obj_set_style_radius(pad, 22, 0);
  lv_obj_set_style_bg_color(pad, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
  remotePadHint = text(pad, "", F_SMALL, MIST);
  lv_obj_center(remotePadHint);
  lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
  for (lv_event_code_t event :
       {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_LONG_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
    lv_obj_add_event_cb(pad, remotePadEvent, event, nullptr);
  remoteScrollButton = remoteTile(remotePadPanel, 12, 230, 102, 30, "Scroll", [] {
    remotePointerReset();
    remoteScrollMode = !remoteScrollMode;
    setText(remoteScrollLabel, remoteScrollMode ? "Scroll on" : "Scroll");
    lv_obj_set_style_bg_color(remoteScrollButton, remoteScrollMode ? ICON_BG : GRAPHITE, 0);
  });
  remoteScrollLabel = lv_obj_get_child(remoteScrollButton, 0);
  remoteTile(remotePadPanel, 126, 230, 102, 30, "Right click", [] {
    if (remoteMouseReady()) remoteMouseClick(RemoteMouse::Right);
    else
      toast(remotePlatform == RemotePlatform::IPhone ? "Enable AssistiveTouch, then pair Perch"
                                                     : "Remove Perch in host Bluetooth, then pair again");
  });
  // Slides retain direct arrows, with explicit presentation start and exit.

  remoteSlides = plain(s);
  lv_obj_set_size(remoteSlides, W, 260);
  remoteZone(remoteSlides, 14, 102, LV_SYMBOL_LEFT, [] { remoteKey(RemoteKey::Left); });
  remoteZone(remoteSlides, 124, 102, LV_SYMBOL_RIGHT, [] { remoteKey(RemoteKey::Right); });
  remotePresentButton = remoteTile(remoteSlides, 14, 184, 102, 34, "Present", [] {
    const auto shortcut = remotePresentationShortcut(remotePlatform);
    if (shortcut.key) remoteKey(shortcut.key, shortcut.modifiers);
  });
  remoteExitButton = remoteTile(remoteSlides, 124, 184, 102, 34, "Exit", [] { remoteKey(RemoteKey::Escape); });
  remotePcPanel = plain(s);
  lv_obj_set_size(remotePcPanel, W, 260);
  remoteShortcutTitle = text(remotePcPanel, "", F_SMALL, MIST);
  lv_obj_align(remoteShortcutTitle, LV_ALIGN_TOP_MID, 0, 58);
  for (int i = 0; i < 6; ++i) {
    lv_obj_t *button = remoteShortcutButtons[i] =
        remoteTile(remotePcPanel, 14 + (i % 2) * 110, 84 + (i / 2) * 56, 102, 46, "", nullptr);
    lv_obj_remove_event_cb(button, remoteAction);
    lv_obj_add_event_cb(
        button,
        [](lv_event_t *e) {
          if (!remoteConnected()) {
            toast("Pair Perch in Bluetooth settings");
            return;
          }
          const auto shortcut = remoteShortcut(remotePlatform, uintptr_t(lv_event_get_user_data(e)));
          if (shortcut) remoteKey(shortcut->key, shortcut->modifiers);
        },
        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
  // Media: play/pause in the middle, tracks either side, volume below.
  remoteMediaPanel = plain(s);
  lv_obj_set_size(remoteMediaPanel, W, 260);
  remoteButton(remoteMediaPanel, 120, 104, 72, LV_SYMBOL_PLAY, [] { remoteMedia(RemoteMedia::PlayPause); });
  remoteButton(remoteMediaPanel, 46, 104, 52, LV_SYMBOL_PREV, [] { remoteMedia(RemoteMedia::Previous); });
  remoteButton(remoteMediaPanel, 194, 104, 52, LV_SYMBOL_NEXT, [] { remoteMedia(RemoteMedia::Next); });
  remoteButton(remoteMediaPanel, 58, 174, 46, LV_SYMBOL_MINUS, [] { remoteMedia(RemoteMedia::VolumeDown); }, true);
  remoteButton(remoteMediaPanel, 120, 174, 46, LV_SYMBOL_MUTE, [] { remoteMedia(RemoteMedia::Mute); });
  remoteButton(remoteMediaPanel, 182, 174, 46, LV_SYMBOL_PLUS, [] { remoteMedia(RemoteMedia::VolumeUp); }, true);
  // Choosing a tool is a separate sheet, rather than four permanent tiny tabs.
  remoteModes = plain(s);
  lv_obj_add_flag(remoteModes, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(remoteModes, W, H);
  lv_obj_set_style_bg_color(remoteModes, VOID_, 0);
  lv_obj_set_style_bg_opa(remoteModes, LV_OPA_COVER, 0);
  lv_obj_t *title = text(remoteModes, "Controls", F_BODY, INK);
  lv_obj_set_pos(title, 22, 25);
  lv_obj_t *platform = pressedFeedback(plain(remoteModes));
  lv_obj_set_pos(platform, 128, 14);
  lv_obj_set_size(platform, 98, 44);
  remotePlatformLabel = text(platform, "", F_SMALL, MIST);
  lv_obj_align(remotePlatformLabel, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_t *platformArrow = text(platform, LV_SYMBOL_DOWN, F_SMALL, MIST);
  lv_obj_align(platformArrow, LV_ALIGN_RIGHT_MID, -2, 0);
  onClick(platform, [] { hide(remotePlatformSheet, false); });
  const char *names[] = {"Trackpad", "Media", "Slides", "Shortcuts"};
  const char *symbols[] = {LV_SYMBOL_GPS, LV_SYMBOL_PLAY, LV_SYMBOL_RIGHT, LV_SYMBOL_KEYBOARD};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *tile = pressedFeedback(plain(remoteModes));
    lv_obj_set_pos(tile, 16 + (i % 2) * 108, 68 + (i / 2) * 82);
    lv_obj_set_size(tile, 100, 72);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_bg_color(tile, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_t *icon = text(tile, symbols[i], F_BODY, INK);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *label = text(tile, names[i], F_SMALL, MIST);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        tile, [](lv_event_t *e) { setRemoteMode(uintptr_t(lv_event_get_user_data(e))); }, LV_EVENT_CLICKED,
        (void *)(uintptr_t)i);
  }
  lv_obj_t *done = pill(remoteModes, "Done", 144);
  lv_obj_set_size(done, 144, 36);
  lv_obj_set_pos(done, 48, 226);
  onClick(done, [] { hide(remoteModes, true); });
  remotePlatformSheet = plain(s);
  lv_obj_set_size(remotePlatformSheet, W, H);
  lv_obj_add_flag(remotePlatformSheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(remotePlatformSheet, VOID_, 0);
  lv_obj_set_style_bg_opa(remotePlatformSheet, LV_OPA_COVER, 0);
  lv_obj_t *platformTitle = text(remotePlatformSheet, "Key layout", F_BODY, INK);
  lv_obj_set_pos(platformTitle, 22, 25);
  lv_obj_t *close = pressedFeedback(plain(remotePlatformSheet));
  lv_obj_set_pos(close, 166, 14);
  lv_obj_set_size(close, 60, 44);
  lv_obj_center(text(close, "Done", F_SMALL, MIST));
  onClick(close, [] { hide(remotePlatformSheet, true); });
  for (int i = 0; i < int(RemotePlatform::Count); ++i) {
    lv_obj_t *row = pressedFeedback(plain(remotePlatformSheet));
    lv_obj_set_pos(row, 16, 64 + i * 48);
    lv_obj_set_size(row, 208, 42);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_t *label = text(row, remotePlatformName(static_cast<RemotePlatform>(i)), F_BODY, INK);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        row,
        [](lv_event_t *e) { selectRemotePlatform(static_cast<RemotePlatform>(uintptr_t(lv_event_get_user_data(e)))); },
        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
  hide(remotePlatformSheet, true);
  refreshRemotePlatform();
  setRemoteMode(0);
}
