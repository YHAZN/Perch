// Part of device_ui.cpp (included there, one translation unit). Answers: loading, formatting (Markdown-ish to blocks),
// Smart Stack card.
// ---------- content ----------
void loadLatestAnswer() {
  answerCount = listAnswers(answerIds, ANSWER_KEEP);
  lastAnswer = "";
  lastInfo = AnswerInfo();
  if (answerCount) loadAnswer(answerIds[0], lastAnswer, lastInfo);
  historyDirty = true;
}
// ---------- answer formatting ----------
// Models write Markdown-ish text. The answer screen turns it into blocks: the answer in large
// type, numbered steps with a badge, bullets, formulas on a tinted panel, headings in small
// caps, and **key terms** in amber. Maths is shown with real symbols (x², √, π, ≤).
// Every way models write maths (LaTeX in its various delimiters, x^2, Unicode) becomes the
// Unicode the fonts draw; see math_text.h. Applied when shown, so older answers benefit too.
String cleanMath(const String &t) { return String(mathToDisplay(std::string(t.c_str(), t.length())).c_str()); }
String stripMarks(String t) {
  t.replace("**", "");
  t.replace("__", "");
  return t;
}
// "Q2: c", "Question 3a: 12", "Q4) yes": one question's answer. Fills number and answer.
bool questionLine(const String &line, String &number, String &answer) {
  String t = stripMarks(line);
  t.trim();
  String lower = t;
  lower.toLowerCase();
  int i = 0;
  if (lower.startsWith("question")) i = 8;
  else if (lower.startsWith("q")) i = 1;
  else return false;
  while (i < (int)t.length() && t[i] == ' ') ++i;
  const int start = i;
  while (i < (int)t.length() && isdigit((unsigned char)t[i])) ++i;
  if (i == start) return false;
  while (i < (int)t.length() && isalpha((unsigned char)t[i]) && i - start < 4) ++i;  // 3a, 3b
  number = t.substring(start, i);
  if (i >= (int)t.length() || (t[i] != ':' && t[i] != '.' && t[i] != ')')) return false;
  answer = t.substring(i + 1);
  answer.trim();
  return answer.length() > 0;
}
// The one-line answer: the "Answer:" line if there is one, else the first line. For the
// card on the clock face and the Ask history.
String answerHeadline(const String &raw) {
  String body = cleanMath(raw);
  int from = 0;
  String first, answers;
  while (from <= (int)body.length()) {
    int end = body.indexOf('\n', from);
    if (end < 0) end = body.length();
    String line = stripMarks(body.substring(from, end));
    line.trim();
    from = end + 1;
    if (line.isEmpty()) continue;
    String lower = line;
    lower.toLowerCase();
    String number, value;
    if (questionLine(line, number, value)) {
      answers += (answers.length() ? "  " : "") + String("Q") + number + " " + value;
      continue;
    }
    if (lower.startsWith("answer:") || lower.startsWith("final answer:")) {
      value = line.substring(line.indexOf(':') + 1);
      value.trim();
      answers += (answers.length() ? "  " : "") + value;
      continue;
    }
    if (first.isEmpty()) first = line;
  }
  if (answers.length()) return answers;
  while (first.startsWith("#")) first = first.substring(1);
  first.trim();
  return first;
}
// Body text that may contain **bold**: a label, or a span group when there is emphasis.
bool looksLikeFormula(const String &t);
// Large type for an answer that fits on one or two lines; body type when a long formula would
// otherwise break over three or more.
const lv_font_t *headlineFont(const String &t, int width) {
  lv_point_t size;
  lv_text_get_size(&size, stripMarks(t).c_str(), F_LARGE, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (size.x <= width) return F_LARGE;
  if (looksLikeFormula(stripMarks(t))) return F_BODY;  // a formula reads best unbroken
  return size.x <= width * 2 - 20 ? F_LARGE : F_BODY;
}
lv_obj_t *flowText(lv_obj_t *parent, const String &line, const lv_font_t *font, lv_color_t color, int width) {
  if (line.indexOf("**") < 0) {
    lv_obj_t *l = text(parent, line.c_str(), font, color);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(l, 4, 0);
    return l;
  }
  lv_obj_t *sg = lv_spangroup_create(parent);
  lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_width(sg, width);
  lv_obj_set_height(sg, LV_SIZE_CONTENT);
  lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
  lv_obj_set_style_text_font(sg, font, 0);
  lv_obj_set_style_text_color(sg, color, 0);
  lv_obj_set_style_text_line_space(sg, 4, 0);
  int from = 0;
  bool bold = false;
  while (true) {
    const int at = line.indexOf("**", from);
    const String seg = line.substring(from, at < 0 ? line.length() : at);
    if (seg.length()) {
      lv_span_t *sp = lv_spangroup_new_span(sg);
      lv_span_set_text(sp, seg.c_str());
      if (bold) lv_style_set_text_color(lv_span_get_style(sp), LENS);
    }
    if (at < 0) break;
    bold = !bold;
    from = at + 2;
  }
  lv_spangroup_refr_mode(sg);
  return sg;
}
// A badge (number) or dot beside wrapped text, with a hanging indent.
lv_obj_t *markedRow(const char *mark, const String &rest) {
  lv_obj_t *r = plain(answerFlow);
  lv_obj_set_size(r, W - 40, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(r, 10, 0);
  if (mark) {
    lv_obj_t *badge = circle(r, 24, LINE, 0, GRAPHITE, LV_OPA_COVER);
    lv_obj_center(text(badge, mark, F_SMALL, INK));
  } else {
    lv_obj_t *dot = circle(r, 6, MIST, 0, MIST, LV_OPA_COVER);
    lv_obj_set_style_margin_top(dot, 8, 0);
    lv_obj_set_style_margin_left(dot, 9, 0);
    lv_obj_set_style_margin_right(dot, 9, 0);
  }
  lv_obj_t *t = flowText(r, rest, F_BODY, INK, W - 40 - 34);
  if (mark) lv_obj_set_style_margin_top(t, 2, 0);
  return r;
}
lv_obj_t *panel(const String &content) {
  lv_obj_t *b = plain(answerFlow);
  lv_obj_set_size(b, W - 40, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(b, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_pad_all(b, 10, 0);
  flowText(b, content, F_BODY, INK, W - 60);
  return b;
}
void heading(const String &t) {
  String h = stripMarks(t);
  while (h.startsWith("#")) h = h.substring(1);
  if (h.endsWith(":")) h = h.substring(0, h.length() - 1);
  h.trim();
  h.toUpperCase();
  lv_obj_t *l = text(answerFlow, h.c_str(), F_SMALL, MIST);
  lv_obj_set_style_text_letter_space(l, 1, 0);
  lv_obj_set_style_margin_top(l, 6, 0);
}
// Mostly symbols and numbers, few words: show it as a formula.
bool looksLikeFormula(const String &t) {
  if (t.length() > 70) return false;
  bool maths = t.indexOf('=') >= 0 || t.indexOf('^') >= 0;
  for (const char *sym : {"√", "≤", "≥", "×", "∫", "∑", "≠", "≈", "²", "³"})
    if (t.indexOf(sym) >= 0) maths = true;
  if (!maths) return false;
  int words = 0, run = 0;
  for (size_t i = 0; i <= t.length(); ++i) {
    const char c = i < t.length() ? t[i] : ' ';
    if (isalpha((unsigned char)c)) ++run;
    else {
      if (run > 3) ++words;
      run = 0;
    }
  }
  return words <= 2;
}
void renderAnswer(const String &raw) {
  lv_obj_clean(answerFlow);
  String body = cleanMath(raw);
  body.replace("\r", "");
  bool sawAnswer = false, firstBlock = true, inCode = false;
  String code;
  int from = 0;
  while (from <= (int)body.length()) {
    int end = body.indexOf('\n', from);
    if (end < 0) end = body.length();
    const String line = body.substring(from, end);
    from = end + 1;
    String t = line;
    t.trim();
    if (t.startsWith("```")) {
      if (inCode && code.length()) panel(code);
      code = "";
      inCode = !inCode;
      continue;
    }
    if (inCode) {
      code += (code.length() ? "\n" : "") + line;
      continue;
    }
    if (t.isEmpty() || t == "---" || t == "***") continue;
    String lower = stripMarks(t);
    lower.toLowerCase();
    // A question's answer (pages often have several): a label with its number, then large type.
    String qNumber, qAnswer;
    if (questionLine(t, qNumber, qAnswer)) {
      lv_obj_t *label = text(answerFlow, (String("QUESTION ") + qNumber).c_str(), F_SMALL, LENS);
      lv_obj_set_style_text_letter_space(label, 1, 0);
      if (sawAnswer) lv_obj_set_style_margin_top(label, 10, 0);
      flowText(answerFlow, qAnswer, headlineFont(qAnswer, W - 40), INK, W - 40);
      sawAnswer = true;
      firstBlock = false;
      continue;
    }
    // The answer itself: large, under a small label (every one, not only the first).
    if (lower.startsWith("answer:") || lower.startsWith("final answer:")) {
      String value = stripMarks(t);
      value = value.substring(value.indexOf(':') + 1);
      value.trim();
      lv_obj_t *label = text(answerFlow, "ANSWER", F_SMALL, LENS);
      lv_obj_set_style_text_letter_space(label, 1, 0);
      flowText(answerFlow, value, headlineFont(value, W - 40), INK, W - 40);
      sawAnswer = true;
      firstBlock = false;
      continue;
    }
    if (t.startsWith("#") || (t.endsWith(":") && t.length() < 34 && !isdigit((unsigned char)t[0]))) {
      heading(t);
      firstBlock = false;
      continue;
    }
    // "1." / "1)" steps
    int digits = 0;
    while (digits < (int)t.length() && digits < 3 && isdigit((unsigned char)t[digits])) ++digits;
    if (digits && digits + 1 < (int)t.length() && (t[digits] == '.' || t[digits] == ')') && t[digits + 1] == ' ') {
      String rest = t.substring(digits + 2);
      rest.trim();
      markedRow(t.substring(0, digits).c_str(), rest);
      firstBlock = false;
      continue;
    }
    if ((t.startsWith("- ") || t.startsWith("* ") || t.startsWith("+ ")) && t.length() > 2) {
      String rest = t.substring(2);
      rest.trim();
      markedRow(nullptr, rest);
      firstBlock = false;
      continue;
    }
    if (t.startsWith("|")) {
      if (t.indexOf("---") >= 0) continue;  // table rule
      String cells = t.substring(1, t.endsWith("|") ? t.length() - 1 : t.length());
      cells.replace("|", "  -  ");
      cells.trim();
      t = cells;
    }
    if (t.startsWith("[Answer cut off]")) {
      text(answerFlow, "The answer was cut off.", F_SMALL, MIST);
      continue;
    }
    if (looksLikeFormula(stripMarks(t))) {
      panel(t);
      firstBlock = false;
      continue;
    }
    // Without an "Answer:" line, a short first sentence still leads in large type.
    if (firstBlock && !sawAnswer && t.length() <= 120) {
      flowText(answerFlow, t, headlineFont(t, W - 40), INK, W - 40);
      firstBlock = false;
      continue;
    }
    flowText(answerFlow, t, F_BODY, INK, W - 40);
    firstBlock = false;
  }
  if (inCode && code.length()) panel(code);
}

// Streaming: the answer screen opens with the first words and grows as the model writes.
void showStreaming(const String &text, bool first) {
  renderAnswer(text);
  if (first) {
    setText(answerMeta, "Writing...");
    hide(answerPhoto, true);
    lv_obj_scroll_to_y(answerScroll, 0, LV_ANIM_OFF);
    show(Screen::Answer);
  }
}
void showAnswer(uint32_t id) {
  String body;
  AnswerInfo info;
  if (!loadAnswer(id, body, info)) {
    notice("That answer could not be read.", Screen::Ask);
    return;
  }
  renderAnswer(body);
  String meta = String("Answered by ") + (info.gemini ? "Gemini" : "GPT");
  const String when = ago(info.when);
  if (when.length()) meta += ", " + when;
  lv_label_set_text(answerMeta, meta.c_str());
  const bool photo = info.photoId && loadPhotoInto(info.photoId, galleryPixels);
  if (photo) lv_image_cache_drop(&galleryDsc);
  hide(answerPhoto, !photo);
  lv_obj_scroll_to_y(answerScroll, 0, LV_ANIM_OFF);
  show(Screen::Answer);
}

// Smart Stack: glanceable cards; swipe the card to cycle, tap to open.
enum class Card { Answer, Photo, Offline, Queue };
std::vector<Card> cards;
int cardIndex = 0;
String cardSignature = "";
void renderCard() {
  const bool online = networkConnected();
  // Rebuild only when something the card shows has changed.
  const String signature = String(online) + networkEnabled() + "|" + lastInfo.id + "|" + latestPhotoId + "|" +
                           jpegBytes + "|" + cardIndex + "|" + ago(lastInfo.when) + "|" + queuedCount;
  if (signature == cardSignature) return;
  cardSignature = signature;
  cards.clear();
  if (queuedCount) cards.push_back(Card::Queue);
  if (!online) cards.push_back(Card::Offline);
  if (!lastAnswer.isEmpty()) cards.push_back(Card::Answer);
  if (jpegBytes) cards.push_back(Card::Photo);
  if (cards.empty()) cards.push_back(Card::Answer);
  cardIndex = constrain(cardIndex, 0, (int)cards.size() - 1);
  const Card c = cards[cardIndex];
  const bool photo = c == Card::Photo;
  hide(cardImage, !photo);
  hide(cardScrim, !photo);
  hide(cardValue, photo);
  hide(cardRing, c != Card::Answer);
  if (c == Card::Queue) {
    setText(cardKey, online ? "Asking" : "Waiting for Wi-Fi");
    setText(cardValue, (String(queuedCount) + (queuedCount == 1 ? " question" : " questions") +
                        (online ? " being answered." : " will be asked when Wi-Fi is back."))
                           .c_str());
  } else if (c == Card::Offline) {
    setText(cardKey, "Wi-Fi");
    setText(cardValue, networkEnabled() ? "Not connected. Camera and Photos work offline."
                                        : "Wi-Fi is off. Turn it on in Control Center.");
  } else if (c == Card::Photo) {
    setText(cardKey, "Last photo");
  } else if (!lastAnswer.isEmpty()) {
    const String when = ago(lastInfo.when);
    setText(cardKey, when.length() ? ("Last answer, " + when).c_str() : "Last answer");
    setText(cardValue, answerHeadline(lastAnswer).c_str());
  } else {
    setText(cardKey, "Ask");
    setText(cardValue, "Point at a question, then open Ask.");
  }
  lv_obj_set_style_text_color(cardKey, photo ? INK : MIST, 0);
  lv_obj_align(cardKey, photo ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_TOP_LEFT, c == Card::Answer ? 16 : 0, 0);
  // Dots on the right edge: one per card, the current one longer.
  lv_obj_clean(cardDots);
  hide(cardDots, cards.size() < 2);
  for (size_t i = 0; i < cards.size(); ++i) {
    lv_obj_t *d = plain(cardDots);
    lv_obj_set_size(d, 5, (int)i == cardIndex ? 12 : 5);
    lv_obj_set_style_radius(d, 3, 0);
    lv_obj_set_style_bg_color(d, INK, 0);
    lv_obj_set_style_bg_opa(d, (int)i == cardIndex ? LV_OPA_COVER : LV_OPA_30, 0);
  }
}
void cycleCard(int step) {
  const int next = constrain(cardIndex + step, 0, (int)cards.size() - 1);
  if (next == cardIndex) return;
  cardIndex = next;
  renderCard();
  // The card is bottom-aligned; its y is an offset from that anchor.
  animY(card, -ABOVE_HOME + (step > 0 ? 18 : -18), -ABOVE_HOME, 200, lv_anim_path_ease_out, nullptr);
}

// Cached so the UI never waits on flash: key presence changes only via 'K'; free space is
// measured on the save task (the query walks the whole filesystem, ~175 ms).
int keyCache = -1;  // bit 0 = Gemini key, bit 1 = GPT key; -1 = unknown
volatile uint32_t freeKbCache = 0;
volatile bool freeKbStale = false;
String ccSignature;
void refreshDynamic() {
  const bool online = networkConnected();
  const time_t now = time(nullptr);
  if (clockKnown()) {
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M", &t);
    setText(clockLabel, buf[0] == '0' ? buf + 1 : buf);
    strftime(buf, sizeof(buf), "%a %b %e", &t);
    setText(dateLabel, buf);
  } else {
    setText(clockLabel, "");
    setText(dateLabel, "");
  }
  {
    const lv_color_t wifiColor = online ? INK : MIST;
    if (!lv_color_eq(lv_obj_get_style_text_color(faceWifi, 0), wifiColor))
      lv_obj_set_style_text_color(faceWifi, wifiColor, 0);
    const lv_color_t btColor = remoteConnected() ? INK : MIST;
    if (!lv_color_eq(lv_obj_get_style_text_color(faceBt, 0), btColor)) lv_obj_set_style_text_color(faceBt, btColor, 0);
    hide(faceBt, !bluetoothOn);
    const int battery = batteryPercent();
    hide(faceBattery, battery < 0);
    if (battery >= 0) {
      setText(faceBattery, (String(battery) + "%").c_str());
      const lv_color_t c = battery <= 15 ? lv_color_hex(0xFF5A4E) : INK;
      if (!lv_color_eq(lv_obj_get_style_text_color(faceBattery, 0), c)) lv_obj_set_style_text_color(faceBattery, c, 0);
    }
  }
  renderCard();
  const bool photo = jpegBytes > 0;
  hide(thumb, !photo || cameraOff);
  hide(pageBanner, !pageCamera);
  refreshChat();
  // Camera privacy
  hide(viewfinder, cameraOff);
  hide(shutter, cameraOff);
  hide(cameraOffLabel, !cameraOff && !cameraFailed);
  setText(cameraOffLabel,
          cameraOff ? "Camera is off.\nTurn it on in Control Center." : "The camera did not start.\nTrying again...");
  // Settings and Model
  setText(modelValue, useGemini ? GEMINI_MODELS[constrain(geminiModel, 0, GEMINI_MODEL_COUNT - 1)].label
                                : GPT_MODELS[constrain(gptModel, 0, GPT_MODEL_COUNT - 1)].label);
  setText(wifiValue, !networkEnabled() ? "Off" : online ? networkName().c_str() : "Not connected");
  // Wi-Fi list: show results when a scan finishes; rescan every 20 s while it is open.
  if (current == Screen::Wifi) {
    renderScan();
    if (scanShown && millis() - scanStartedAt > 20000) startScan();
  }
  if (joiningSsid.length() && networkName() == joiningSsid) {
    toast(("Connected to " + joiningSsid).c_str());
    joiningSsid = "";
    if (current == Screen::Wifi) startScan();
  }
  if (keyCache < 0) keyCache = (settings.isKey("gemini-key") ? 1 : 0) | (settings.isKey("gpt-key") ? 2 : 0);
  if (current == Screen::Model) refreshAiScreen();
  {
    const uint16_t secs = SLEEP_CHOICES[sleepChoice];
    setText(sleepValue, !secs       ? "Never"
                        : secs < 60 ? (String(secs) + " s").c_str()
                                    : (String(secs / 60) + " min").c_str());
  }
  {
    String line = String("Perch ") + PERCH_VERSION + ", built " + __DATE__;
    switch (updaterState()) {
      case UpdateState::Checking: line = "Checking for updates..."; break;
      case UpdateState::UpToDate: line = String("Perch ") + PERCH_VERSION + " is up to date"; break;
      case UpdateState::Available:
        line = String("Perch ") + updaterAvailableVersion() + " available: tap to install";
        break;
      case UpdateState::Installing: line = String("Installing ") + updaterProgress() + "%..."; break;
      case UpdateState::Ready: line = "Installed: tap to restart"; break;
      case UpdateState::Failed: line = updaterError(); break;
      default: break;
    }
    setText(softwareSub, line.c_str());
  }
  setText(storageValue, freeKbCache ? (String(freeKbCache) + " KB free").c_str() : "");
  setText(storageSub, (String(photoCount) + " photos, " + answerCount + " answers on the device").c_str());
  setText(remoteStatus, !remoteStarted()    ? ""
                        : remoteConnected() ? "Connected"
                                            : "Pair \"Perch\" in your computer's Bluetooth");
  lv_obj_set_style_opa(remoteSlides, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_set_style_opa(remoteMediaPanel, remoteConnected() ? LV_OPA_COVER : LV_OPA_40, 0);
  // Control Center: restyle only when a toggle actually changed.
  const String cc =
      String(networkEnabled()) + online + cameraOff + useGemini + bluetoothOn + remoteConnected() + btPaused;
  if (cc != ccSignature) {
    ccSignature = cc;
    // On: amber disc, dark glyph. Off: dark disc, light glyph. The label always says which.
    auto toggle = [](lv_obj_t *t, bool on) {
      lv_obj_set_style_bg_color(t, on ? LENS : ICON_BG, 0);
      lv_obj_t *glyph = lv_obj_get_child(t, 0);
      if (glyph) lv_obj_set_style_text_color(glyph, on ? VOID_ : INK, 0);
    };
    toggle(ccWifi, networkEnabled());
    setText(ccWifiState, !networkEnabled() ? "Off" : online ? "On" : "No net");
    toggle(ccBt, bluetoothOn);
    setText(ccBtState, !bluetoothOn ? "Off" : btPaused ? "Paused" : remoteConnected() ? "Linked" : "On");
    toggle(ccCamera, !cameraOff);
    setText(lv_obj_get_child(ccCamera, 0), cameraOff ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    setText(ccCameraState, cameraOff ? "Off" : "On");
    toggle(ccModel, false);
    setText(lv_obj_get_child(ccModel, 0), useGemini ? "G" : "GPT");
    setText(ccModelState, useGemini ? "Gemini" : "GPT");
  }
}
