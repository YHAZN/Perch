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
}  // namespace

bool requestImageAnswer(bool gemini, const String &key, const uint8_t *jpeg, size_t length, String &answer) {
  auto fail = [&](const char *message) {
    answer = message;
    return false;
  };
  if (key.isEmpty()) return fail("Save an API key for the selected provider in PC setup.");
  if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9)
    return fail("Incomplete photo. Capture again.");
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_TEST_SSID, WIFI_TEST_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(50);
    if (WiFi.status() != WL_CONNECTED)
      return fail("Wi-Fi unavailable. Connect the configured hotspot or use Wi-Fi setup.");
  }
  if (time(nullptr) < 1700000000) {
    configTime(0, 0, "time.google.com", "pool.ntp.org");
    unsigned long start = millis();
    while (time(nullptr) < 1700000000 && millis() - start < 8000) delay(50);
    if (time(nullptr) < 1700000000) return fail("Internet clock unavailable. Check Wi-Fi internet access.");
  }
  String prefix = gemini ? String("{\"contents\":[{\"parts\":[{\"text\":\"") + PROMPT +
                               "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\""
                         : String(
                               "{\"model\":\"gpt-4.1\",\"max_completion_tokens\":1600,\"messages\":[{\"role\":\"user\","
                               "\"content\":[{\"type\":\"text\",\"text\":\"") +
                               PROMPT + "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
  String suffix = gemini ? "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":2048}}" : "\",\"detail\":\"high\"}}]}]}";
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
    http.end();
    if (status == 401 || status == 403) return fail("API key rejected or access denied. Check provider setup.");
    if (status == 429) return fail("Provider quota reached. Wait or check the API account.");
    if (status == 404) return fail("Configured AI model unavailable for this account.");
    if (status < 0) return fail("HTTPS failed. Check internet access, clock and certificate trust.");
    answer = "AI service returned HTTP " + String(status) + ". Try again later.";
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
  if (gemini) {
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
      if (part["thought"] == true) continue;
      const char *text = part["text"];
      if (text) {
        if (answer.length()) answer += '\n';
        answer += text;
      }
    }
  } else {
    const char *text = doc["choices"][0]["message"]["content"];
    if (text) answer = text;
  }
  if (answer.isEmpty()) return fail("No text answer returned. The image may have been blocked or unreadable.");
  if (answer.length() > 10000) answer = answer.substring(0, 10000) + "\n[Answer shortened]";
  return true;
}
