#include "ai_client.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <img_converters.h>
#include <mbedtls/platform.h>
#include <time.h>
#include <memory>
#include <Preferences.h>
#include <vector>
#include "ai_root_certs.h"
#include "storage.h"

const AiModel GEMINI_MODELS[] = {
    {"gemini-3.8-flash", "Gemini 3.8 Flash", "Newest. Small free daily limit"},
    {"gemini-3.7-flash", "Gemini 3.7 Flash", "Strong. Separate free limit"},
    {"gemini-3.5-flash", "Gemini 3.5 Flash", "Older, still good"},
    {"gemini-3.5-flash-lite", "Gemini 3.5 Flash-Lite", "Fastest, simplest problems"},
};
const int GEMINI_MODEL_COUNT = sizeof(GEMINI_MODELS) / sizeof(GEMINI_MODELS[0]);
const AiModel GPT_MODELS[] = {
    {"gpt-6.1-sol", "GPT-6.1 Sol", "Accurate. About 1-2 cents a photo"},
    {"gpt-6-luna", "GPT-6 Luna", "Cheapest. Simple problems"},
    {"gpt-6-astra", "GPT-6 Astra", "Most capable. Costs the most"},
};
const int GPT_MODEL_COUNT = sizeof(GPT_MODELS) / sizeof(GPT_MODELS[0]);
const char *const AI_EFFORTS[] = {"low", "medium", "high"};

namespace {
constexpr size_t RESPONSE_LIMIT = 32768;
constexpr size_t ANSWER_LIMIT = 10000;
constexpr unsigned long WIFI_WAIT_MS = 15000;
// Set by the UI; checked before anything leaves the device.
volatile bool cancelled = false;
// Set by the UI: true while a photo is still being written to flash. Flash writes stall
// the cache Wi-Fi runs from, and an upload started meanwhile failed mid-send.
bool (*flashBusy)() = nullptr;
void countRequest(bool gemini);

class BoundedResponse : public Stream {
 public:
  char *data;
  size_t used = 0;
  bool overflow = false;
  explicit BoundedResponse(char *buffer) : data(buffer) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    if (n > RESPONSE_LIMIT - used) {
      overflow = true;
      return 0;
    }
    memcpy(data + used, b, n);
    used += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// The reply format is what the device renders (see the answer screen): a one-line answer,
// then numbered steps, formulas on their own lines. Plain ASCII: the fonts have nothing else.
const char *FORMAT =
    "Reply in this exact format for a small screen.\n"
    "If there is one question:\n"
    "Answer: <the final answer, one short line>\n"
    "Steps:\n"
    "1. <one short step>\n"
    "2. <next step>\n"
    "If there are several questions, answer every one in order, each like this, with a blank line between them:\n"
    "Q<number>: <the final answer, one short line>\n"
    "1. <one short step>\n"
    "2. <next step>\n"
    "Use the question numbers printed on the page when there are any. "
    "Put each formula or equation on its own line. Use ASCII math (x^2, sqrt(x), *, /, <=). "
    "No tables, no LaTeX, no other headings. Use **bold** only for key terms. "
    "If text is unclear, say exactly what cannot be read and ask for a closer photo; never invent content.";
const char *TASK_ONE = "Read the photographed problem carefully and solve it.";
const char *TASK_PAGES =
    "Several photos are attached in order; the last one is the newest. Earlier photos are "
    "context from this conversation. Solve what the newest photo asks.";
const char *TASK_VOICE =
    "The attached audio is the user's spoken question about the photos. Answer that question. "
    "If the audio is unclear, say what you could not hear.";
// A conversation's standing instructions (system message); the turns carry the chat itself.
const char *CHAT_SYSTEM =
    "You are Perch, the assistant on a small camera device with a 240x284 screen. "
    "The user may attach photos of problems, pages, screens or objects: read them carefully. "
    "Earlier photos and messages in this conversation are context; answer the newest message.\n";
const char *TRANSCRIBE_PROMPT =
    "Transcribe this audio exactly as spoken. Reply with only the words, no commentary. "
    "If nothing intelligible was said, reply with an empty line.";

// The display fonts only cover printable ASCII. Models still send Unicode math and
// typography, which would silently vanish and can change the meaning of an answer.
// Markdown markers (** and list syntax) are kept: the answer screen formats them.
String toDisplayText(const String &input) {
  static const struct {
    uint32_t code;
    const char *text;
  } map[] = {
      {0x00D7, "x"},       {0x2212, "-"},     {0x00F7, "/"},     {0x2215, "/"},    {0x00B2, "^2"},
      {0x00B3, "^3"},      {0x00B9, "^1"},    {0x2070, "^0"},    {0x2074, "^4"},   {0x2075, "^5"},
      {0x2076, "^6"},      {0x2077, "^7"},    {0x2078, "^8"},    {0x2079, "^9"},   {0x207F, "^n"},
      {0x2080, "_0"},      {0x2081, "_1"},    {0x2082, "_2"},    {0x2083, "_3"},   {0x221A, "sqrt"},
      {0x03C0, "pi"},      {0x03B8, "theta"}, {0x03B1, "alpha"}, {0x03B2, "beta"}, {0x0394, "Delta"},
      {0x03BB, "lambda"},  {0x03BC, "mu"},    {0x03C3, "sigma"}, {0x03A3, "Sum"},  {0x2264, "<="},
      {0x2265, ">="},      {0x2260, "!="},    {0x2248, "~="},    {0x00B1, "+/-"},  {0x221E, "inf"},
      {0x00B0, " deg"},    {0x2192, "->"},    {0x2190, "<-"},    {0x21D2, "=>"},   {0x2194, "<->"},
      {0x2018, "'"},       {0x2019, "'"},     {0x201C, "\""},    {0x201D, "\""},   {0x2013, "-"},
      {0x2014, "-"},       {0x2026, "..."},   {0x2022, "-"},     {0x00B7, "*"},    {0x22C5, "*"},
      {0x00BD, "1/2"},     {0x00BC, "1/4"},   {0x00BE, "3/4"},   {0x2208, " in "}, {0x2200, "for all "},
      {0x2203, "exists "}, {0x2229, " and "}, {0x222A, " or "},  {0x00A0, " "},    {0x2032, "'"},
      {0x2033, "\""},
  };
  String out;
  out.reserve(input.length());
  const uint8_t *s = (const uint8_t *)input.c_str();
  for (size_t i = 0; i < input.length();) {
    uint8_t c = s[i];
    if (c < 0x80) {
      if (c == '\n' || c == '\t' || c >= 0x20) out += (char)(c == '\t' ? ' ' : c);
      ++i;
      continue;
    }
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t code = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : c & 0x1F;
    if (!extra || i + extra >= input.length()) {
      ++i;
      out += '?';
      continue;
    }
    for (int k = 1; k <= extra; ++k) code = (code << 6) | (s[i + k] & 0x3F);
    i += extra + 1;
    const char *text = "?";
    for (const auto &entry : map)
      if (entry.code == code) {
        text = entry.text;
        break;
      }
    out += text;
  }
  return out;
}

String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else out += c;
  }
  return out;
}

// A request body is text pieces with base64 blobs in between, assembled once in PSRAM.
struct Piece {
  String text;
  const uint8_t *data = nullptr;
  size_t length = 0;
};
struct Body {
  std::vector<Piece> pieces;
  void add(const String &t) {
    Piece p;
    p.text = t;
    pieces.push_back(p);
  }
  void blob(const uint8_t *d, size_t n) {
    Piece p;
    p.data = d;
    p.length = n;
    pieces.push_back(p);
  }
  // Returns a NUL-terminated PSRAM buffer the caller frees, or nullptr.
  uint8_t *build(size_t &total) const {
    total = 0;
    for (const auto &p : pieces) total += p.data ? 4 * ((p.length + 2) / 3) : p.text.length();
    uint8_t *buf = (uint8_t *)ps_malloc(total + 1);
    if (!buf) return nullptr;
    uint8_t *at = buf;
    for (const auto &p : pieces) {
      if (p.data) {
        size_t written = 0;
        const size_t encoded = 4 * ((p.length + 2) / 3);
        if (mbedtls_base64_encode(at, encoded + 1, &written, p.data, p.length) || written != encoded) {
          free(buf);
          return nullptr;
        }
        at += encoded;
      } else {
        memcpy(at, p.text.c_str(), p.text.length());
        at += p.text.length();
      }
    }
    buf[total] = 0;
    return buf;
  }
};

// Streamed answers arrive as server-sent events ("data: {json}" lines): Gemini sends pieces
// of GenerateContentResponse, OpenAI sends chat deltas. The text so far is shared with the UI.
portMUX_TYPE partialLock = portMUX_INITIALIZER_UNLOCKED;
String partialText;
volatile uint32_t partialVersion = 0;
class SseSink : public Stream {
 public:
  bool gemini;
  String text, finish, line, error;
  bool blocked = false;
  explicit SseSink(bool g) : gemini(g) {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; ++i) {
      const char c = (char)b[i];
      if (c == '\n') {
        handle();
        line = "";
      } else if (c != '\r' && line.length() < 60000) line += c;
    }
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  void handle() {
    if (!line.startsWith("data:")) return;
    String payload = line.substring(5);
    payload.trim();
    if (payload.isEmpty() || payload == "[DONE]") return;
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return;
    if (doc["error"].is<JsonObject>()) {
      error = (const char *)(doc["error"]["message"] | "error");
      return;
    }
    String piece;
    if (gemini) {
      for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
        if (part["thought"] == true) continue;
        const char *t = part["text"];
        if (t) piece += t;
      }
      const char *f = doc["candidates"][0]["finishReason"];
      if (f) finish = f;
      if (doc["promptFeedback"]["blockReason"].is<const char *>()) blocked = true;
    } else {
      const char *t = doc["choices"][0]["delta"]["content"];
      if (t) piece = t;
      const char *f = doc["choices"][0]["finish_reason"];
      if (f) finish = f;
    }
    if (piece.isEmpty()) return;
    text += piece;
    const String shown = toDisplayText(text);
    portENTER_CRITICAL(&partialLock);
    partialText = shown;
    ++partialVersion;
    portEXIT_CRITICAL(&partialLock);
  }
};

// Multipart body for OpenAI's transcription endpoint (raw bytes, not base64).
uint8_t *multipartWav(const uint8_t *wav, size_t length, const char *model, const char *boundary, size_t &total) {
  const String head = String("--") + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + model +
                      "\r\n"
                      "--" +
                      boundary +
                      "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
                      "--" +
                      boundary +
                      "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n"
                      "Content-Type: audio/wav\r\n\r\n";
  const String tail = String("\r\n--") + boundary + "--\r\n";
  total = head.length() + length + tail.length();
  uint8_t *buf = (uint8_t *)ps_malloc(total);
  if (!buf) return nullptr;
  memcpy(buf, head.c_str(), head.length());
  memcpy(buf + head.length(), wav, length);
  memcpy(buf + head.length() + length, tail.c_str(), tail.length());
  return buf;
}

// Half-size copy of a photo (1024x768 from 2048x1536, about a third of the bytes) for a
// retry when a full upload failed part way on a weak connection.
bool shrinkJpeg(const uint8_t *in, size_t inLen, uint8_t *&out, size_t &outLen) {
  int w = 0, h = 0;
  for (size_t i = 2; i + 9 < inLen;) {
    if (in[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = in[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      h = (in[i + 5] << 8) | in[i + 6];
      w = (in[i + 7] << 8) | in[i + 8];
      break;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    i += 2 + ((in[i + 2] << 8) | in[i + 3]);
  }
  if (w < 800) return false;  // already small
  const int sw = w / 2, sh = h / 2;
  uint8_t *rgb = (uint8_t *)ps_malloc((size_t)sw * sh * 2);
  if (!rgb) return false;
  bool ok = jpg2rgb565(in, inLen, rgb, JPG_SCALE_2X);
  // The decoder writes little-endian pixels; the encoder reads them big-endian.
  for (size_t i = 0; ok && i < (size_t)sw * sh * 2; i += 2) {
    const uint8_t t = rgb[i];
    rgb[i] = rgb[i + 1];
    rgb[i + 1] = t;
  }
  out = nullptr;
  outLen = 0;
  ok = ok && fmt2jpg(rgb, (size_t)sw * sh * 2, sw, sh, PIXFORMAT_RGB565, 88, &out, &outLen);
  free(rgb);
  return ok && out;
}

enum class Kind { Answer, Transcribe };
struct Job {
  Kind kind = Kind::Answer;
  AiOptions options;
  uint8_t *jpegs[AI_MAX_PAGES] = {nullptr};
  size_t lengths[AI_MAX_PAGES] = {0};
  int pages = 0;
  String question;
  String history;             // earlier questions and answers in this conversation
  std::vector<AiTurn> turns;  // a conversation (Ask): sent as real turns
  uint32_t photoIds[AI_MAX_PAGES] = {0};
  String fileUris[AI_MAX_PAGES];  // Gemini Files API links for photos already uploaded
  uint8_t *wav = nullptr;
  size_t wavLength = 0;
  String result;
  bool ok = false;
  void release() {
    for (int i = 0; i < AI_MAX_PAGES; ++i) {
      free(jpegs[i]);
      jpegs[i] = nullptr;
      lengths[i] = 0;
      photoIds[i] = 0;
      fileUris[i] = "";
    }
    pages = 0;
    free(wav);
    wav = nullptr;
    wavLength = 0;
    options.key = "";
    question = "";
    history = "";
    turns.clear();
  }
};

// A conversation as real turns: system instructions, then user/model messages, each with its
// own photos (a Files API link when uploaded, else inline). Consecutive turns of the same
// role were merged by the caller.
void addImage(const Job &j, int i, Body &body) {
  if (j.options.gemini) {
    if (j.fileUris[i].length()) {
      body.add(String(",{\"file_data\":{\"mime_type\":\"image/jpeg\",\"file_uri\":\"") + j.fileUris[i] + "\"}}");
    } else {
      body.add(",{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"");
      body.blob(j.jpegs[i], j.lengths[i]);
      body.add("\"}}");
    }
  } else {
    body.add(",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
    body.blob(j.jpegs[i], j.lengths[i]);
    body.add("\",\"detail\":\"high\"}}");
  }
}
void buildChatBody(const Job &j, Body &body) {
  const String system = jsonEscape(String(CHAT_SYSTEM) + FORMAT);
  const bool voice = j.options.gemini && j.wav && j.wavLength;
  int image = 0;
  if (j.options.gemini) {
    body.add(String("{\"systemInstruction\":{\"parts\":[{\"text\":\"") + system + "\"}]},\"contents\":[");
    for (size_t t = 0; t < j.turns.size(); ++t) {
      const AiTurn &turn = j.turns[t];
      String text = turn.text;
      if (text.isEmpty()) text = turn.user ? (turn.images ? "(photo)" : "(spoken question)") : "(no answer)";
      body.add(String(t ? "," : "") + "{\"role\":\"" + (turn.user ? "user" : "model") + "\",\"parts\":[{\"text\":\"" +
               jsonEscape(text) + "\"}");
      for (int k = 0; k < turn.images && image < j.pages; ++k) addImage(j, image++, body);
      if (voice && t == j.turns.size() - 1) {
        body.add(",{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
        body.blob(j.wav, j.wavLength);
        body.add("\"}}");
      }
      body.add("]}");
    }
    body.add(String("],\"generationConfig\":{\"maxOutputTokens\":8192,\"thinkingConfig\":{\"thinkingLevel\":\"") +
             j.options.effort + "\"}}}");
  } else {
    body.add(String("{\"model\":\"") + j.options.model + "\",\"reasoning_effort\":\"" + j.options.effort +
             "\",\"max_completion_tokens\":8000,\"stream\":true,\"messages\":[{\"role\":\"system\",\"content\":\"" +
             system + "\"}");
    for (const AiTurn &turn : j.turns) {
      String text = turn.text;
      if (text.isEmpty()) text = turn.user ? (turn.images ? "(photo)" : "(spoken question)") : "(no answer)";
      if (turn.user) {
        body.add(String(",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"") + jsonEscape(text) + "\"}");
        for (int k = 0; k < turn.images && image < j.pages; ++k) addImage(j, image++, body);
        body.add("]}");
      } else {
        body.add(String(",{\"role\":\"assistant\",\"content\":\"") + jsonEscape(text) + "\"}");
      }
    }
    body.add("]}");
  }
}

// Gemini Files API: upload a photo once (resumable start, then upload+finalize) and get its
// link. Free; files are kept 48 hours. Later turns send the link instead of the photo.
uint32_t keyTag(const String &key) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < key.length(); ++i) {
    h ^= (uint8_t)key[i];
    h *= 16777619u;
  }
  return h;
}
bool geminiUpload(const String &key, const uint8_t *data, size_t length, String &uri) {
  String uploadUrl;
  {
    WiFiClientSecure client;
    client.setCACert(AI_ROOT_CERTS);
    client.setHandshakeTimeout(12);
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(20000);
    static const char *keep[] = {"x-goog-upload-url"};
    http.collectHeaders(keep, 1);
    if (!http.begin(client, "https://generativelanguage.googleapis.com/upload/v1beta/files")) return false;
    http.addHeader("x-goog-api-key", key);
    http.addHeader("X-Goog-Upload-Protocol", "resumable");
    http.addHeader("X-Goog-Upload-Command", "start");
    http.addHeader("X-Goog-Upload-Header-Content-Length", String(length));
    http.addHeader("X-Goog-Upload-Header-Content-Type", "image/jpeg");
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST("{\"file\":{\"display_name\":\"perch-photo\"}}");
    uploadUrl = http.header("x-goog-upload-url");
    http.end();
    Serial.printf("AI_UPLOAD start %d\n", status);
    if (status != 200 || !uploadUrl.startsWith("https://")) return false;
  }
  WiFiClientSecure client;
  client.setCACert(AI_ROOT_CERTS);
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(65000);
  if (!http.begin(client, uploadUrl)) return false;
  http.addHeader("X-Goog-Upload-Offset", "0");
  http.addHeader("X-Goog-Upload-Command", "upload, finalize");
  const uint32_t t = millis();
  const int status = http.POST((uint8_t *)data, length);
  const String reply = status > 0 ? http.getString() : String("");
  http.end();
  JsonDocument doc;
  if (status != 200 || deserializeJson(doc, reply)) return false;
  uri = (const char *)(doc["file"]["uri"] | "");
  Serial.printf("AI_UPLOAD %u bytes in %lums: %s\n", (unsigned)length, (unsigned long)(millis() - t),
                uri.length() ? "ok" : "no link");
  return uri.length() > 0;
}
// Give each photo a Files API link: reuse a saved one (same key, not expired) or upload now.
void attachFiles(Job &j) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return;  // expiry needs the clock
  const uint32_t tag = keyTag(j.options.key);
  for (int i = 0; i < j.pages && !cancelled; ++i) {
    if (!j.photoIds[i]) continue;
    String uri;
    uint32_t expires = 0, savedTag = 0;
    if (photoUriLoad(j.photoIds[i], uri, expires, savedTag) && savedTag == tag && (time_t)expires > now + 600) {
      j.fileUris[i] = uri;
      continue;
    }
    if (geminiUpload(j.options.key, j.jpegs[i], j.lengths[i], uri)) {
      j.fileUris[i] = uri;
      photoUriSave(j.photoIds[i], uri, (uint32_t)now + 47 * 3600, tag);
    }
  }
}

// The request bodies. Shared by real requests and the dry run.
void buildAnswerBody(const Job &j, Body &body) {
  if (!j.turns.empty()) {
    buildChatBody(j, body);
    return;
  }
  const bool voice = j.options.gemini && j.wav && j.wavLength;
  String task = voice ? TASK_VOICE
                      : (j.pages > 1    ? TASK_PAGES
                         : j.pages == 1 ? TASK_ONE
                                        : "Answer the user's question.");
  if (voice && j.pages == 0)
    task =
        "The attached audio is the user's spoken question. Answer it. If it is unclear, say what you could not hear.";
  if (j.history.length())
    task += String("\nEarlier in this conversation (context only, do not repeat it):\n") + j.history + "\n";
  if (j.question.length()) task += String(" The user's question now: \"") + j.question + "\". Answer that question.";
  const String prompt = jsonEscape(task + "\n" + FORMAT);
  if (j.options.gemini) {
    body.add(String("{\"contents\":[{\"parts\":[{\"text\":\"") + prompt + "\"}");
    for (int i = 0; i < j.pages; ++i) {
      body.add(",{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"");
      body.blob(j.jpegs[i], j.lengths[i]);
      body.add("\"}}");
    }
    if (voice) {
      body.add(",{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
      body.blob(j.wav, j.wavLength);
      body.add("\"}}");
    }
    // Thinking tokens count toward the output budget; leave room for both.
    body.add(String("]}],\"generationConfig\":{\"maxOutputTokens\":8192,\"thinkingConfig\":{\"thinkingLevel\":\"") +
             j.options.effort + "\"}}}");
  } else {
    // Chat Completions: reasoning_effort and max_completion_tokens (max_tokens is deprecated).
    body.add(String("{\"model\":\"") + j.options.model + "\",\"reasoning_effort\":\"" + j.options.effort +
             "\",\"max_completion_tokens\":8000,\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":[{"
             "\"type\":\"text\",\"text\":\"" +
             prompt + "\"}");
    for (int i = 0; i < j.pages; ++i) {
      body.add(",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
      body.blob(j.jpegs[i], j.lengths[i]);
      body.add("\",\"detail\":\"high\"}}");
    }
    body.add("]}]}");
  }
}
void buildGeminiTranscribeBody(const Job &j, Body &body) {
  body.add(String("{\"contents\":[{\"parts\":[{\"text\":\"") + jsonEscape(TRANSCRIBE_PROMPT) +
           "\"},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"");
  body.blob(j.wav, j.wavLength);
  body.add(
      "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":1024,\"thinkingConfig\":{\"thinkingLevel\":\"minimal\"}}}");
}

// Extract the text of a successful response.
String responseText(Kind kind, bool gemini, JsonDocument &doc, String &finish) {
  String text;
  if (kind == Kind::Transcribe && !gemini) {
    const char *t = doc["text"];
    return t ? String(t) : String();
  }
  if (gemini) {
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
      if (part["thought"] == true) continue;
      const char *t = part["text"];
      if (t) {
        if (text.length()) text += '\n';
        text += t;
      }
    }
    finish = doc["candidates"][0]["finishReason"] | "";
  } else {
    const char *t = doc["choices"][0]["message"]["content"];
    if (t) text = t;
    finish = doc["choices"][0]["finish_reason"] | "";
  }
  return text;
}

bool request(Job &j) {
  String &answer = j.result;
  auto fail = [&](const char *message) {
    answer = message;
    return false;
  };
  const bool gemini = j.options.gemini;
  if (j.options.key.isEmpty()) return fail("Save an API key for the selected provider in PC setup.");
  for (int i = 0; i < j.pages; ++i) {
    const uint8_t *p = j.jpegs[i];
    const size_t n = j.lengths[i];
    if (!p || n < 4 || p[0] != 0xff || p[1] != 0xd8 || p[n - 2] != 0xff || p[n - 1] != 0xd9)
      return fail("Incomplete photo. Capture again.");
  }
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_WAIT_MS && !cancelled) delay(100);
  if (cancelled) return fail("Cancelled. Nothing was sent.");
  if (WiFi.status() != WL_CONNECTED) return fail("Not connected to Wi-Fi. Nothing was sent.");
  if (time(nullptr) < 1700000000) {
    // The clock is started at boot by networkBegin(); just give it a moment.
    start = millis();
    while (time(nullptr) < 1700000000 && millis() - start < 8000) delay(50);
    if (time(nullptr) < 1700000000)
      return fail("Internet clock unavailable. Wi-Fi may need a login page. Nothing was sent.");
  }
  // Build the body once; retries resend the same bytes.
  size_t total = 0;
  uint8_t *payload = nullptr;
  String contentType = "application/json";
  const char *boundary = "----perch7f3a9c";
  if (j.kind == Kind::Transcribe && !gemini) {
    payload = multipartWav(j.wav, j.wavLength, "gpt-transcribe", boundary, total);
    contentType = String("multipart/form-data; boundary=") + boundary;
  } else {
    if (j.kind == Kind::Answer && gemini && !j.turns.empty()) attachFiles(j);
    Body body;
    if (j.kind == Kind::Transcribe) buildGeminiTranscribeBody(j, body);
    else buildAnswerBody(j, body);
    payload = body.build(total);
  }
  if (!payload) return fail("Not enough memory for the request.");
  if (cancelled) {
    free(payload);
    return fail("Cancelled. Nothing was sent.");
  }
  if (flashBusy) {
    const uint32_t waitStart = millis();
    while (flashBusy() && millis() - waitStart < 10000 && !cancelled) delay(50);
    Serial.printf("AI_WAIT photo save %lu ms\n", (unsigned long)(millis() - waitStart));
  }
  // Which models to try, in order. Gemini starts with the chosen model, then the others
  // (daily free limits are per model). GPT uses only the chosen model: it is paid.
  std::vector<String> models;
  if (gemini) {
    if (j.kind == Kind::Transcribe) {
      models.push_back("gemini-3.5-flash-lite");
      models.push_back("gemini-3.5-flash");
    } else {
      models.push_back(j.options.model.length() ? j.options.model : String(GEMINI_MODELS[0].id));
      for (int i = 0; i < GEMINI_MODEL_COUNT; ++i)
        if (models[0] != GEMINI_MODELS[i].id && i < 3) models.push_back(GEMINI_MODELS[i].id);
    }
  } else models.push_back(j.kind == Kind::Transcribe ? String("gpt-transcribe") : j.options.model);
  // Only requests the provider cannot have processed are retried:
  //  - 500/503 overloaded (Gemini): the next model after a pause;
  //  - 429 per-minute limit: wait as long as asked (if short), same model;
  //  - 429 per-day limit (Gemini): the next model;
  //  - -1 never connected / -3 body cut off mid-send: same model once more.
  constexpr int MAX_TRIES = 4;
  std::unique_ptr<WiFiClientSecure> client;
  HTTPClient http;
  int status = -1, tries = 0;
  size_t model = 0;
  uint32_t waitMs = 0;
  bool dailyLimit = false;
  int retryAfterS = 0;
  bool resent = false;
  bool inlineRetry = false;
  String errorBody;
  while (tries < MAX_TRIES && !cancelled) {
    if (tries) {
      http.end();
      const uint32_t until = millis() + waitMs;
      while (millis() < until && !cancelled) delay(100);
      if (cancelled) break;
    }
    ++tries;
    // A fresh connection per attempt: reusing one after an error hung until the read timeout.
    client.reset(new WiFiClientSecure());
    client->setCACert(AI_ROOT_CERTS);
    client->setHandshakeTimeout(12);
    http.setConnectTimeout(12000);
    http.setTimeout(65000);  // uint16_t ms: 65 s is the maximum
    String url;
    if (gemini)
      url = String("https://generativelanguage.googleapis.com/v1beta/models/") + models[model] +
            (j.kind == Kind::Answer ? ":streamGenerateContent?alt=sse" : ":generateContent");
    else
      url = j.kind == Kind::Transcribe ? "https://api.openai.com/v1/audio/transcriptions"
                                       : "https://api.openai.com/v1/chat/completions";
    if (!http.begin(*client, url)) {
      status = -1;
      break;
    }
    http.addHeader("Content-Type", contentType);
    http.addHeader(gemini ? "x-goog-api-key" : "Authorization", gemini ? j.options.key : "Bearer " + j.options.key);
    Serial.printf("AI_SEND try %d %s %s %u bytes, internal heap %u free, largest %u\n", tries, models[model].c_str(),
                  j.kind == Kind::Transcribe ? "transcribe" : j.options.effort.c_str(), (unsigned)total,
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const uint32_t sendStart = millis();
    countRequest(gemini);
    status = http.POST(payload, total);
    Serial.printf("AI_STATUS %d after %lu ms\n", status, (unsigned long)(millis() - sendStart));
    if (status == 200) break;
    // Error bodies are small JSON and never contain the key.
    errorBody = status > 0 ? http.getString().substring(0, 2000) : String("");
    String flat = errorBody.substring(0, 600);
    flat.replace("\n", " ");
    flat.replace("  ", " ");
    Serial.printf("AI_HTTP %d %s\n", status, flat.c_str());
    if ((status == 400 || status == 403 || status == 404) && gemini && !inlineRetry) {
      bool usedLinks = false;
      for (int i = 0; i < j.pages; ++i)
        if (j.fileUris[i].length()) {
          usedLinks = true;
          photoUriForget(j.photoIds[i]);
          j.fileUris[i] = "";
        }
      if (usedLinks) {
        inlineRetry = true;
        Body body;
        buildAnswerBody(j, body);
        uint8_t *inlinePayload = body.build(total);
        if (inlinePayload) {
          free(payload);
          payload = inlinePayload;
          waitMs = 0;
          Serial.println("AI_RETRY photos inline");
          continue;
        }
      }
    }
    if (status == 429) {
      dailyLimit = errorBody.indexOf("PerDay") >= 0;
      const int at = errorBody.indexOf("\"retryDelay\"");
      retryAfterS = at >= 0 ? errorBody.substring(errorBody.indexOf('"', at + 13) + 1).toInt() : 0;
      if (!dailyLimit && retryAfterS > 0 && retryAfterS <= 20) {
        waitMs = retryAfterS * 1000UL + 500;
        continue;
      }
      if (gemini && model + 1 < models.size()) {
        ++model;
        waitMs = 0;
        continue;
      }
      break;
    }
    if (gemini && (status == 503 || status == 500)) {
      model = (model + 1) % models.size();
      waitMs = tries == 1 ? 1000 : 4000;
      continue;
    }
    if ((status == HTTPC_ERROR_CONNECTION_REFUSED || status == HTTPC_ERROR_SEND_PAYLOAD_FAILED) && !resent) {
      resent = true;
      waitMs = 1000;
      // The upload broke part way (weak Wi-Fi): resend with half-size photos.
      if (status == HTTPC_ERROR_SEND_PAYLOAD_FAILED && j.kind == Kind::Answer) {
        bool shrunk = false;
        for (int i = 0; i < j.pages; ++i) {
          uint8_t *small = nullptr;
          size_t smallLen = 0;
          if (shrinkJpeg(j.jpegs[i], j.lengths[i], small, smallLen)) {
            free(j.jpegs[i]);
            j.jpegs[i] = small;
            j.lengths[i] = smallLen;
            shrunk = true;
          }
        }
        if (shrunk) {
          Body body;
          buildAnswerBody(j, body);
          uint8_t *smaller = body.build(total);
          if (smaller) {
            free(payload);
            payload = smaller;
          }
          Serial.printf("AI_RETRY smaller photos: %u bytes\n", (unsigned)total);
        }
      }
      continue;
    }
    break;
  }
  free(payload);
  if (status != 200) {
    http.end();
    const String &body = errorBody;
    if (body.indexOf("API_KEY_INVALID") >= 0 || body.indexOf("API key not valid") >= 0 ||
        body.indexOf("invalid_api_key") >= 0 || status == 401)
      return fail("API key rejected. Check the key in PC setup.");
    if (status == 403) return fail("Access denied for this API key.");
    if (status == 429) {
      if (body.indexOf("insufficient_quota") >= 0) return fail("The OpenAI account has no credit left.");
      if (dailyLimit)
        return fail(
            "Today's free Gemini limit is used up on every model. It resets at midnight Pacific time; a paid key "
            "removes the cap.");
      answer = String("Too many requests right now. Try again in ") +
               (retryAfterS > 0 ? String(retryAfterS) + " s." : String("a minute."));
      return false;
    }
    if (status == 404 || status == 400) {
      if (body.indexOf("model") >= 0)
        return fail("This model is not available for this key. Pick another in Settings.");
      answer = String("The provider rejected the request (") + status + ").";
      return false;
    }
    if (status == 503 || status == 500) {
      answer = String("The AI service is busy (") + status + "). Try again in a minute.";
      return false;
    }
    if (status < 0) {
      char tlsError[100] = "";
      if (client) client->lastError(tlsError, sizeof tlsError);
      Serial.printf("AI_TLS %s\n", tlsError);
      answer = String("Connection lost (") + HTTPClient::errorToString(status) +
               "). The photo may or may not have reached the provider; it was not resent.";
      return false;
    }
    answer = "AI service returned HTTP " + String(status) + ".";
    return false;
  }
  if (j.kind == Kind::Answer) {
    // Streamed: text appears on the device as it is written.
    SseSink sink(gemini);
    const int received = http.writeToStream(&sink);
    http.end();
    if (sink.line.length()) sink.handle();
    if (sink.error.length() && sink.text.isEmpty()) {
      answer = "The provider reported an error: " + sink.error;
      return false;
    }
    if (received < 0 && sink.text.isEmpty()) return fail("Connection lost while answering. Try again.");
    answer = toDisplayText(sink.text);
    const bool cutOff = sink.finish == "MAX_TOKENS" || sink.finish == "length";
    if (answer.isEmpty()) {
      if (sink.blocked) return fail("The provider blocked this photo. Nothing to show.");
      if (cutOff) return fail("The model ran out of space before answering. Try a lower effort.");
      if (sink.finish == "SAFETY" || sink.finish == "content_filter") return fail("The provider blocked this answer.");
      return fail("No text answer returned. The image may have been unreadable.");
    }
    if (answer.length() > ANSWER_LIMIT) answer = answer.substring(0, ANSWER_LIMIT);
    if (cutOff || answer.length() >= ANSWER_LIMIT) answer += "\n\n[Answer cut off]";
    if (received < 0) answer += "\n\n[Connection lost before the end]";
    return true;
  }
  char *response = (char *)ps_malloc(RESPONSE_LIMIT + 1);
  if (!response) {
    http.end();
    return fail("Not enough memory for the AI response.");
  }
  BoundedResponse output(response);
  int received = http.writeToStream(&output);
  http.end();
  response[output.used] = 0;
  if (received < 0 || output.overflow) {
    free(response);
    return fail("AI response was incomplete or too large.");
  }
  JsonDocument doc;
  auto error = deserializeJson(doc, response, output.used);
  free(response);
  if (error) return fail("Could not read the AI response.");
  String finish;
  answer = responseText(j.kind, gemini, doc, finish);
  if (j.kind == Kind::Transcribe) {
    answer = toDisplayText(answer);
    answer.trim();
    if (answer.isEmpty()) return fail("Could not hear any words. Hold the button and speak again.");
    return true;
  }
  if (answer.isEmpty() && gemini && doc["promptFeedback"]["blockReason"].is<const char *>())
    return fail("The provider blocked this photo. Nothing to show.");
  const bool cutOff = finish == "MAX_TOKENS" || finish == "length";
  if (answer.isEmpty()) {
    if (cutOff) return fail("The model ran out of space before answering. Try a lower effort.");
    if (finish == "SAFETY" || finish == "content_filter") return fail("The provider blocked this answer.");
    return fail("No text answer returned. The image may have been unreadable.");
  }
  answer = toDisplayText(answer);
  if (answer.length() > ANSWER_LIMIT) answer = answer.substring(0, ANSWER_LIMIT);
  if (cutOff || answer.length() >= ANSWER_LIMIT) answer += "\n\n[Answer cut off]";
  return true;
}

// Requests sent today, per provider (every attempt counts: limits count attempts too).
Preferences usage;
void countRequest(bool gemini) {
  const time_t now = time(nullptr);
  const int day = now > 1700000000 ? (int)(now / 86400) : 0;
  if (usage.getInt("day", -1) != day) {
    usage.putInt("day", day);
    usage.putUShort("g", 0);
    usage.putUShort("o", 0);
  }
  const char *key = gemini ? "g" : "o";
  usage.putUShort(key, usage.getUShort(key, 0) + 1);
}

// Request slot shared between the UI loop and the worker task.
portMUX_TYPE slotLock = portMUX_INITIALIZER_UNLOCKED;
volatile AiState state = AiState::Idle;
TaskHandle_t worker = nullptr;
Job *job = nullptr;

void workerTask(void *) {
  job->ok = request(*job);
  job->release();
  portENTER_CRITICAL(&slotLock);
  state = cancelled ? AiState::Cancelled : (job->ok ? AiState::Done : AiState::Failed);
  worker = nullptr;
  portEXIT_CRITICAL(&slotLock);
  vTaskDelete(nullptr);
}

uint8_t *copyOf(const uint8_t *data, size_t length) {
  uint8_t *p = (uint8_t *)ps_malloc(length);
  if (p) memcpy(p, data, length);
  return p;
}

bool launch() {
  portENTER_CRITICAL(&partialLock);
  partialText = "";
  ++partialVersion;
  portEXIT_CRITICAL(&partialLock);
  cancelled = false;
  state = AiState::Working;
  // TLS needs a deep stack; run beside the Wi-Fi stack on core 0, away from the UI loop.
  if (xTaskCreatePinnedToCore(workerTask, "ai-request", 16384, nullptr, 1, &worker, 0) != pdPASS) {
    job->release();
    state = AiState::Idle;
    return false;
  }
  return true;
}

}  // namespace

bool aiBusy() { return state != AiState::Idle; }

bool aiStart(const AiOptions &options, const uint8_t *const *jpegs, const size_t *lengths, int pages,
             const String &question, const String &history, const uint8_t *wav, size_t wavLength) {
  if (state != AiState::Idle || pages < 0 || pages > AI_MAX_PAGES) return false;
  if (!job) job = new Job();
  job->release();
  job->kind = Kind::Answer;
  job->options = options;
  job->question = question;
  job->history = history;
  job->result = "";
  job->ok = false;
  // The worker owns its own copies, so a new capture or recording can never change an upload.
  for (int i = 0; i < pages; ++i) {
    job->jpegs[i] = copyOf(jpegs[i], lengths[i]);
    if (!job->jpegs[i]) {
      job->release();
      return false;
    }
    job->lengths[i] = lengths[i];
  }
  job->pages = pages;
  if (wav && wavLength) {
    job->wav = copyOf(wav, wavLength);
    if (!job->wav) {
      job->release();
      return false;
    }
    job->wavLength = wavLength;
  }
  return launch();
}

bool aiChat(const AiOptions &options, const AiTurn *turns, int turnCount, const uint8_t *const *jpegs,
            const size_t *lengths, const uint32_t *photoIds, int images, const uint8_t *wav, size_t wavLength) {
  if (state != AiState::Idle || turnCount < 1 || images < 0 || images > AI_MAX_PAGES) return false;
  if (!job) job = new Job();
  job->release();
  job->kind = Kind::Answer;
  job->options = options;
  job->result = "";
  job->ok = false;
  for (int i = 0; i < turnCount; ++i) job->turns.push_back(turns[i]);
  for (int i = 0; i < images; ++i) {
    job->jpegs[i] = copyOf(jpegs[i], lengths[i]);
    if (!job->jpegs[i]) {
      job->release();
      return false;
    }
    job->lengths[i] = lengths[i];
    job->photoIds[i] = photoIds ? photoIds[i] : 0;
  }
  job->pages = images;
  if (wav && wavLength) {
    job->wav = copyOf(wav, wavLength);
    if (!job->wav) {
      job->release();
      return false;
    }
    job->wavLength = wavLength;
  }
  return launch();
}

bool aiTranscribe(const AiOptions &options, const uint8_t *wav, size_t wavLength) {
  if (state != AiState::Idle || !wav || !wavLength) return false;
  if (!job) job = new Job();
  job->release();
  job->kind = Kind::Transcribe;
  job->options = options;
  job->result = "";
  job->ok = false;
  job->wav = copyOf(wav, wavLength);
  if (!job->wav) return false;
  job->wavLength = wavLength;
  return launch();
}

void aiCancel() {
  if (state == AiState::Working) cancelled = true;
}

uint32_t aiPartial(String &text) {
  portENTER_CRITICAL(&partialLock);
  text = partialText;
  const uint32_t v = partialVersion;
  portEXIT_CRITICAL(&partialLock);
  return v;
}

AiState aiPoll(String &text) {
  AiState current = state;
  if (current == AiState::Done || current == AiState::Failed) {
    text = job->result;
    job->result = "";
    state = AiState::Idle;
  } else if (current == AiState::Cancelled) {
    job->result = "";
    state = AiState::Idle;
  }
  return current;
}

void aiDryRun(const AiOptions &options, int pages, const String &question, bool transcribe, const String &history) {
  // A minimal valid JPEG/WAV stand-in: the shape of the body is what is checked.
  static const uint8_t fakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0xFF, 0xD9};
  static const uint8_t fakeWav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  Job j;
  j.kind = transcribe ? Kind::Transcribe : Kind::Answer;
  j.options = options;
  j.question = question;
  j.history = history;
  j.pages = transcribe ? 0 : constrain(pages, 1, AI_MAX_PAGES);
  for (int i = 0; i < j.pages; ++i) {
    j.jpegs[i] = (uint8_t *)fakeJpeg;
    j.lengths[i] = sizeof fakeJpeg;
  }
  if (transcribe) {
    j.wav = (uint8_t *)fakeWav;
    j.wavLength = sizeof fakeWav;
  }
  if (transcribe && !options.gemini) {
    size_t total = 0;
    uint8_t *body = multipartWav(fakeWav, sizeof fakeWav, "gpt-transcribe", "----perch7f3a9c", total);
    Serial.printf("DRYRUN POST https://api.openai.com/v1/audio/transcriptions multipart %u bytes\n", (unsigned)total);
    if (body) {
      for (size_t i = 0; i < total; ++i) {
        const char c = body[i];
        Serial.print(c >= 32 && c < 127 ? c : (c == '\n' ? '|' : '.'));
      }
      Serial.println();
    }
    free(body);
    j.wav = nullptr;
    return;
  }
  Body body;
  if (transcribe) buildGeminiTranscribeBody(j, body);
  else buildAnswerBody(j, body);
  size_t total = 0;
  uint8_t *buf = body.build(total);
  if (!buf) {
    Serial.println("DRYRUN build failed");
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, (const char *)buf, total);
  String shape;
  serializeJson(doc, shape);
  free(buf);
  const String url = options.gemini
                         ? String("https://generativelanguage.googleapis.com/v1beta/models/") +
                               (transcribe ? "gemini-3.5-flash-lite" : options.model.c_str()) + ":generateContent"
                         : String("https://api.openai.com/v1/chat/completions");
  Serial.printf("DRYRUN POST %s json %s, %u bytes\n", url.c_str(), err ? err.c_str() : "valid", (unsigned)total);
  Serial.printf("DRYRUN %s\n", shape.substring(0, 1500).c_str());
  for (int i = 0; i < AI_MAX_PAGES; ++i) j.jpegs[i] = nullptr;
  j.wav = nullptr;
}

// Developer: the photo upload path without an API key. Google rejects it unauthenticated,
// so no model runs and nothing is billed; it shows whether TLS and a large upload succeed.
size_t uploadTestBytes = 0;
void uploadTestTask(void *) {
  const size_t bytes = uploadTestBytes;
  const char *head = "{\"contents\":[{\"parts\":[{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
  const char *tail = "\"}}]}]}";
  const size_t total = strlen(head) + bytes + strlen(tail);
  uint8_t *payload = (uint8_t *)ps_malloc(total);
  if (!payload) {
    Serial.println("UPLOAD_TEST no memory");
    vTaskDelete(nullptr);
    return;
  }
  memcpy(payload, head, strlen(head));
  memset(payload + strlen(head), 'A', bytes);
  memcpy(payload + strlen(head) + bytes, tail, strlen(tail));
  WiFiClientSecure client;
  client.setCACert(AI_ROOT_CERTS);
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(45000);
  const uint32_t t = millis();
  int status = -100;
  if (http.begin(client, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent")) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(payload, total);
    http.end();
  }
  free(payload);
  Serial.printf("UPLOAD_TEST status %d (%s) in %lu ms\n", status,
                status < 0 ? HTTPClient::errorToString(status).c_str() : "http", (unsigned long)(millis() - t));
  vTaskDelete(nullptr);
}
// Same core, priority and stack as a real request, so it meets the same conditions.
void aiUploadTest(size_t bytes) {
  uploadTestBytes = bytes;
  xTaskCreatePinnedToCore(uploadTestTask, "upload-test", 16384, nullptr, 1, nullptr, 0);
}

// The prebuilt Arduino SDK forces every TLS allocation into internal RAM, which camera,
// Wi-Fi and Bluetooth leave fragmented (52 KB free but no 17 KB block: "SSL - Memory
// allocation failed"). Large TLS buffers go to PSRAM instead (ESP-IDF supports this as
// CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC); small ones stay internal for speed.
static void *tlsCalloc(size_t count, size_t size) {
  const size_t total = count * size;
  void *p = total > 2048 ? heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  if (!p) p = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p;
}
void aiBegin() {
  mbedtls_platform_set_calloc_free(tlsCalloc, free);
  usage.begin("perch-usage", false);
}
int aiRequestsToday(bool gemini) {
  const time_t now = time(nullptr);
  const int day = now > 1700000000 ? (int)(now / 86400) : 0;
  if (usage.getInt("day", -1) != day) return 0;
  return usage.getUShort(gemini ? "g" : "o", 0);
}

// Developer: is the saved Gemini key valid, and can it see the model? A GET of the model's
// details is free (no content is generated).
void aiKeyCheck(const String &key) {
  WiFiClientSecure client;
  client.setCACert(AI_ROOT_CERTS);
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(20000);
  if (!http.begin(client, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash")) {
    Serial.println("KEYCHECK begin failed");
    return;
  }
  http.addHeader("x-goog-api-key", key);
  const int status = http.GET();
  String body = status > 0 ? http.getString() : String(HTTPClient::errorToString(status));
  http.end();
  body = body.substring(0, 400);
  body.replace("\n", " ");
  Serial.printf("KEYCHECK %d %s\n", status, body.c_str());
}

void aiSetFlashBusy(bool (*busy)()) { flashBusy = busy; }

// Developer: how small and how fast a retry photo would be (nothing is sent).
void aiShrinkTest(const uint8_t *jpeg, size_t len) {
  const uint32_t t = millis();
  uint8_t *out = nullptr;
  size_t outLen = 0;
  const bool ok = shrinkJpeg(jpeg, len, out, outLen);
  Serial.printf("SHRINK %s %u -> %u bytes in %lums\n", ok ? "ok" : "failed", (unsigned)len, (unsigned)outLen,
                (unsigned long)(millis() - t));
  free(out);
}

// Developer: a three-turn conversation (one photo by link, one inline), built and validated
// without sending.
void aiDryRunChat(const AiOptions &options) {
  static const uint8_t fakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0xFF, 0xD9};
  Job j;
  j.options = options;
  AiTurn a;
  a.text = "What is on this page?";
  a.images = 1;
  AiTurn b;
  b.user = false;
  b.text = "Answer: a quiz on runtime\nSteps:\n1. ...";
  AiTurn c;
  c.text = "Solve question 2";
  c.images = 1;
  j.turns = {a, b, c};
  j.pages = 2;
  for (int i = 0; i < 2; ++i) {
    j.jpegs[i] = (uint8_t *)fakeJpeg;
    j.lengths[i] = sizeof fakeJpeg;
  }
  if (options.gemini) j.fileUris[0] = "https://generativelanguage.googleapis.com/v1beta/files/example";
  Body body;
  buildAnswerBody(j, body);
  size_t total = 0;
  uint8_t *buf = body.build(total);
  for (int i = 0; i < AI_MAX_PAGES; ++i) j.jpegs[i] = nullptr;
  if (!buf) {
    Serial.println("DRYRUN build failed");
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, (const char *)buf, total);
  String shape;
  serializeJson(doc, shape);
  free(buf);
  Serial.printf("DRYRUN chat %s json %s, %u bytes\n", options.gemini ? "gemini" : "gpt", err ? err.c_str() : "valid",
                (unsigned)total);
  Serial.printf("DRYRUN %s\n", shape.substring(0, 1800).c_str());
}
