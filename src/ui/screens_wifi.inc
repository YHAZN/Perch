// Part of device_ui.cpp (included there, one translation unit). Screens: Wi-Fi list, password and question keyboard.
// ---------- Wi-Fi: choose a network and type its password on the device ----------
void startScan() {
  networkScanStart();
  scanShown = false;
  scanStartedAt = millis();
  setText(wifiStatus, networkConnected() ? ("Connected to " + networkName()).c_str() : "Searching...");
}
void joinNetwork(const String &ssid, const String &password) {
  networkSave(ssid, password);
  joiningSsid = ssid;
  toast(("Joining " + ssid).c_str());
}
void renderScan() {
  static ScanResult results[12];
  const int n = networkScanResults(results, 12);
  if (n < 0 || scanShown) return;
  scanShown = true;
  lv_obj_clean(wifiList);
  const String current = networkName();
  setText(wifiStatus, current.length() ? ("Connected to " + current).c_str()
                      : n              ? "Choose a network"
                                       : "No networks found");
  for (int i = 0; i < n; ++i) {
    lv_obj_t *r = pressedFeedback(plain(wifiList));
    lv_obj_set_size(r, W, 50);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, LINE, 0);
    lv_obj_t *name = text(r, results[i].ssid.c_str(), F_BODY, INK);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, 160, 20);
    lv_obj_set_pos(name, 22, 7);
    const bool connected = results[i].ssid == current, saved = networkIsSaved(results[i].ssid);
    const char *note = connected ? "Connected" : saved ? "Saved" : results[i].secured ? "" : "Open";
    lv_obj_t *sub = text(r, note, F_SMALL, connected ? LENS : MIST);
    lv_obj_set_pos(sub, 22, 29);
    // Signal: the Wi-Fi glyph, brighter when stronger.
    lv_obj_t *bars = text(r, LV_SYMBOL_WIFI, F_BODY, INK);
    lv_obj_set_style_text_opa(bars,
                              results[i].rssi > -60   ? LV_OPA_COVER
                              : results[i].rssi > -75 ? LV_OPA_70
                                                      : LV_OPA_40,
                              0);
    lv_obj_align(bars, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    // The row keeps its SSID and security in a small heap record for the callbacks.
    struct Row {
      String ssid;
      bool secured;
    };
    Row *row = new Row{results[i].ssid, results[i].secured};
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          Row *row = (Row *)lv_event_get_user_data(e);
          if (!row->secured || networkIsSaved(row->ssid)) {
            joinNetwork(row->ssid, "");
            return;
          }
          joiningSsid = row->ssid;
          setText(passwordTitle, row->ssid.c_str());
          textMode = TextMode::Password;
          lv_textarea_set_password_mode(passwordField, true);
          lv_textarea_set_placeholder_text(passwordField, "Password");
          hide(passwordEye, false);
          lv_obj_set_width(passwordField, 166);
          lv_textarea_set_text(passwordField, "");
          show(Screen::Password);
        },
        LV_EVENT_CLICKED, row);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          Row *row = (Row *)lv_event_get_user_data(e);
          if (!networkIsSaved(row->ssid)) return;
          networkForget(row->ssid);
          toast(("Forgot " + row->ssid).c_str());
          startScan();
        },
        LV_EVENT_LONG_PRESSED, row);
    lv_obj_add_event_cb(r, [](lv_event_t *e) { delete (Row *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, row);
  }
}
void buildWifi() {
  lv_obj_t *s = scr[(int)Screen::Wifi] = screenBase();
  title(s, "Wi-Fi");
  wifiStatus = text(s, "", F_SMALL, MIST);
  lv_obj_set_pos(wifiStatus, 22, 58);
  wifiList = plain(s);
  lv_obj_set_size(wifiList, W, H - 80);
  lv_obj_set_pos(wifiList, 0, 80);
  scrollable(wifiList);
  lv_obj_set_flex_flow(wifiList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(wifiList, 56, 0);
}
// Keyboard: three layouts, keys as large as 240 px allows, kept above the home strip.
const char *KB_LOWER[] = {"q",  "w",   "e",    "r", "t",          "y", "u", "i", "o", "p",
                          "\n", "a",   "s",    "d", "f",          "g", "h", "j", "k", "l",
                          "\n", "ABC", "z",    "x", "c",          "v", "b", "n", "m", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  "Talk", " ", LV_SYMBOL_OK, ""};
const char *KB_UPPER[] = {"Q",  "W",   "E",    "R", "T",          "Y", "U", "I", "O", "P",
                          "\n", "A",   "S",    "D", "F",          "G", "H", "J", "K", "L",
                          "\n", "abc", "Z",    "X", "C",          "V", "B", "N", "M", LV_SYMBOL_BACKSPACE,
                          "\n", "1#",  "Talk", " ", LV_SYMBOL_OK, ""};
const char *KB_SPECIAL[] = {
    "1",  "2",   "3",    "4", "5",          "6",  "7", "8", "9", "0", "\n", "-", "_", "/", ":", ";",
    "(",  ")",   "@",    "&", "\"",         "\n", "#", "%", "*", "+", "=",  ".", ",", "?", "!", LV_SYMBOL_BACKSPACE,
    "\n", "abc", "Talk", " ", LV_SYMBOL_OK, ""};
const lv_buttonmatrix_ctrl_t KB_CTRL_LETTERS[] = {(lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(4),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(4),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(6),
                                                  (lv_buttonmatrix_ctrl_t)(3)};
const lv_buttonmatrix_ctrl_t KB_CTRL_SPECIAL[] = {(lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | 2),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(3),
                                                  (lv_buttonmatrix_ctrl_t)(6),
                                                  (lv_buttonmatrix_ctrl_t)(3)};
void buildPassword() {
  lv_obj_t *s = scr[(int)Screen::Password] = screenBase();
  passwordTitle = text(s, "", F_BODY, INK);
  lv_label_set_long_mode(passwordTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_size(passwordTitle, W - 80, 20);
  lv_obj_set_pos(passwordTitle, 58, 22);
  passwordField = lv_textarea_create(s);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Password");
  lv_obj_set_size(passwordField, 166, 36);
  lv_obj_set_pos(passwordField, 14, 54);
  lv_obj_set_style_bg_color(passwordField, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(passwordField, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(passwordField, 0, 0);
  lv_obj_set_style_radius(passwordField, 12, 0);
  lv_obj_set_style_text_color(passwordField, INK, 0);
  lv_obj_set_style_text_font(passwordField, F_BODY, 0);
  lv_obj_set_style_pad_all(passwordField, 8, 0);
  lv_obj_set_style_bg_color(passwordField, LENS, LV_PART_CURSOR);
  lv_obj_set_style_bg_opa(passwordField, LV_OPA_COVER, LV_PART_CURSOR);
  lv_obj_set_style_text_color(passwordField, MIST, LV_PART_TEXTAREA_PLACEHOLDER);
  // Show or hide what was typed.
  lv_obj_t *eye = passwordEye = circle(s, 36, ICON_BG, 0, ICON_BG, LV_OPA_COVER);
  lv_obj_set_pos(eye, 188, 54);
  lv_obj_set_style_opa(eye, LV_OPA_60, LV_STATE_PRESSED);
  static lv_obj_t *eyeGlyph = nullptr;
  eyeGlyph = text(eye, LV_SYMBOL_EYE_OPEN, F_BODY, INK);
  lv_obj_center(eyeGlyph);
  onClick(eye, [] {
    const bool hidden = lv_textarea_get_password_mode(passwordField);
    lv_textarea_set_password_mode(passwordField, !hidden);
    lv_label_set_text(eyeGlyph, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
  });
  keyboard = lv_keyboard_create(s);
  lv_keyboard_set_textarea(keyboard, passwordField);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, KB_LOWER, KB_CTRL_LETTERS);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, KB_UPPER, KB_CTRL_LETTERS);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, KB_SPECIAL, KB_CTRL_SPECIAL);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(keyboard, W - 8, HOME_ZONE - 98);
  // Keyboards are bottom-anchored by default; place it under the field, above the home strip.
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 4, 96);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(keyboard, 0, 0);
  lv_obj_set_style_pad_gap(keyboard, 3, 0);
  lv_obj_set_style_border_width(keyboard, 0, 0);
  lv_obj_set_style_bg_color(keyboard, ICON_BG, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 7, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, INK, LV_PART_ITEMS);
  lv_obj_set_style_text_font(keyboard, F_BODY, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, INK, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(keyboard, VOID_, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(keyboard, GRAPHITE, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *) {
        const String typed = lv_textarea_get_text(passwordField);
        if (textMode == TextMode::Question) {
          String q = typed;
          q.trim();
          if (q.isEmpty() && attachIds.empty()) {
            toast("Type a message, or hold Talk");
            return;
          }
          lv_textarea_set_text(passwordField, "");
          textMode = TextMode::Password;  // so returning to Ask does not read the field again
          draft = q;
          goBack();
          sendMessage(draft);
          return;
        }
        if (typed.length() < 8) {
          toast("Wi-Fi passwords have at least 8 characters");
          return;
        }
        joinNetwork(joiningSsid, typed);
        lv_textarea_set_text(passwordField, "");
        goBack();
      },
      LV_EVENT_READY, nullptr);
  // Talk: hold the key and speak; the words are typed into the field.
  lv_obj_remove_event_cb(keyboard, lv_keyboard_def_event_cb);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
        const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
        const char *label = id == LV_BUTTONMATRIX_BUTTON_NONE ? nullptr : lv_buttonmatrix_get_button_text(kb, id);
        if (label && strcmp(label, "Talk") == 0) return;  // handled on press / release
        lv_keyboard_def_event_cb(e);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(
      keyboard,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
        const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
        const char *label = id == LV_BUTTONMATRIX_BUTTON_NONE ? nullptr : lv_buttonmatrix_get_button_text(kb, id);
        if (!label || strcmp(label, "Talk") != 0 || micRecording() || aiBusy()) return;
        if (!networkConnected()) {
          toast("Talk needs Wi-Fi");
          return;
        }
        if (micStart()) {
          dictating = true;
          toast("Listening. Let go when done");
        }
      },
      LV_EVENT_PRESSED, nullptr);
  auto talkUp = [](lv_event_t *) {
    if (!dictating || !micRecording()) return;
    micStop();
    size_t len = 0;
    const uint8_t *wav = micWav(len);
    if (micSeconds() < 0.6f) {
      dictating = false;
      toast("Hold Talk while you speak");
      return;
    }
    AiOptions options = aiOptions(useGemini);
    pauseBluetooth();
    const bool started = aiTranscribe(options, wav, len);
    options.key = "";
    if (!started) {
      dictating = false;
      toast("Could not start");
      return;
    }
    pending = Pending::Transcribe;
    toast("Writing it down");
  };
  lv_obj_add_event_cb(keyboard, talkUp, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(keyboard, talkUp, LV_EVENT_PRESS_LOST, nullptr);
}
