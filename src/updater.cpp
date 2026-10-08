#include "updater.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include <memory>
#include "update_root_certs.h"
#include "version.h"

namespace {
Preferences store;
volatile UpdateState state = UpdateState::Idle;
volatile int progress = 0;
String available, imageUrl, imageSha, error;
size_t imageSize = 0;

// GET with redirects followed by hand: GitHub release links redirect twice, the second time
// to a different host with a long signed address, which the built-in follower mishandled
// (404). Each hop gets a fresh TLS connection. On return `http` holds the final response.
int openGet(String url, HTTPClient &http, std::unique_ptr<WiFiClientSecure> &client) {
  static const char *keep[] = {"Location"};
  for (int hop = 0; hop < 6; ++hop) {
    http.end();
    client.reset(new WiFiClientSecure());
    client->setCACert(UPDATE_ROOT_CERTS);
    client->setHandshakeTimeout(15);
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.collectHeaders(keep, 1);
    if (!http.begin(*client, url)) return -1;
    const uint32_t started = millis();
    const int status = http.GET();
    Serial.printf("UPDATE GET %d in %lums (hop %d)\n", status, (unsigned long)(millis() - started), hop);
    if (status < 300 || status >= 400) return status;
    url = http.getLocation();
    Serial.printf("UPDATE redirect -> %s\n", url.substring(0, 160).c_str());
    if (!url.startsWith("https://")) return -2;  // never leave HTTPS
  }
  return -3;
}

void fail(const String &why) {
  error = why;
  Serial.printf("UPDATE failed: %s\n", why.c_str());
  state = UpdateState::Failed;
}

void checkTask(void *) {
  std::unique_ptr<WiFiClientSecure> client;
  HTTPClient http;
  const int status = openGet(updaterUrl(), http, client);
  if (status != 200) {
    http.end();
    fail(String("Update check failed (") + status + ").");
    vTaskDelete(nullptr);
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err || !doc["version"].is<const char *>() || !doc["url"].is<const char *>()) {
    fail("The update description is not valid.");
    vTaskDelete(nullptr);
    return;
  }
  available = (const char *)doc["version"];
  imageUrl = (const char *)doc["url"];
  imageSha = doc["sha256"] | "";
  imageSize = doc["size"] | 0;
  imageSha.toLowerCase();
  Serial.printf("UPDATE newest %s, this %s\n", available.c_str(), PERCH_VERSION);
  state = versionNewer(available, PERCH_VERSION) ? UpdateState::Available : UpdateState::UpToDate;
  vTaskDelete(nullptr);
}

void installTask(void *) {
  if (imageSha.length() != 64) {
    fail("The update has no checksum; not installing.");
    vTaskDelete(nullptr);
    return;
  }
  std::unique_ptr<WiFiClientSecure> client;
  HTTPClient http;
  const int status = openGet(imageUrl, http, client);
  const int length = http.getSize();
  if (status != 200 || length <= 0 || (imageSize && (size_t)length != imageSize)) {
    http.end();
    fail(String("Download failed (") + status + ").");
    vTaskDelete(nullptr);
    return;
  }
  if (!Update.begin(length)) {
    http.end();
    fail("Not enough room for the update.");
    vTaskDelete(nullptr);
    return;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[4096];
  int written = 0;
  unsigned long lastData = millis();
  while (written < length && millis() - lastData < 20000) {
    const int n = stream->readBytes(buf, min((int)sizeof buf, length - written));
    if (n <= 0) {
      delay(10);
      continue;
    }
    lastData = millis();
    mbedtls_sha256_update_ret(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) break;
    written += n;
    progress = written * 100 / length;
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (written != length) {
    Update.abort();
    fail("The download stopped. Nothing changed.");
    vTaskDelete(nullptr);
    return;
  }
  if (imageSha != hex) {
    Update.abort();
    fail("The download did not match its checksum. Nothing changed.");
    vTaskDelete(nullptr);
    return;
  }
  if (!Update.end(true)) {
    fail(String("Could not finish the update: ") + Update.errorString());
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("UPDATE ready: %s (%d bytes, sha256 ok)\n", available.c_str(), written);
  state = UpdateState::Ready;  // the UI restarts the device when the user agrees
  vTaskDelete(nullptr);
}

bool launch(UpdateState next, TaskFunction_t fn) {
  if (state == UpdateState::Checking || state == UpdateState::Installing) return false;
  if (updaterUrl().isEmpty()) {
    error = "Updates are not set up.";
    state = UpdateState::Failed;
    return false;
  }
  error = "";
  progress = 0;
  state = next;
  if (xTaskCreatePinnedToCore(fn, "updater", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    fail("Not enough memory.");
    return false;
  }
  return true;
}
}  // namespace

void updaterBegin() { store.begin("perch-update", false); }
String updaterUrl() { return store.getString("url", ""); }
void updaterSetUrl(const String &url) { store.putString("url", url); }
bool updaterCheck() { return launch(UpdateState::Checking, checkTask); }
bool updaterInstall() {
  if (state != UpdateState::Available) return false;
  return launch(UpdateState::Installing, installTask);
}
UpdateState updaterState() { return state; }
String updaterAvailableVersion() { return available; }
int updaterProgress() { return progress; }
String updaterError() { return error; }

bool versionNewer(const String &a, const String &b) {
  int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
  sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (int i = 0; i < 3; ++i)
    if (pa[i] != pb[i]) return pa[i] > pb[i];
  return false;
}
