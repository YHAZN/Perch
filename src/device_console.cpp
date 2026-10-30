#include "device_console.h"
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_https_server.h>
#include <freertos/semphr.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <atomic>
#include <algorithm>

namespace {
constexpr size_t OUTPUT_LIMIT = 2 * 1024 * 1024 + 4096;
constexpr size_t LOG_SIZE = 16384;
constexpr uint16_t PORT = 8443;
Preferences prefs;
String certificate, privateKey, token;
httpd_handle_t server = nullptr;
std::atomic_bool serverRunning{false};
bool enabled = false;
std::atomic_bool generatingIdentity{false};
std::atomic_bool identityReady{false};
uint32_t retryAt = 0;
SemaphoreHandle_t logMutex = nullptr;
char *logs = nullptr;
size_t logStart = 0, logLength = 0;
struct Request {
  String input;
  size_t cursor = 0;
  uint8_t *output = nullptr;
  size_t length = 0, capacity = 0;
  bool overflow = false;
  SemaphoreHandle_t done = nullptr;
};
std::atomic<Request *> pending{nullptr};
Request *active = nullptr;
std::atomic<TaskHandle_t> owner{nullptr};
DeviceConsole console;

void appendLog(const uint8_t *data, size_t length) {
  if (!logs || !logMutex || length > 1024) return;
  for (size_t i = 0; i < length; ++i)
    if ((data[i] < 32 && data[i] != '\n' && data[i] != '\r' && data[i] != '\t') || data[i] > 126) return;
  xSemaphoreTake(logMutex, portMAX_DELAY);
  for (size_t i = 0; i < length; ++i) {
    if (logLength < LOG_SIZE) logs[(logStart + logLength++) % LOG_SIZE] = data[i];
    else { logs[logStart] = data[i]; logStart = (logStart + 1) % LOG_SIZE; }
  }
  xSemaphoreGive(logMutex);
}

bool generateIdentity() {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context rng;
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_mpi serial;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&rng);
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  mbedtls_mpi_init(&serial);
  const unsigned char seed[] = "perch-local-link";
  bool ok = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy, seed, sizeof(seed)) == 0 &&
            mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &rng) == 0;
  uint8_t serialBytes[8];
  esp_fill_random(serialBytes, sizeof(serialBytes));
  serialBytes[0] &= 0x7f;
  ok = ok && mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes)) == 0;
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  ok = ok && mbedtls_x509write_crt_set_serial(&crt, &serial) == 0 &&
       mbedtls_x509write_crt_set_subject_name(&crt, "CN=Perch local development") == 0 &&
       mbedtls_x509write_crt_set_issuer_name(&crt, "CN=Perch local development") == 0 &&
       mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20450101000000") == 0 &&
       mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0 &&
       mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0;
  uint8_t certPem[1600] = {}, keyPem[500] = {};
  ok = ok && mbedtls_x509write_crt_pem(&crt, certPem, sizeof(certPem), mbedtls_ctr_drbg_random, &rng) == 0 &&
       mbedtls_pk_write_key_pem(&key, keyPem, sizeof(keyPem)) == 0;
  if (ok) {
    certificate = reinterpret_cast<char *>(certPem);
    privateKey = reinterpret_cast<char *>(keyPem);
    uint8_t secret[32];
    esp_fill_random(secret, sizeof(secret));
    token = "";
    char pair[3];
    for (uint8_t byte : secret) { snprintf(pair, sizeof(pair), "%02x", byte); token += pair; }
    prefs.putBool("identity-v1", false);
    ok = prefs.putString("cert", certificate) && prefs.putString("key", privateKey) && prefs.putString("token", token);
    if (ok) ok = prefs.putBool("identity-v1", true);
    if (!ok) { certificate = ""; privateKey = ""; token = ""; }
  }
  memset(keyPem, 0, sizeof(keyPem));
  mbedtls_mpi_free(&serial);
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&rng);
  mbedtls_entropy_free(&entropy);
  return ok;
}

void identityTask(void *) {
  identityReady = generateIdentity();
  generatingIdentity = false;
  vTaskDelete(nullptr);
}

bool identity() {
  if (identityReady) return true;
  if (generatingIdentity.exchange(true)) return false;
  if (xTaskCreatePinnedToCore(identityTask, "link-identity", 24576, nullptr, 1, nullptr, 0) != pdPASS)
    generatingIdentity = false;
  return false;
}

bool authorized(httpd_req_t *req) {
  char header[80] = {};
  if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return false;
  const String expected = "Bearer " + token;
  if (strlen(header) != expected.length()) return false;
  unsigned difference = 0;
  for (size_t i = 0; i < expected.length(); ++i) difference |= header[i] ^ expected[i];
  return difference == 0;
}

esp_err_t deny(httpd_req_t *req) { return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Pair this device first"); }

esp_err_t commandHandler(httpd_req_t *req) {
  if (!authorized(req)) return deny(req);
  if (req->content_len < 1 || req->content_len > 1024)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command length");
  char input[1025] = {};
  size_t received = 0;
  while (received < req->content_len) {
    const int n = httpd_req_recv(req, input + received, req->content_len - received);
    if (n <= 0) return ESP_FAIL;
    received += n;
  }
  // Pairing credentials are never exported over the network, even to an authenticated peer.
  if (input[0] == '_' || memchr(input, 0, received) ||
      (memchr(input, '\n', received) && input[received - 1] != '\n'))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command");
  for (size_t i = 0; i + 1 < received; ++i)
    if (input[i] == '\n' || input[i] == '\r')
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "One command per request");
  Request request;
  request.input = input;
  request.done = xSemaphoreCreateBinary();
  if (!request.done) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
  Request *empty = nullptr;
  if (!pending.compare_exchange_strong(empty, &request)) {
    vSemaphoreDelete(request.done);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Device busy", HTTPD_RESP_USE_STRLEN);
  }
  // The UI owns the request until completion; never free a buffer it may still be writing.
  xSemaphoreTake(request.done, portMAX_DELAY);
  vSemaphoreDelete(request.done);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t result = request.overflow
      ? httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Response exceeded memory limit")
      : httpd_resp_send(req, reinterpret_cast<char *>(request.output), request.length);
  free(request.output);
  return result;
}

esp_err_t logsHandler(httpd_req_t *req) {
  if (!authorized(req)) return deny(req);
  char *copy = static_cast<char *>(ps_malloc(LOG_SIZE + 1));
  if (!copy) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough memory");
  xSemaphoreTake(logMutex, portMAX_DELAY);
  const size_t length = logLength;
  for (size_t i = 0; i < length; ++i) copy[i] = logs[(logStart + i) % LOG_SIZE];
  xSemaphoreGive(logMutex);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t result = httpd_resp_send(req, copy, length);
  free(copy);
  return result;
}
}

DeviceConsole &deviceConsole() { return console; }
bool DeviceConsole::wirelessRequest() const { return xTaskGetCurrentTaskHandle() == owner && active; }
int DeviceConsole::available() { return xTaskGetCurrentTaskHandle() == owner && active ? active->input.length() - active->cursor : Serial.available(); }
int DeviceConsole::peek() { return xTaskGetCurrentTaskHandle() == owner && active ? (active->cursor < active->input.length() ? int(static_cast<uint8_t>(active->input[active->cursor])) : -1) : Serial.peek(); }
int DeviceConsole::read() { int value = peek(); if (xTaskGetCurrentTaskHandle() == owner && active) { if (value >= 0) ++active->cursor; return value; } return Serial.read(); }
void DeviceConsole::flush() { if (xTaskGetCurrentTaskHandle() != owner || !active) Serial.flush(); }
size_t DeviceConsole::write(uint8_t value) { return write(&value, 1); }
size_t DeviceConsole::write(const uint8_t *data, size_t length) {
  if (xTaskGetCurrentTaskHandle() == owner && active) {
    if (active->overflow) return length;
    const size_t required = active->length + length;
    if (required > OUTPUT_LIMIT) { active->overflow = true; return length; }
    if (required > active->capacity) {
      const size_t capacity = std::min(OUTPUT_LIMIT, std::max(required, std::max<size_t>(32768, active->capacity * 2)));
      uint8_t *buffer = static_cast<uint8_t *>(ps_realloc(active->output, capacity));
      if (!buffer) { active->overflow = true; return length; }
      active->output = buffer;
      active->capacity = capacity;
    }
    memcpy(active->output + active->length, data, length);
    active->length += length;
    return length;
  }
  appendLog(data, length);
  return Serial.write(data, length);
}

void wirelessLinkBegin() {
  prefs.begin("perch-link", false);
  enabled = prefs.getBool("enabled", false);
  certificate = prefs.getString("cert");
  privateKey = prefs.getString("key");
  token = prefs.getString("token");
  const bool complete = certificate.length() && privateKey.length() && token.length() == 64;
  // Accept identities provisioned before the transaction marker was added.
  identityReady = complete && prefs.getBool("identity-v1", !prefs.isKey("identity-v1"));
  if (identityReady && !prefs.isKey("identity-v1")) prefs.putBool("identity-v1", true);
  logMutex = xSemaphoreCreateMutex();
  logs = static_cast<char *>(ps_malloc(LOG_SIZE));
}
bool wirelessLinkRunning() { return serverRunning.load(); }
bool wirelessLinkEnabled() { return enabled; }
void wirelessLinkSetEnabled(bool value) { enabled = value; prefs.putBool("enabled", value); retryAt = 0; }
String wirelessLinkAddress() { return "https://" + WiFi.localIP().toString() + ":" + PORT; }
void wirelessLinkTick(bool suspended) {
  if (server && (!enabled || suspended || WiFi.status() != WL_CONNECTED)) {
    httpd_ssl_stop(server);
    server = nullptr;
    serverRunning = false;
  }
  if (server || !enabled || suspended || WiFi.status() != WL_CONNECTED || millis() < retryAt) return;
  if (!logMutex || !logs) return;
  if (!identity()) { retryAt = millis() + 500; return; }
  retryAt = millis() + 30000;
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.port_secure = PORT;
  config.httpd.max_open_sockets = 2;
  config.httpd.max_uri_handlers = 2;
  config.httpd.stack_size = 10240;
  config.httpd.recv_wait_timeout = 5;
  config.httpd.send_wait_timeout = 10;
  config.cacert_pem = reinterpret_cast<const uint8_t *>(certificate.c_str());
  config.cacert_len = certificate.length() + 1;
  config.prvtkey_pem = reinterpret_cast<const uint8_t *>(privateKey.c_str());
  config.prvtkey_len = privateKey.length() + 1;
  if (httpd_ssl_start(&server, &config) != ESP_OK) { server = nullptr; return; }
  httpd_uri_t command = {};
  command.uri = "/command"; command.method = HTTP_POST; command.handler = commandHandler;
  httpd_register_uri_handler(server, &command);
  httpd_uri_t logRoute = {};
  logRoute.uri = "/logs"; logRoute.method = HTTP_GET; logRoute.handler = logsHandler;
  httpd_register_uri_handler(server, &logRoute);
  serverRunning = true;
  console.printf("WIRELESS_LINK ready %s\n", wirelessLinkAddress().c_str());
}
void wirelessLinkPoll() {
  if (active) return;
  active = pending.exchange(nullptr);
  if (active) owner = xTaskGetCurrentTaskHandle();
}
void wirelessLinkFinishCommand() {
  if (!active) return;
  Request *finished = active;
  active = nullptr;
  owner = nullptr;
  xSemaphoreGive(finished->done);
}
void wirelessLinkProvision() {
  if (active) { console.println("WIRELESS_LINK pairing requires USB"); return; }
  if (WiFi.status() != WL_CONNECTED) { console.println("WIRELESS_LINK connect Wi-Fi before pairing"); return; }
  if (!identity()) {
    const uint32_t started = millis();
    while (generatingIdentity && millis() - started < 15000) delay(10);
    if (!identityReady) { console.println("WIRELESS_LINK identity generation failed"); return; }
  }
  wirelessLinkSetEnabled(true);
  JsonDocument doc;
  doc["url"] = wirelessLinkAddress();
  doc["certificate"] = certificate;
  doc["token"] = token;
  // Deliberately bypass the console/log ring: the identity goes to physical USB only.
  Serial.print("LINK_IDENTITY "); serializeJson(doc, Serial); Serial.println();
}
