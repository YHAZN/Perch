#include "ai_client.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "wifi_secrets.h"
#include "ai_root_certs.h"

namespace {
constexpr size_t RESPONSE_LIMIT = 32768;
constexpr size_t ANSWER_LIMIT = 10000;
constexpr unsigned long WIFI_WAIT_MS = 15000;
const char *GUEST_SSID = "guest";
// Set by the UI; checked before anything leaves the device.
volatile bool cancelled = false;

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

bool request(bool gemini, const String &key, const uint8_t *jpeg, size_t length, String &answer) {
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
  String prefix = gemini ? String("{\"contents\":[{\"parts\":[{\"text\":\"") + PROMPT +
                               "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\""
                         : String(
                               "{\"model\":\"gpt-4.1\",\"max_completion_tokens\":1600,\"messages\":[{\"role\":\"user\","
                               "\"content\":[{\"type\":\"text\",\"text\":\"") +
                               PROMPT + "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  String suffix = gemini ? "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":4096}}" : "\",\"detail\":\"high\"}}]}]}";
  size_t encoded = 4 * ((length + 2) / 3), total = prefix.length() + encoded + suffix.length(), written = 0;
  uint8_t *payload = (uint8_t *)ps_malloc(total + 1);
  if (!payload) return fail("Not enough memory for the photo request.");
  memcpy(payload, prefix.c_str(), prefix.length());
  int result = mbedtls_base64_encode(payload + prefix.length(), encoded + 1, &written, jpeg, length);
  if (result || written != encoded) {
    free(payload);
    return fail("Photo encoding failed.");
  }
  memcpy(payload + prefix.length() + encoded, suffix.c_str(), suffix.length());
  payload[total] = 0;
  if (cancelled) {
    free(payload);
    return fail("Cancelled. Nothing was sent.");
  }
  WiFiClientSecure client;
  client.setCACert(AI_ROOT_CERTS);
  client.setHandshakeTimeout(12);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(45000);
  const char *url = gemini ? "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent"
                           : "https://api.openai.com/v1/chat/completions";
  if (!http.begin(client, url)) {
    free(payload);
    return fail("Could not start the HTTPS request.");
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader(gemini ? "x-goog-api-key" : "Authorization", gemini ? key : "Bearer " + key);
  int status = http.POST(payload, total);
  free(payload);
  if (status != 200) {
    // Error bodies are small JSON; read a bounded amount to tell the user what went wrong.
    String body = (status > 0 && http.getSize() > 0 && http.getSize() < 4096) ? http.getString() : "";
    http.end();
    if (body.indexOf("API_KEY_INVALID") >= 0 || body.indexOf("API key not valid") >= 0 || status == 401)
      return fail("API key rejected. Check the key in PC setup.");
    if (status == 403) return fail("Access denied for this API key.");
    if (status == 429) return fail("Provider quota reached. Wait or check the API account.");
    if (status == 404) return fail("Configured AI model unavailable for this account.");
    if (status == 503 || status == 500) return fail("The AI service is busy. Try again in a minute.");
    if (status < 0)
      return fail("Connection lost. The photo may or may not have reached the provider; it was not resent.");
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
  String result;
  bool ok;
};
Job *job = nullptr;

void workerTask(void *) {
  job->ok = request(job->gemini, job->key, job->jpeg, job->length, job->result);
  job->key = "";
  free(job->jpeg);
  job->jpeg = nullptr;
  portENTER_CRITICAL(&slotLock);
  state = cancelled ? AiState::Cancelled : (job->ok ? AiState::Done : AiState::Failed);
  worker = nullptr;
  portEXIT_CRITICAL(&slotLock);
  vTaskDelete(nullptr);
}

unsigned long lastAttempt = 0;
bool tryGuest = false;
void join() {
  // Alternate between the configured network and the open campus guest network.
  if (tryGuest) WiFi.begin(GUEST_SSID);
  else WiFi.begin(WIFI_TEST_SSID, WIFI_TEST_PASSWORD);
  tryGuest = !tryGuest;
  lastAttempt = millis();
}
}  // namespace

void networkBegin() {
  // Wi-Fi time replaces any build or USB time once it arrives. SNTP retries until online.
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "time.google.com", "pool.ntp.org");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  join();
}

bool radioOn = true;
void networkTick() {
  if (radioOn && WiFi.status() != WL_CONNECTED && millis() - lastAttempt > 20000) {
    WiFi.disconnect();
    join();
  }
}

void networkSetEnabled(bool on) {
  // Control Center Wi-Fi toggle: off turns the radio off entirely (saves power).
  radioOn = on;
  if (on) {
    WiFi.mode(WIFI_STA);
    join();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool networkEnabled() { return radioOn; }

bool networkConnected() { return WiFi.status() == WL_CONNECTED; }

bool aiBusy() { return state != AiState::Idle; }

bool aiStart(bool gemini, const String &key, const uint8_t *jpeg, size_t length) {
  if (state != AiState::Idle) return false;
  if (!job) job = new Job();
  // The worker owns its own copy, so a new capture can never change an upload in flight.
  job->jpeg = (uint8_t *)ps_malloc(length);
  if (!job->jpeg) return false;
  memcpy(job->jpeg, jpeg, length);
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
