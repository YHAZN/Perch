#include "ai_client.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <time.h>
#include <memory>
#include "wifi_secrets.h"
#include "ai_root_certs.h"

namespace {
constexpr size_t RESPONSE_LIMIT = 32768;
constexpr size_t ANSWER_LIMIT = 10000;
constexpr unsigned long WIFI_WAIT_MS = 15000;
// Set by the UI; checked before anything leaves the device.
volatile bool cancelled = false;
// Set by the UI: true while a photo is still being written to flash. Flash writes stall
// the cache Wi-Fi runs from, and an upload started meanwhile failed mid-send.
bool (*flashBusy)() = nullptr;

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
// With a spoken question the photo is context for what the user asks out loud.
const char *VOICE_PROMPT =
    "The attached audio is the user's spoken question about the attached photo. Answer that question using the photo. "
    "If the audio is unclear, say what you could not hear and answer what you can about the photo; never invent "
    "missing content. Give the answer first, then concise reasoning. Use plain text suitable for a small display, "
    "ASCII math, and no Markdown tables.";
const char *PROMPT =
    "Read the photographed problem carefully and solve it. If symbols or text are unclear, say exactly what cannot be "
    "read and ask for a closer photo; never invent missing content. Give the answer first, then concise reasoning. Use "
    "plain text suitable for a small display, ASCII math, and no Markdown tables.";

// The display fonts only cover printable ASCII. Models still send Unicode math and
// typography, which would silently vanish and can change the meaning of an answer.
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
      // Drop Markdown emphasis markers; keep everything else printable.
      if (c == '*' && i + 1 < input.length() && s[i + 1] == '*') {
        i += 2;
        continue;
      }
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

bool request(bool gemini, const String &key, const uint8_t *jpeg, size_t length, const uint8_t *wav, size_t wavLength,
             String &answer) {
  auto fail = [&](const char *message) {
    answer = message;
    return false;
  };
  if (key.isEmpty()) return fail("Save an API key for the selected provider in PC setup.");
  if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9)
    return fail("Incomplete photo. Capture again.");
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
  // Voice goes to Gemini only; GPT's chat endpoint used here takes images, not audio.
  const bool voice = gemini && wav && wavLength;
  String prefix = gemini ? String("{\"contents\":[{\"parts\":[{\"text\":\"") + (voice ? VOICE_PROMPT : PROMPT) +
                               "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\""
                         : String(
                               "{\"model\":\"gpt-4.1\",\"max_completion_tokens\":1600,\"messages\":[{\"role\":\"user\","
                               "\"content\":[{\"type\":\"text\",\"text\":\"") +
                               PROMPT + "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  String suffix =
      gemini ? "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":4096,\"thinkingConfig\":{\"thinkingLevel\":\"low\"}}}"
             : "\",\"detail\":\"high\"}}]}]}";
  const String middle = "\"}},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
  size_t encoded = 4 * ((length + 2) / 3), audioEncoded = voice ? 4 * ((wavLength + 2) / 3) : 0;
  size_t total = prefix.length() + encoded + (voice ? middle.length() + audioEncoded : 0) + suffix.length(),
         written = 0;
  uint8_t *payload = (uint8_t *)ps_malloc(total + 1);
  if (!payload) return fail("Not enough memory for the photo request.");
  uint8_t *at = payload;
  memcpy(at, prefix.c_str(), prefix.length());
  at += prefix.length();
  int result = mbedtls_base64_encode(at, encoded + 1, &written, jpeg, length);
  if (result || written != encoded) {
    free(payload);
    return fail("Photo encoding failed.");
  }
  at += encoded;
  if (voice) {
    memcpy(at, middle.c_str(), middle.length());
    at += middle.length();
    result = mbedtls_base64_encode(at, audioEncoded + 1, &written, wav, wavLength);
    if (result || written != audioEncoded) {
      free(payload);
      return fail("Voice encoding failed.");
    }
    at += audioEncoded;
  }
  memcpy(at, suffix.c_str(), suffix.length());
  payload[total] = 0;
  if (cancelled) {
    free(payload);
    return fail("Cancelled. Nothing was sent.");
  }
  if (flashBusy) {
    const uint32_t waitStart = millis();
    while (flashBusy() && millis() - waitStart < 10000 && !cancelled) delay(50);
    Serial.printf("AI_WAIT photo save %lu ms\n", (unsigned long)(millis() - waitStart));
  }
  // Gemini attempts: the configured model, then a sibling model, then the first again.
  // 500/503 mean Google did not process the request (overloaded; not billed), so only those
  // are retried; any other error may mean the request was used, and is never resent.
  // Model names and thinking levels: ai.google.dev/gemini-api/docs/generate-content/thinking
  static const char *GEMINI_URLS[] = {
      "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent",
      "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.7-flash:generateContent",
      "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent"};
  static const uint16_t WAIT_BEFORE_MS[] = {0, 1000, 4000};
  const int attempts = gemini ? 3 : 1;
  std::unique_ptr<WiFiClientSecure> client;
  HTTPClient http;
  int status = -1;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (attempt) {
      // Retry only when Google cannot have processed the request: overloaded (500/503),
      // never connected (-1), or the body was cut off mid-send (-3, incomplete JSON).
      if (status != 503 && status != 500 && status != HTTPC_ERROR_CONNECTION_REFUSED &&
          status != HTTPC_ERROR_SEND_PAYLOAD_FAILED)
        break;
      http.end();
      const uint32_t until = millis() + WAIT_BEFORE_MS[attempt];
      while (millis() < until && !cancelled) delay(100);
      if (cancelled) break;
    }
    // A fresh connection per attempt: reusing one after an error hung until the read timeout.
    client.reset(new WiFiClientSecure());
    client->setCACert(AI_ROOT_CERTS);
    client->setHandshakeTimeout(12);
    http.setConnectTimeout(12000);
    http.setTimeout(65000);  // uint16_t ms: 65 s is the maximum (90000 wrapped to 24.5 s)
    const char *url = gemini ? GEMINI_URLS[attempt] : "https://api.openai.com/v1/chat/completions";
    if (!http.begin(*client, url)) {
      status = -1;
      break;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader(gemini ? "x-goog-api-key" : "Authorization", gemini ? key : "Bearer " + key);
    Serial.printf("AI_SEND try %d %s %u bytes, internal heap %u free, largest %u\n", attempt + 1,
                  gemini ? (attempt == 1 ? "gemini-3.7-flash" : "gemini-3.8-flash") : "gpt-4.1", (unsigned)total,
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    const uint32_t sendStart = millis();
    status = http.POST(payload, total);
    Serial.printf("AI_STATUS %d after %lu ms\n", status, (unsigned long)(millis() - sendStart));
    if (status == 503 || status == 500) {
      String reason = http.getString().substring(0, 300);
      reason.replace("\n", " ");
      Serial.printf("AI_BUSY %s\n", reason.c_str());
    }
  }
  free(payload);
  if (status != 200) {
    // Error bodies are small JSON; read a bounded amount to tell the user what went wrong.
    // Google sends error bodies chunked (size unknown, -1); they are small JSON either way.
    String body = (status > 0 && http.getSize() < 4096) ? http.getString() : "";
    http.end();
    // Provider error bodies carry a reason, never the key; log it for diagnosis.
    String flat = body.substring(0, 600);
    flat.replace("\n", " ");
    flat.replace("  ", " ");
    Serial.printf("AI_HTTP %d %s\n", status, flat.c_str());
    if (body.indexOf("API_KEY_INVALID") >= 0 || body.indexOf("API key not valid") >= 0 || status == 401)
      return fail("API key rejected. Check the key in PC setup.");
    if (status == 403) return fail("Access denied for this API key.");
    if (status == 429) return fail("Provider quota reached. Wait or check the API account.");
    if (status == 404) return fail("Configured AI model unavailable for this account.");
    if (status == 503 || status == 500) {
      answer = String("The AI service is busy (") + status + "). Try again in a minute.";
      return false;
    }
    if (status < 0) {
      char tlsError[100] = "";
      if (client) client->lastError(tlsError, sizeof tlsError);
      Serial.printf("AI_TLS %s\n", tlsError);
      answer = String("Connection lost (") + HTTPClient::errorToString(status) +
               (tlsError[0] ? String(": ") + tlsError : String("")) +
               "). The photo may or may not have reached the provider; it was not resent.";
      return false;
    }
    answer = "AI service returned HTTP " + String(status) + ".";
    return false;
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
  answer = "";
  String finish;
  if (gemini) {
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
      if (part["thought"] == true) continue;
      const char *text = part["text"];
      if (text) {
        if (answer.length()) answer += '\n';
        answer += text;
      }
    }
    finish = doc["candidates"][0]["finishReason"] | "";
    if (answer.isEmpty() && doc["promptFeedback"]["blockReason"].is<const char *>())
      return fail("The provider blocked this photo. Nothing to show.");
  } else {
    const char *text = doc["choices"][0]["message"]["content"];
    if (text) answer = text;
    finish = doc["choices"][0]["finish_reason"] | "";
  }
  const bool cutOff = finish == "MAX_TOKENS" || finish == "length";
  if (answer.isEmpty()) {
    if (cutOff) return fail("The model ran out of space before answering. Try again.");
    if (finish == "SAFETY" || finish == "content_filter") return fail("The provider blocked this answer.");
    return fail("No text answer returned. The image may have been unreadable.");
  }
  answer = toDisplayText(answer);
  if (answer.length() > ANSWER_LIMIT) answer = answer.substring(0, ANSWER_LIMIT);
  if (cutOff || answer.length() >= ANSWER_LIMIT) answer += "\n\n[Answer cut off]";
  return true;
}

// Request slot shared between the UI loop and the worker task.
portMUX_TYPE slotLock = portMUX_INITIALIZER_UNLOCKED;
volatile AiState state = AiState::Idle;
TaskHandle_t worker = nullptr;
struct Job {
  bool gemini;
  String key;
  uint8_t *jpeg;
  size_t length;
  uint8_t *wav;
  size_t wavLength;
  String result;
  bool ok;
};
Job *job = nullptr;

void workerTask(void *) {
  job->ok = request(job->gemini, job->key, job->jpeg, job->length, job->wav, job->wavLength, job->result);
  job->key = "";
  free(job->jpeg);
  job->jpeg = nullptr;
  free(job->wav);
  job->wav = nullptr;
  portENTER_CRITICAL(&slotLock);
  state = cancelled ? AiState::Cancelled : (job->ok ? AiState::Done : AiState::Failed);
  worker = nullptr;
  portEXIT_CRITICAL(&slotLock);
  vTaskDelete(nullptr);
}

}  // namespace

bool aiBusy() { return state != AiState::Idle; }

bool aiStart(bool gemini, const String &key, const uint8_t *jpeg, size_t length, const uint8_t *wav, size_t wavLength) {
  if (state != AiState::Idle) return false;
  if (!job) job = new Job();
  // The worker owns its own copies, so a new capture or recording can never change an upload.
  job->jpeg = (uint8_t *)ps_malloc(length);
  if (!job->jpeg) return false;
  memcpy(job->jpeg, jpeg, length);
  job->wav = nullptr;
  job->wavLength = 0;
  if (wav && wavLength) {
    job->wav = (uint8_t *)ps_malloc(wavLength);
    if (!job->wav) {
      free(job->jpeg);
      job->jpeg = nullptr;
      return false;
    }
    memcpy(job->wav, wav, wavLength);
    job->wavLength = wavLength;
  }
  job->length = length;
  job->gemini = gemini;
  job->key = key;
  job->result = "";
  job->ok = false;
  cancelled = false;
  state = AiState::Working;
  // TLS needs a deep stack; run beside the Wi-Fi stack on core 0, away from the UI loop.
  if (xTaskCreatePinnedToCore(workerTask, "ai-request", 16384, nullptr, 1, &worker, 0) != pdPASS) {
    free(job->jpeg);
    job->jpeg = nullptr;
    free(job->wav);
    job->wav = nullptr;
    state = AiState::Idle;
    return false;
  }
  return true;
}

void aiCancel() {
  if (state == AiState::Working) cancelled = true;
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
  Serial.printf("UPLOAD_TEST %u bytes, internal heap %u, largest %u\n", (unsigned)total,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  IPAddress resolved;
  const int dns = WiFi.hostByName("generativelanguage.googleapis.com", resolved);
  Serial.printf("UPLOAD_TEST dns=%d ip=%s stack_free=%u\n", dns, resolved.toString().c_str(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  const uint32_t t = millis();
  int status = -100;
  if (http.begin(client, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent")) {
    http.addHeader("Content-Type", "application/json");
    status = http.POST(payload, total);
    http.end();
  }
  free(payload);
  char tlsError[100] = "";
  client.lastError(tlsError, sizeof tlsError);
  Serial.printf("UPLOAD_TEST tls: %s\n", tlsError);
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
void aiBegin() { mbedtls_platform_set_calloc_free(tlsCalloc, free); }

// Developer: is the saved key valid, and can it see the model? A GET of the model's details
// is free (no content is generated). Prints the HTTP status and the start of the reply.
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
  const String body = status > 0 ? http.getString() : String(HTTPClient::errorToString(status));
  http.end();
  Serial.printf("KEYCHECK %d %s\n", status, body.substring(0, 400).c_str());
}

void aiSetFlashBusy(bool (*busy)()) { flashBusy = busy; }
