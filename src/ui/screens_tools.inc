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

// ---------- Remote: single-finger Bluetooth controls ----------
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
// All primary tools use the same full-size, one-finger gesture surface.
RemoteGesture::Surface remoteSurface;
int remotePendingX = 0, remotePendingY = 0;
uint32_t remoteMotionAt = 0;

bool remoteNavigationMode() { return remotePlatform == RemotePlatform::IPhone && !remotePhonePointer; }
bool remoteNavigationReady() { return remoteNavigationMode() && (remoteSubscriptions() & 1); }
bool remoteControlReady() { return remoteNavigationMode() ? remoteNavigationReady() : remoteMouseReady(); }
void remotePointerReset() {
  remoteSurface = {};
  remotePendingX = remotePendingY = 0;
  if (remoteDragging) remoteMouseButtons(0);
  remoteDragging = false;
  if (remotePadHint) setText(remotePadHint, "");
}
void remoteFeedback(const char *symbol) {
  if (remoteMode == 1) setText(remoteMediaGlyph, symbol);
  if (remoteMode == 2) setText(remoteSlideGlyph, symbol);
  remoteFeedbackUntil = millis() + 500;
}
void remoteFeedbackTick(uint32_t now) {
  if (remoteFeedbackUntil && int32_t(now - remoteFeedbackUntil) >= 0) {
    remoteFeedbackUntil = 0;
    setText(remoteMediaGlyph, LV_SYMBOL_PLAY);
    setText(remoteSlideGlyph, LV_SYMBOL_RIGHT);
  }
}
void remoteGestureAction(RemoteGesture::Event event) {
  using RemoteGesture::Kind;
  switch (event.kind) {
    case Kind::Motion:
      remotePendingX += event.x;
      remotePendingY += event.y;
      break;
    case Kind::Scroll: remoteMouseMove(0, 0, event.x); break;
    case Kind::Click: remoteMouseClick(RemoteMouse::Left); break;
    case Kind::RightClick: remoteMouseClick(RemoteMouse::Right); break;
    case Kind::DragBegin:
      remoteDragging = true;
      remoteMouseButtons(RemoteMouse::Left);
      break;
    case Kind::DragEnd:
      remoteDragging = false;
      remoteMouseButtons(0);
      break;
    case Kind::Next:
      if (remoteMode == 1) remoteMedia(RemoteMedia::Next);
      else remoteKey(RemoteKey::Right);
      remoteFeedback(remoteMode == 1 ? LV_SYMBOL_NEXT : LV_SYMBOL_RIGHT);
      break;
    case Kind::Previous:
      if (remoteMode == 1) remoteMedia(RemoteMedia::Previous);
      else remoteKey(RemoteKey::Left);
      remoteFeedback(remoteMode == 1 ? LV_SYMBOL_PREV : LV_SYMBOL_LEFT);
      break;
    case Kind::PlayPause:
      remoteMedia(RemoteMedia::PlayPause);
      remoteFeedback(LV_SYMBOL_OK);
      break;
    case Kind::Mute:
      remoteMedia(RemoteMedia::Mute);
      remoteFeedback(LV_SYMBOL_MUTE);
      break;
    case Kind::Volume:
      for (int i = 0; i < min(4, abs(event.x)); ++i)
        remoteMedia(event.x > 0 ? RemoteMedia::VolumeUp : RemoteMedia::VolumeDown);
      remoteFeedback(event.x > 0 ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS);
      break;
    case Kind::FocusNext: remoteKey(RemoteKey::Tab); break;
    case Kind::FocusPrevious: remoteKey(RemoteKey::Tab, RemoteModifier::Shift); break;
    case Kind::Up: remoteKey(RemoteKey::Up); break;
    case Kind::Down: remoteKey(0x51); break;
    case Kind::Activate: remoteKey(RemoteKey::Space); break;
    case Kind::Enter: remoteKey(0x28); break;
    default: break;
  }
}
void remoteFlushMotion(uint32_t now, bool force = false) {
  if ((force || now - remoteMotionAt >= 25) && (remotePendingX || remotePendingY)) {
    remoteMouseMove(remotePendingX, remotePendingY);
    remotePendingX = remotePendingY = 0;
    remoteMotionAt = now;
  }
}
void remotePadEvent(lv_event_t *e) {
  const auto code = lv_event_get_code(e);
  lv_indev_t *input = lv_indev_active();
  if (!input) return;
  lv_point_t point;
  lv_indev_get_point(input, &point);
  lv_area_t bounds;
  lv_obj_get_coords(lv_event_get_current_target_obj(e), &bounds);
  const uint32_t now = millis();
  if (code == LV_EVENT_PRESSED) {
    if (!remoteConnected() || (remoteMode == 0 && !remoteControlReady())) {
      remotePointerReset();
      toast(remotePlatform == RemotePlatform::IPhone ? "Enable Full Keyboard Access or AssistiveTouch"
                                                     : "Pair Perch in Bluetooth settings");
      return;
    }
    using RemoteGesture::Mode;
    const Mode mode = remoteMode == 1          ? Mode::Media
                      : remoteMode == 2        ? Mode::Slides
                      : remoteNavigationMode() ? Mode::Navigation
                                               : Mode::Pointer;
    remoteGestureAction(remoteSurface.press(mode, point.x - bounds.x1, point.y - bounds.y1, now,
                                            lv_obj_get_width(lv_event_get_current_target_obj(e))));
  } else if (code == LV_EVENT_PRESSING) {
    remoteGestureAction(remoteSurface.move(point.x - bounds.x1, point.y - bounds.y1));
    remoteGestureAction(remoteSurface.hold(now));
    remoteFlushMotion(now);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    remoteFlushMotion(now, true);
    remoteGestureAction(remoteSurface.release(now, code == LV_EVENT_PRESS_LOST));
  }
}
lv_obj_t *remoteGestureSurface(lv_obj_t *parent, int height, lv_obj_t **glyph, const char *symbol) {
  lv_obj_t *pad = plain(parent);
  lv_obj_set_pos(pad, 8, 62);
  lv_obj_set_size(pad, 224, height);
  lv_obj_set_style_radius(pad, 24, 0);
  lv_obj_set_style_bg_color(pad, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pad, ICON_BG, LV_STATE_PRESSED);
  if (glyph) {
    *glyph = text(pad, symbol, F_LARGE, INK);
    lv_obj_center(*glyph);
  }
  lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
  for (lv_event_code_t event : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
    lv_obj_add_event_cb(pad, remotePadEvent, event, nullptr);
  return pad;
}
void remoteGestureHelp() {
  const char *help =
      remoteMode == 1
          ? "Tap to play or pause. Swipe left or right for tracks. Swipe up or down for volume. Hold to mute."
      : remoteMode == 2        ? "Tap for the next slide. Swipe right or hold for previous. Swipe left for next."
      : remoteNavigationMode() ? "Swipe left or right to change focus. Tap to activate. Swipe up or down for arrow "
                                 "keys. Hold for Enter. Enable Full Keyboard Access on iPhone."
                               : "Tap to click. Hold for right-click. Double-tap and keep your finger down to drag. "
                                 "Start at the right edge to scroll.";
  remotePointerReset();
  hide(remoteModes, true);
  notice(help, Screen::Remote);
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
  if (remoteMode != mode) settings.putUChar("remote-mode", mode);
  remoteMode = mode;
  hide(remotePadPanel, mode != 0);
  hide(remoteMediaPanel, mode != 1);
  hide(remoteSlides, mode != 2);
  hide(remotePcPanel, mode != 3);
  hide(remoteMoreButton, mode != 2);
  hide(remoteStatus, mode == 2);
  hide(remoteSlideOptions, true);
  hide(remotePlatformSheet, true);
  hide(remotePhoneOptions, true);
  const char *names[] = {"Trackpad", "Media", "Slides", "Shortcuts"};
  setText(remoteModeTitle, mode == 0 && remoteNavigationMode() ? "Navigate" : names[mode]);
  hide(remoteModes, true);
  remoteFeedbackUntil = 0;
  setText(remoteMediaGlyph, LV_SYMBOL_PLAY);
  setText(remoteSlideGlyph, LV_SYMBOL_RIGHT);
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
  lv_obj_set_width(remoteExitButton, 208);
  lv_obj_set_x(remoteExitButton, 16);
}
void selectRemotePlatform(RemotePlatform platform) {
  if (platform >= RemotePlatform::Count) return;
  remotePointerReset();
  remotePlatform = platform;
  settings.putUChar("remote-layout", static_cast<uint8_t>(platform));
  refreshRemotePlatform();
  hide(remotePlatformSheet, true);
  hide(remotePhoneOptions, true);
  if (platform == RemotePlatform::IPhone) {
    hide(remoteModes, true);
    notice(remotePhonePointer ? "On iPhone, enable AssistiveTouch in Settings > Accessibility > Touch. Pair Perch "
                                "under AssistiveTouch > Devices > Bluetooth Devices."
                              : "On iPhone, enable Full Keyboard Access in Settings > Accessibility > Keyboards. Pair "
                                "Perch in Bluetooth settings. Swipe to change focus; tap to activate.",
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
  lv_obj_t *pad = remoteGestureSurface(remotePadPanel, 198, nullptr, "");
  remotePadHint = text(pad, "", F_SMALL, MIST);
  lv_obj_set_width(remotePadHint, 190);
  lv_obj_set_style_text_align(remotePadHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(remotePadHint);
  // A quiet edge mark shows the scroll region; its hit area is a generous 40 px.
  lv_obj_t *scrollMark = plain(pad);
  lv_obj_set_size(scrollMark, 2, 72);
  lv_obj_align(scrollMark, LV_ALIGN_RIGHT_MID, -16, 0);
  lv_obj_set_style_bg_color(scrollMark, MIST, 0);
  lv_obj_set_style_bg_opa(scrollMark, LV_OPA_40, 0);
  // Slides retain direct arrows, with explicit presentation start and exit.

  remoteSlides = plain(s);
  lv_obj_set_size(remoteSlides, W, 260);
  remoteGestureSurface(remoteSlides, 198, &remoteSlideGlyph, LV_SYMBOL_RIGHT);
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
  remoteGestureSurface(remoteMediaPanel, 198, &remoteMediaGlyph, LV_SYMBOL_PLAY);
  remoteMoreButton = pressedFeedback(plain(s));
  lv_obj_set_pos(remoteMoreButton, 184, 14);
  lv_obj_set_size(remoteMoreButton, 44, 44);
  lv_obj_center(text(remoteMoreButton, "...", F_BODY, INK));
  onClick(remoteMoreButton, [] {
    remotePointerReset();
    hide(remoteSlideOptions, false);
  });
  remoteSlideOptions = plain(s);
  lv_obj_set_size(remoteSlideOptions, W, H);
  lv_obj_add_flag(remoteSlideOptions, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(remoteSlideOptions, VOID_, 0);
  lv_obj_set_style_bg_opa(remoteSlideOptions, LV_OPA_COVER, 0);
  lv_obj_t *slideTitle = text(remoteSlideOptions, "Presentation", F_BODY, INK);
  lv_obj_set_pos(slideTitle, 22, 25);
  remotePresentButton = remoteTile(remoteSlideOptions, 16, 100, 208, 48, "Present", [] {
    const auto shortcut = remotePresentationShortcut(remotePlatform);
    if (shortcut.key) remoteKey(shortcut.key, shortcut.modifiers);
    hide(remoteSlideOptions, true);
  });
  remoteExitButton = remoteTile(remoteSlideOptions, 16, 160, 208, 48, "Exit", [] {
    remoteKey(RemoteKey::Escape);
    hide(remoteSlideOptions, true);
  });
  lv_obj_t *slideDone = pill(remoteSlideOptions, "Done", 144);
  lv_obj_set_pos(slideDone, 48, 220);
  onClick(slideDone, [] { hide(remoteSlideOptions, true); });
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
  lv_obj_t *help = pill(remoteModes, "Help", 96);
  lv_obj_set_size(help, 96, 36);
  lv_obj_set_pos(help, 16, 226);
  onClick(help, remoteGestureHelp);
  lv_obj_t *done = pill(remoteModes, "Done", 96);
  lv_obj_set_size(done, 96, 36);
  lv_obj_set_pos(done, 128, 226);
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
        [](lv_event_t *e) {
          const auto platform = static_cast<RemotePlatform>(uintptr_t(lv_event_get_user_data(e)));
          if (platform == RemotePlatform::IPhone) hide(remotePhoneOptions, false);
          else selectRemotePlatform(platform);
        },
        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
  }
  remotePhoneOptions = plain(s);
  lv_obj_set_size(remotePhoneOptions, W, H);
  lv_obj_add_flag(remotePhoneOptions, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(remotePhoneOptions, VOID_, 0);
  lv_obj_set_style_bg_opa(remotePhoneOptions, LV_OPA_COVER, 0);
  lv_obj_t *phoneTitle = text(remotePhoneOptions, "iPhone input", F_BODY, INK);
  lv_obj_set_pos(phoneTitle, 22, 25);
  const char *phoneNames[] = {"Keyboard navigation", "Pointer"};
  const char *phoneNotes[] = {"Full Keyboard Access", "AssistiveTouch"};
  for (int i = 0; i < 2; ++i) {
    lv_obj_t *row = pressedFeedback(plain(remotePhoneOptions));
    lv_obj_set_pos(row, 16, 80 + i * 76);
    lv_obj_set_size(row, 208, 64);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_t *label = text(row, phoneNames[i], F_BODY, INK);
    lv_obj_set_pos(label, 14, 10);
    lv_obj_t *note = text(row, phoneNotes[i], F_SMALL, MIST);
    lv_obj_set_pos(note, 14, 38);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        row,
        [](lv_event_t *e) {
          remotePhonePointer = lv_event_get_user_data(e) != nullptr;
          settings.putBool("remote-phoneptr", remotePhonePointer);
          selectRemotePlatform(RemotePlatform::IPhone);
        },
        LV_EVENT_CLICKED, i ? (void *)1 : nullptr);
  }
  lv_obj_t *phoneDone = pill(remotePhoneOptions, "Back", 144);
  lv_obj_set_pos(phoneDone, 48, 224);
  onClick(phoneDone, [] { hide(remotePhoneOptions, true); });
  hide(remotePlatformSheet, true);
  hide(remotePhoneOptions, true);
  refreshRemotePlatform();
  setRemoteMode(remoteMode);
}
