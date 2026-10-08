// Part of device_ui.cpp (included there, one translation unit). The Ask app: chat sessions.
// ---------- Ask: a conversation ----------
// The chat starts blank. Photos join a message only when the user adds them with "+" (take one
// now, or choose from Photos); the Camera app is independent of it. Each send carries the
// chat's own photos (the newest AI_MAX_PAGES) and its earlier messages as context. Chats are
// saved to flash and can be reopened from the Chats list.
struct ChatMsg {
  bool user = true;
  String text;
  std::vector<uint32_t> photos;
  bool pending = false;  // user: waiting for Wi-Fi
  bool failed = false;   // assistant: the request failed; `text` says why
  bool voice = false;    // user: spoken question sent as audio
};
std::vector<ChatMsg> chat;
uint32_t chatId = 0;
uint32_t requestChatId = 0;       // the chat the running request belongs to (it may not be on screen)
std::vector<uint32_t> attachIds;  // photos for the next message
int attachAwaiting = 0;           // photos taken for the chat that are still being saved
uint8_t *chatWav = nullptr;       // a spoken question kept until it is sent
size_t chatWavLen = 0;
// The composer sits just above the home strip (only its middle starts the home swipe).
constexpr int COMPOSER_Y = HOME_ZONE - 48;
lv_obj_t *chatScroll, *chatEmpty, *chatTitle, *attachRow, *composerText, *sendBtn, *stopBtn, *plusSheet;
lv_obj_t *assistantLive = nullptr;
std::vector<void *> chatImages;  // thumbnails and their descriptors, freed when the chat is redrawn

void dispatchChat();
void rebuildChat();

lv_obj_t *photoThumb(lv_obj_t *parent, uint32_t id, int size) {
  uint16_t *pixels = (uint16_t *)ps_malloc(THUMB * THUMB * 2);
  lv_image_dsc_t *dsc = (lv_image_dsc_t *)ps_calloc(1, sizeof(lv_image_dsc_t));
  if (!pixels || !dsc || !loadPhotoThumb(id, pixels)) {
    free(pixels);
    free(dsc);
    lv_obj_t *gone = plain(parent);  // the photo was deleted or never saved
    lv_obj_set_size(gone, size, size);
    lv_obj_set_style_radius(gone, 8, 0);
    lv_obj_set_style_bg_color(gone, LINE, 0);
    lv_obj_set_style_bg_opa(gone, LV_OPA_COVER, 0);
    return gone;
  }
  chatImages.push_back(pixels);
  chatImages.push_back(dsc);
  setupImage(*dsc, pixels, THUMB, THUMB);
  lv_obj_t *img = lv_image_create(parent);
  lv_image_set_src(img, dsc);
  lv_obj_set_size(img, size, size);
  lv_image_set_scale(img, 256 * size / THUMB);
  lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_style_radius(img, 8, 0);
  lv_obj_set_style_clip_corner(img, true, 0);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  return img;
}
void freeChatImages() {
  for (void *p : chatImages) free(p);
  chatImages.clear();
}

String chatTitleText() {
  for (const ChatMsg &m : chat)
    if (m.user) {
      if (m.text.length()) return m.text.length() > 28 ? m.text.substring(0, 27) + "..." : m.text;
      return m.voice ? "Spoken question" : "Photo question";
    }
  return "New chat";
}

// ---------- persistence ----------
void saveChat() {
  if (chat.empty()) return;
  if (!chatId) chatId = chatNewId();
  JsonDocument doc;
  doc["title"] = chatTitleText();
  doc["when"] = clockKnown() ? (uint32_t)time(nullptr) : 0;
  JsonArray list = doc["m"].to<JsonArray>();
  for (const ChatMsg &m : chat) {
    JsonObject o = list.add<JsonObject>();
    o["u"] = m.user;
    o["t"] = m.text;
    if (m.pending) o["w"] = 1;
    if (m.failed) o["f"] = 1;
    if (m.voice) o["v"] = 1;
    JsonArray p = o["p"].to<JsonArray>();
    for (uint32_t id : m.photos) p.add(id);
  }
  String json;
  serializeJson(doc, json);
  chatSave(chatId, json);
}
bool loadChat(uint32_t id) {
  String json;
  JsonDocument doc;
  if (!chatLoad(id, json) || deserializeJson(doc, json)) return false;
  chat.clear();
  for (JsonObject o : doc["m"].as<JsonArray>()) {
    ChatMsg m;
    m.user = o["u"] | true;
    m.text = (const char *)(o["t"] | "");
    m.pending = o["w"] | 0;
    m.failed = o["f"] | 0;
    m.voice = o["v"] | 0;
    for (uint32_t p : o["p"].as<JsonArray>()) m.photos.push_back(p);
    chat.push_back(m);
  }
  // An answer that was being written when the device restarted is gone; say so. (If it is
  // still on its way, the reply will fill it in.)
  const bool live = aiBusy() && pending == Pending::Answer && requestChatId == id;
  if (!live && !chat.empty() && !chat.back().user && chat.back().text.isEmpty()) {
    chat.back().text = "This answer was interrupted.";
    chat.back().failed = true;
  }
  chatId = id;
  attachIds.clear();
  draft = "";
  return true;
}

// ---------- drawing ----------
// Text width for a bubble that hugs short messages and wraps long ones.
int bubbleWidth(const String &text, int max) {
  lv_point_t size;
  lv_text_get_size(&size, text.c_str(), F_BODY, 0, 4, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return min((int)size.x + 2, max);
}
void renderInto(lv_obj_t *box, const String &text) {
  lv_obj_t *saved = answerFlow;
  answerFlow = box;
  renderAnswer(text);
  answerFlow = saved;
}
lv_obj_t *drawMessage(const ChatMsg &m, int index) {
  if (m.user) {
    lv_obj_t *row = plain(chatScroll);
    lv_obj_set_size(row, W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_row(row, 4, 0);
    lv_obj_t *bubble = plain(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(bubble, 8, 0);
    if (!m.photos.empty()) {
      lv_obj_t *photos = plain(bubble);
      lv_obj_set_size(photos, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(photos, LV_FLEX_FLOW_ROW);
      lv_obj_set_style_pad_column(photos, 6, 0);
      for (uint32_t id : m.photos) photoThumb(photos, id, 52);
    }
    const String shown = m.text.length() ? m.text : m.voice ? String("(spoken question)") : String("");
    if (shown.length()) {
      lv_obj_t *l = text(bubble, shown.c_str(), F_BODY, m.text.length() ? INK : MIST);
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(l, bubbleWidth(shown, W - 84));
      lv_obj_set_style_text_line_space(l, 4, 0);
    }
    if (m.pending) text(row, "Waiting for Wi-Fi", F_SMALL, MIST);
    return row;
  }
  lv_obj_t *box = plain(chatScroll);
  lv_obj_set_size(box, W, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(box, 20, 0);
  lv_obj_set_style_pad_row(box, 12, 0);
  if (m.text.isEmpty()) {
    // Typing bubble: three dots pulse in turn until the first words arrive.
    lv_obj_t *bubble = plain(box);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_pad_hor(bubble, 14, 0);
    lv_obj_set_style_pad_ver(bubble, 12, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bubble, 6, 0);
    for (int i = 0; i < 3; ++i) {
      lv_obj_t *dot = circle(bubble, 8, MIST, 0, MIST, LV_OPA_COVER);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, dot);
      lv_anim_set_values(&a, 70, 255);
      lv_anim_set_duration(&a, 380);
      lv_anim_set_playback_duration(&a, 380);
      lv_anim_set_delay(&a, i * 180);
      lv_anim_set_repeat_delay(&a, 300);
      lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
      lv_anim_set_exec_cb(&a, [](void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, v, 0); });
      lv_anim_start(&a);
    }
  } else if (m.failed) {
    lv_obj_t *l = text(box, m.text.c_str(), F_SMALL, MIST);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, W - 40);
    if (index == (int)chat.size() - 1) {
      lv_obj_t *again = chip(box, "Try again");
      onClick(again, [] {
        if (chat.empty() || chat.back().user) return;
        chat.pop_back();  // drop the error; the question is sent again
        rebuildChat();
        dispatchChat();
      });
    }
  } else {
    renderInto(box, m.text);
  }
  return box;
}
void scrollChatToEnd(bool animate) {
  lv_obj_update_layout(chatScroll);
  lv_obj_scroll_to_y(chatScroll, LV_COORD_MAX, animate ? LV_ANIM_ON : LV_ANIM_OFF);
}
void refreshAttachments() {
  lv_obj_clean(attachRow);
  for (size_t i = 0; i < attachIds.size(); ++i) {
    lv_obj_t *cell = plain(attachRow);
    lv_obj_set_size(cell, 44, 44);
    photoThumb(cell, attachIds[i], 44);
    lv_obj_t *x = circle(cell, 18, INK, 0, VOID_, 200);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 2, -2);
    lv_obj_center(text(x, LV_SYMBOL_CLOSE, F_SMALL, INK));
    // Tap a photo to take it out of the message.
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        cell,
        [](lv_event_t *e) {
          const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
          for (auto it = attachIds.begin(); it != attachIds.end(); ++it)
            if (*it == id) {
              attachIds.erase(it);
              break;
            }
          refreshAttachments();
          refreshChat();
        },
        LV_EVENT_CLICKED, (void *)(uintptr_t)attachIds[i]);
  }
  for (int i = 0; i < attachAwaiting; ++i) {
    lv_obj_t *cell = plain(attachRow);
    lv_obj_set_size(cell, 44, 44);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, GRAPHITE, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_center(text(cell, "...", F_SMALL, MIST));
  }
}
void rebuildChat() {
  lv_obj_clean(chatScroll);
  freeChatImages();
  assistantLive = nullptr;
  for (size_t i = 0; i < chat.size(); ++i) {
    lv_obj_t *o = drawMessage(chat[i], i);
    if (!chat[i].user && i == chat.size() - 1 && chat[i].text.isEmpty()) assistantLive = o;
  }
  refreshAttachments();
  refreshChat();
  scrollChatToEnd(false);
}
// Composer state: Send when there is something to send, Stop while answering, else the mic.
void refreshChat() {
  if (!chatScroll) return;
  const bool answering = aiBusy() && pending == Pending::Answer && requestChatId == chatId;
  const bool something = draft.length() || !attachIds.empty();
  hide(chatEmpty, !chat.empty() || !attachIds.empty() || attachAwaiting);
  const bool photosWaiting = !attachIds.empty() || attachAwaiting;
  hide(attachRow, !photosWaiting);
  const int pad = H - COMPOSER_Y + 8 + (photosWaiting ? 52 : 0);
  if (lv_obj_get_style_pad_bottom(chatScroll, 0) != pad) lv_obj_set_style_pad_bottom(chatScroll, pad, 0);
  hide(stopBtn, !answering);
  hide(sendBtn, answering || !something);
  hide(micBtn, answering || something);
  setText(composerText, draft.length() ? draft.c_str() : "Message");
  const lv_color_t c = draft.length() ? INK : MIST;
  if (!lv_color_eq(lv_obj_get_style_text_color(composerText, 0), c)) lv_obj_set_style_text_color(composerText, c, 0);
  hide(newChatBtn, chat.empty() && attachIds.empty());
}

// ---------- sending ----------
void sendMessage(String words, const uint8_t *wav = nullptr, size_t wavLength = 0) {
  words.trim();
  if (words.isEmpty() && attachIds.empty() && !wav) {
    toast("Type, speak or add a photo");
    return;
  }
  if (attachAwaiting) {
    toast("A photo is still saving. Send again in a moment");
    return;
  }
  if (aiBusy()) {
    toast("Still answering the last message");
    return;
  }
  ChatMsg m;
  m.text = words;
  m.photos = attachIds;
  m.voice = wav && wavLength;
  free(chatWav);
  chatWav = nullptr;
  chatWavLen = 0;
  if (m.voice && (chatWav = (uint8_t *)ps_malloc(wavLength))) {
    memcpy(chatWav, wav, wavLength);
    chatWavLen = wavLength;
  }
  chat.push_back(m);
  attachIds.clear();
  draft = "";
  drawMessage(chat.back(), chat.size() - 1);
  refreshAttachments();
  saveChat();
  dispatchChat();
}
// Ask the provider about the last user message, with the chat as context.
void dispatchChat() {
  if (chat.empty() || !chat.back().user) return;
  ChatMsg &q = chat.back();
  if (!networkConnected()) {
    if (!q.pending) {
      q.pending = true;
      saveChat();
      rebuildChat();
    }
    toast("No Wi-Fi: it will send when Wi-Fi is back");
    return;
  }
  if (aiBusy()) return;
  // The conversation as turns, as chat apps send it to a stateless API: every message again,
  // newest last. Only the newest AI_MAX_PAGES photos travel (older ones become "(photo)").
  // Failed replies are left out; same-role neighbours are merged (the API wants alternation).
  std::vector<uint32_t> photos;
  for (const ChatMsg &m : chat)
    for (uint32_t id : m.photos) photos.push_back(id);
  const int skip = max(0, (int)photos.size() - AI_MAX_PAGES);  // photos too old to attach
  std::vector<AiTurn> turns;
  std::vector<uint32_t> sent;
  int seen = 0;
  const int from = max(0, (int)chat.size() - 12);  // the last 12 messages are plenty of context
  for (int i = 0; i < (int)chat.size(); ++i) {
    const ChatMsg &m = chat[i];
    int attached = 0;
    String note;
    for (uint32_t id : m.photos) {
      if (seen++ >= skip) {
        sent.push_back(id);
        ++attached;
      } else note += "(earlier photo) ";
    }
    if (i < from && !attached) continue;
    if (!m.user && (m.failed || m.text.isEmpty())) continue;
    String t = note + m.text;
    t.trim();
    if (!turns.empty() && turns.back().user == m.user) {
      turns.back().text += (turns.back().text.length() && t.length() ? "\n" : "") + t;
      turns.back().images += attached;
    } else {
      AiTurn turn;
      turn.user = m.user;
      turn.text = t;
      turn.images = attached;
      turns.push_back(turn);
    }
  }
  if (turns.empty() || !turns.back().user) return;
  const uint8_t *jpegs[AI_MAX_PAGES];
  size_t lengths[AI_MAX_PAGES];
  uint32_t ids[AI_MAX_PAGES];
  uint8_t *loaded[AI_MAX_PAGES] = {nullptr};
  int pages = 0;
  for (uint32_t id : sent) {
    uint8_t *j = nullptr;
    size_t l = 0;
    if (pages < AI_MAX_PAGES && loadPhoto(id, j, l)) {
      loaded[pages] = j;
      jpegs[pages] = j;
      lengths[pages] = l;
      ids[pages] = id;
      ++pages;
    } else
      for (AiTurn &turn : turns)
        if (turn.images) {
          --turn.images;
          break;
        }  // a deleted photo
  }
  AiOptions options = aiOptions(useGemini);
  pauseBluetooth();  // Bluetooth leaves too little memory for HTTPS; it comes back after
  const bool voice = q.voice && chatWav && useGemini;
  const bool started = aiChat(options, turns.data(), turns.size(), jpegs, lengths, ids, pages,
                              voice ? chatWav : nullptr, voice ? chatWavLen : 0);
  options.key = "";
  for (uint8_t *l : loaded) free(l);
  if (!started) {
    toast("Could not start: not enough memory");
    return;
  }
  q.pending = false;
  pending = Pending::Answer;
  pendingGemini = useGemini;
  pendingPhotoId = photos.empty() ? 0 : photos.back();
  ChatMsg reply;
  reply.user = false;
  chat.push_back(reply);
  saveChat();
  requestChatId = chatId;
  assistantLive = drawMessage(chat.back(), chat.size() - 1);
  refreshChat();
  scrollChatToEnd(true);
}
// The answer so far (streaming), the final answer, or why it failed.
void chatPartial(const String &text) {
  if (!assistantLive || requestChatId != chatId) return;
  const bool atEnd = lv_obj_get_scroll_bottom(chatScroll) < 40;
  renderInto(assistantLive, text);
  if (atEnd) scrollChatToEnd(false);
}
// A reply for a chat that is not on screen goes into that chat's saved file.
void finishInFile(uint32_t id, bool ok, const String &result) {
  String json;
  JsonDocument doc;
  if (!chatLoad(id, json) || deserializeJson(doc, json)) return;
  JsonArray list = doc["m"].as<JsonArray>();
  if (list.size() == 0) return;
  JsonObject last = list[list.size() - 1];
  if ((bool)(last["u"] | true)) return;
  last["t"] = result;
  if (!ok) last["f"] = 1;
  String out;
  serializeJson(doc, out);
  chatSave(id, out);
}
void chatFinished(bool ok, const String &result) {
  free(chatWav);
  chatWav = nullptr;
  chatWavLen = 0;
  if (requestChatId && requestChatId != chatId) {
    finishInFile(requestChatId, ok, result);
    toast(ok ? "Answer ready in another chat" : "A request in another chat failed");
    requestChatId = 0;
    refreshChat();
    return;
  }
  requestChatId = 0;
  if (chat.empty() || chat.back().user) return;
  ChatMsg &reply = chat.back();
  reply.text = result;
  reply.failed = !ok;
  if (assistantLive) {
    lv_obj_t *old = assistantLive;
    assistantLive = nullptr;
    const bool atEnd = lv_obj_get_scroll_bottom(chatScroll) < 40;
    lv_obj_delete(old);
    drawMessage(reply, chat.size() - 1);
    if (atEnd) scrollChatToEnd(false);
  }
  saveChat();
  refreshChat();
}
void chatCancelled() {
  if (requestChatId && requestChatId != chatId) {
    finishInFile(requestChatId, false, "Stopped.");
    requestChatId = 0;
    return;
  }
  requestChatId = 0;
  if (!chat.empty() && !chat.back().user && chat.back().text.isEmpty()) chat.pop_back();
  saveChat();
  rebuildChat();
}

// ---------- actions ----------
void newChat() {
  chat.clear();
  chatId = 0;
  attachIds.clear();
  attachAwaiting = 0;
  draft = "";
  rebuildChat();
}
void chatTakePhoto() {
  lv_obj_add_flag(plusSheet, LV_OBJ_FLAG_HIDDEN);
  if (cameraOff) {
    toast("Camera is off. Turn it on in Control Center");
    return;
  }
  if ((int)attachIds.size() + attachAwaiting >= AI_MAX_PAGES) {
    toast("Up to 3 photos per message");
    return;
  }
  pageCamera = true;
  setText(pageBanner, "Photo for this chat");
  show(Screen::Camera);
}
void addPage() { chatTakePhoto(); }
void newPhoto() { chatTakePhoto(); }
// The camera came back with a photo for the chat; it is attached once it is saved.
void chatPhotoTaken() {
  ++attachAwaiting;
  show(Screen::Ask);
  refreshAttachments();
  refreshChat();
}
void chatPhotoSaved(uint32_t id) {
  if (!attachAwaiting) return;
  --attachAwaiting;
  attachIds.push_back(id);
  refreshAttachments();
  refreshChat();
}
void typeQuestion() {
  textMode = TextMode::Question;
  setText(passwordTitle, "Message");
  lv_textarea_set_password_mode(passwordField, false);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Type, or hold Talk");
  hide(passwordEye, true);
  lv_obj_set_width(passwordField, W - 28);
  lv_textarea_set_text(passwordField, draft.c_str());
  show(Screen::Password);
}
// Hold-to-talk released. With "Show words first" the words land in the message box to send;
// otherwise Gemini hears the recording directly (GPT gets the words, sent at once).
void startListenBack(const uint8_t *wav, size_t length) {
  if (!networkConnected()) {
    sendMessage(draft, wav, length);
    return;
  }
  if (!confirmWords && useGemini) {
    sendMessage(draft, wav, length);
    return;
  }
  if (aiBusy()) {
    toast("Still answering the last message");
    return;
  }
  sendAfterTranscribe = !confirmWords;
  AiOptions options = aiOptions(useGemini);
  pauseBluetooth();
  const bool started = aiTranscribe(options, wav, length);
  options.key = "";
  if (!started) {
    toast("Could not start: not enough memory");
    return;
  }
  pending = Pending::Transcribe;
  toast("Writing it down");
}
void chatHeard(bool ok, const String &words) {
  if (!ok) {
    toast("Could not hear that. Hold the mic and try again");
    return;
  }
  draft = (draft.length() ? draft + " " : String("")) + words;
  if (sendAfterTranscribe) {
    sendAfterTranscribe = false;
    sendMessage(draft);
    return;
  }
  refreshChat();
}

// ---------- the screen ----------
void buildChat(lv_obj_t *s) {
  // Top: the chat's title, the Chats list and New chat.
  lv_obj_t *top = plain(s);
  lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_align(top, LV_ALIGN_TOP_RIGHT, -10, 14);
  lv_obj_t *chats = circle(top, 36, INK, 0, GRAPHITE, 230);
  lv_obj_set_style_opa(chats, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(chats, LV_SYMBOL_LIST, F_BODY, INK));
  lv_obj_set_ext_click_area(chats, 5);
  onClick(chats, [] { show(Screen::Chats); });
  newChatBtn = chip(top, "New chat");
  lv_obj_set_height(newChatBtn, 36);
  lv_obj_set_style_radius(newChatBtn, 18, 0);
  onClick(newChatBtn, [] {
    newChat();
    toast("New chat");
  });

  // The messages fill the screen to the bottom and scroll behind the floating composer; their
  // bottom padding keeps the last message clear of it (and of waiting photos).
  chatScroll = plain(s);
  lv_obj_set_pos(chatScroll, 0, 58);
  lv_obj_set_size(chatScroll, W, H - 58);
  scrollable(chatScroll);
  lv_obj_set_flex_flow(chatScroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(chatScroll, 14, 0);
  lv_obj_set_style_pad_bottom(chatScroll, H - COMPOSER_Y + 8, 0);
  chatEmpty = plain(s);
  lv_obj_set_size(chatEmpty, W, 120);
  lv_obj_align(chatEmpty, LV_ALIGN_CENTER, 0, -24);
  lv_obj_t *hello = text(chatEmpty, "Ask anything", F_LARGE, INK);
  lv_obj_align(hello, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_t *how = text(chatEmpty, "Add a photo with +, type,\nor hold the mic", F_SMALL, MIST);
  lv_obj_set_style_text_align(how, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(how, LV_ALIGN_TOP_MID, 0, 58);

  attachRow = plain(s);
  lv_obj_set_size(attachRow, W - 24, 48);
  lv_obj_set_pos(attachRow, 12, COMPOSER_Y - 52);
  lv_obj_set_flex_flow(attachRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(attachRow, 8, 0);

  const int composerY = COMPOSER_Y;
  // Composer: + | message box | mic, send or stop.
  lv_obj_t *plus = circle(s, 44, INK, 0, GRAPHITE, LV_OPA_COVER);
  lv_obj_set_pos(plus, 14, composerY);
  lv_obj_set_ext_click_area(plus, 8);
  lv_obj_set_style_opa(plus, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(plus, LV_SYMBOL_PLUS, F_BODY, INK));
  onClick(plus, [] { lv_obj_remove_flag(plusSheet, LV_OBJ_FLAG_HIDDEN); });
  lv_obj_t *field = pressedFeedback(plain(s));
  lv_obj_set_size(field, W - 14 - 44 - 6 - 6 - 44 - 14, 44);
  lv_obj_set_pos(field, 64, composerY);
  lv_obj_set_style_radius(field, 22, 0);
  lv_obj_set_style_bg_color(field, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
  composerText = text(field, "Message", F_BODY, MIST);
  lv_label_set_long_mode(composerText, LV_LABEL_LONG_DOT);
  lv_obj_set_size(composerText, W - 14 - 44 - 6 - 6 - 44 - 14 - 28, 20);  // one line
  lv_obj_align(composerText, LV_ALIGN_LEFT_MID, 14, 0);
  onClick(field, typeQuestion);
  const int right = W - 14 - 44;
  micBtn = circle(s, 44, INK, 0, GRAPHITE, LV_OPA_COVER);
  lv_obj_set_pos(micBtn, right, composerY);
  lv_obj_set_style_bg_color(micBtn, LENS, LV_STATE_PRESSED);
  {
    lv_obj_t *cap = plain(micBtn);  // microphone glyph: capsule, cradle, stem
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
  }
  lv_obj_add_flag(micBtn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(micBtn, 8);
  lv_obj_add_event_cb(
      micBtn,
      [](lv_event_t *) {
        if (micRecording() || aiBusy()) return;
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
  sendBtn = circle(s, 44, VOID_, 0, LENS, LV_OPA_COVER);
  lv_obj_set_pos(sendBtn, right, composerY);
  lv_obj_set_style_opa(sendBtn, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_center(text(sendBtn, LV_SYMBOL_UP, F_BODY, VOID_));
  onClick(sendBtn, [] { sendMessage(draft); });
  lv_obj_set_ext_click_area(sendBtn, 8);
  stopBtn = circle(s, 44, VOID_, 0, INK, LV_OPA_COVER);
  lv_obj_set_pos(stopBtn, right, composerY);
  {
    lv_obj_t *sq = plain(stopBtn);
    lv_obj_set_size(sq, 14, 14);
    lv_obj_set_style_radius(sq, 3, 0);
    lv_obj_set_style_bg_color(sq, VOID_, 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_center(sq);
  }
  onClick(stopBtn, [] { aiCancel(); });
  lv_obj_set_ext_click_area(stopBtn, 8);

  // "+": take a photo now, or choose from Photos.
  plusSheet = plain(s);
  lv_obj_set_size(plusSheet, W, H);
  lv_obj_set_style_bg_color(plusSheet, VOID_, 0);
  lv_obj_set_style_bg_opa(plusSheet, 140, 0);
  lv_obj_add_flag(plusSheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(plusSheet, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      plusSheet,
      [](lv_event_t *e) {
        if (lv_event_get_target(e) == plusSheet) lv_obj_add_flag(plusSheet, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CLICKED, nullptr);
  lv_obj_t *sheet = plain(plusSheet);
  lv_obj_set_size(sheet, 220, 104);
  lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME - 52);
  lv_obj_set_style_radius(sheet, 22, 0);
  lv_obj_set_style_bg_color(sheet, GRAPHITE, 0);
  lv_obj_set_style_bg_opa(sheet, 250, 0);
  lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *take = pressedFeedback(plain(sheet));
  lv_obj_set_size(take, 220, 52);
  lv_obj_t *takeLabel = text(take, LV_SYMBOL_IMAGE "   Take photo", F_BODY, INK);
  lv_obj_align(takeLabel, LV_ALIGN_LEFT_MID, 20, 0);
  onClick(take, chatTakePhoto);
  lv_obj_t *pick = pressedFeedback(plain(sheet));
  lv_obj_set_size(pick, 220, 52);
  lv_obj_set_pos(pick, 0, 52);
  lv_obj_set_style_border_side(pick, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(pick, 1, 0);
  lv_obj_set_style_border_color(pick, LINE, 0);
  lv_obj_t *pickLabel = text(pick, LV_SYMBOL_LIST "   Choose from Photos", F_BODY, INK);
  lv_obj_align(pickLabel, LV_ALIGN_LEFT_MID, 20, 0);
  onClick(pick, [] {
    lv_obj_add_flag(plusSheet, LV_OBJ_FLAG_HIDDEN);
    if ((int)attachIds.size() + attachAwaiting >= AI_MAX_PAGES) {
      toast("Up to 3 photos per message");
      return;
    }
    show(Screen::Picker);
  });
}

// ---------- Choose from Photos ----------
lv_obj_t *pickerGrid, *pickerAdd, *pickerEmpty;
std::vector<uint32_t> pickerChosen;
std::vector<void *> pickerImages;
void refreshPicker() {
  lv_obj_clean(pickerGrid);
  for (void *p : pickerImages) free(p);
  pickerImages.clear();
  pickerChosen.clear();
  refreshPhotos();
  hide(pickerEmpty, photoCount != 0);
  for (int i = 0; i < photoCount; ++i) {
    lv_obj_t *cell = plain(pickerGrid);
    lv_obj_set_size(cell, 66, 66);
    lv_obj_set_style_radius(cell, 10, 0);
    lv_obj_set_style_border_color(cell, LENS, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 3, 0);
    const size_t before = chatImages.size();
    lv_obj_t *img = photoThumb(cell, photoIds[i], 60);
    lv_obj_center(img);
    // These thumbnails belong to the picker, not the chat.
    for (size_t k = before; k < chatImages.size(); ++k) pickerImages.push_back(chatImages[k]);
    chatImages.resize(before);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        cell,
        [](lv_event_t *e) {
          lv_obj_t *c = (lv_obj_t *)lv_event_get_target(e);
          const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
          for (auto it = pickerChosen.begin(); it != pickerChosen.end(); ++it)
            if (*it == id) {
              pickerChosen.erase(it);
              lv_obj_set_style_border_width(c, 0, 0);
              goto counted;
            }
          if ((int)(pickerChosen.size() + attachIds.size()) + attachAwaiting >= AI_MAX_PAGES) {
            toast("Up to 3 photos per message");
            return;
          }
          pickerChosen.push_back(id);
          lv_obj_set_style_border_width(c, 3, 0);
        counted:
          setText(lv_obj_get_child(pickerAdd, 0),
                  pickerChosen.empty() ? "Add" : (String("Add ") + pickerChosen.size()).c_str());
        },
        LV_EVENT_CLICKED, (void *)(uintptr_t)photoIds[i]);
  }
  setText(lv_obj_get_child(pickerAdd, 0), "Add");
}
void buildPicker() {
  lv_obj_t *s = scr[(int)Screen::Picker] = screenBase();
  title(s, "Photos");
  pickerGrid = plain(s);
  lv_obj_set_pos(pickerGrid, 0, 56);
  lv_obj_set_size(pickerGrid, W, H - 56 - ABOVE_HOME - 54);
  scrollable(pickerGrid);
  lv_obj_set_flex_flow(pickerGrid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_hor(pickerGrid, 14, 0);
  lv_obj_set_style_pad_column(pickerGrid, 6, 0);
  lv_obj_set_style_pad_row(pickerGrid, 6, 0);
  pickerEmpty = text(s, "No photos yet.\nTake one with Camera.", F_BODY, MIST);
  lv_obj_set_style_text_align(pickerEmpty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(pickerEmpty, LV_ALIGN_CENTER, 0, -20);
  pickerAdd = pill(s, "Add", 120);
  lv_obj_set_style_bg_color(pickerAdd, LENS, 0);
  lv_obj_set_style_text_color(lv_obj_get_child(pickerAdd, 0), VOID_, 0);
  lv_obj_align(pickerAdd, LV_ALIGN_BOTTOM_MID, 0, -ABOVE_HOME);
  onClick(pickerAdd, [] {
    for (uint32_t id : pickerChosen) attachIds.push_back(id);
    pickerChosen.clear();
    goBack();
    refreshAttachments();
    refreshChat();
  });
}

// ---------- Chats: earlier conversations ----------
lv_obj_t *chatsList, *chatsEmpty;
void refreshChats() {
  lv_obj_clean(chatsList);
  uint32_t ids[CHAT_KEEP];
  const int n = chatList(ids, CHAT_KEEP);
  hide(chatsEmpty, n != 0);
  for (int i = 0; i < n; ++i) {
    String json;
    JsonDocument doc;
    if (!chatLoad(ids[i], json) || deserializeJson(doc, json)) continue;
    lv_obj_t *r = pressedFeedback(plain(chatsList));
    lv_obj_set_size(r, W, 56);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, LINE, 0);
    lv_obj_t *one = text(r, (const char *)(doc["title"] | "Chat"), F_BODY, ids[i] == chatId ? LENS : INK);
    lv_label_set_long_mode(one, LV_LABEL_LONG_DOT);
    lv_obj_set_size(one, W - 40, 20);
    lv_obj_set_pos(one, 20, 8);
    const String when = ago(doc["when"] | 0);
    const bool answering = aiBusy() && pending == Pending::Answer && requestChatId == ids[i];
    text(r,
         answering
             ? "Answering..."
             : (String(doc["m"].as<JsonArray>().size()) + " messages" + (when.length() ? ", " + when : "")).c_str(),
         F_SMALL, answering ? LENS : MIST);
    lv_obj_set_pos(lv_obj_get_child(r, 1), 20, 32);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          const uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
          if (loadChat(id)) {
            rebuildChat();
            goBack();
          } else toast("That chat could not be opened");
        },
        LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)ids[i]);
    lv_obj_add_event_cb(
        r,
        [](lv_event_t *e) {
          confirm(
              "Delete this chat?", "Delete chat",
              [](uint32_t id) {
                chatDelete(id);
                if (id == chatId) newChat();
                refreshChats();
                toast("Deleted");
              },
              (uint32_t)(uintptr_t)lv_event_get_user_data(e));
        },
        LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)ids[i]);
  }
}
void buildChats() {
  lv_obj_t *s = scr[(int)Screen::Chats] = screenBase();
  title(s, "Chats");
  chatsList = plain(s);
  lv_obj_set_pos(chatsList, 0, 56);
  lv_obj_set_size(chatsList, W, H - 56);
  scrollable(chatsList);
  lv_obj_set_flex_flow(chatsList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(chatsList, 60, 0);
  chatsEmpty = text(s, "No chats yet.", F_BODY, MIST);
  lv_obj_align(chatsEmpty, LV_ALIGN_CENTER, 0, -20);
}
