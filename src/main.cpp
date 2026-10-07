#include <Arduino.h>

#include <esp_system.h>
#include <esp_camera.h>
#include <WiFi.h>
#include "wifi_secrets.h"
#include "device_ui.h"
#include "ai_client.h"
#include "clock.h"
#include "camera.h"

void testWifi(const char *ssid = WIFI_TEST_SSID, const char *password = WIFI_TEST_PASSWORD) {
  WiFi.mode(WIFI_STA);
  Serial.println("\n=== Wi-Fi test: scanning ===");
  const int count = WiFi.scanNetworks(false, true, false, 600);
  Serial.printf("Scan found %d networks\n", count);
  bool targetFound = false;
  for (int i = 0; i < count; ++i) {
    Serial.printf("SSID: %s | RSSI: %d dBm | channel: %d | %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
    if (WiFi.SSID(i) == ssid) targetFound = true;
  }
  WiFi.scanDelete();
  if (!targetFound) {
    Serial.println("Cannot join: configured test network not visible on this board.");
    return;
  }
  Serial.printf("Connecting to %s...\n", ssid);
  WiFi.begin(ssid, password);
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi connection timed out. Status: %d\n", WiFi.status());
    return;
  }
  Serial.printf("Wi-Fi CONNECTED | IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  IPAddress address;
  const int resolved = WiFi.hostByName("example.com", address);
  Serial.printf("DNS example.com: %s | %s\n", resolved ? "PASS" : "FAIL", address.toString().c_str());
  Serial.println("Wi-Fi test complete. Internet HTTPS and captive portal checks remain.");
}

void capture(bool sendImage = false) {
  // Diagnostic capture in still mode (the UI may have the camera off or in preview).
  if (!cameraSetMode(CameraMode::Still)) {
    Serial.println("Camera unavailable; check initialization report.");
    return;
  }
  // Discard the queued frame so the command captures the current scene; focus like a photo.
  camera_fb_t *queued = esp_camera_fb_get();
  if (queued) esp_camera_fb_return(queued);
  if (cameraFocus(2000)) {
    queued = esp_camera_fb_get();
    if (queued) esp_camera_fb_return(queued);
  }
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("CAPTURE FAILED: no frame returned.");
    return;
  }
  const bool jpeg = frame->format == PIXFORMAT_JPEG && frame->len >= 4 && frame->buf[0] == 0xff &&
                    frame->buf[1] == 0xd8 && frame->buf[frame->len - 2] == 0xff && frame->buf[frame->len - 1] == 0xd9;
  if (sendImage && jpeg) {
    Serial.printf("JPEG_BEGIN %u\n", static_cast<unsigned>(frame->len));
    Serial.write(frame->buf, frame->len);
    Serial.println("\nJPEG_END");
  } else {
    Serial.printf("Capture: %ux%u | %u bytes | JPEG markers: %s\n", static_cast<unsigned>(frame->width),
                  static_cast<unsigned>(frame->height), static_cast<unsigned>(frame->len), jpeg ? "PASS" : "FAIL");
  }
  cameraReport();
  esp_camera_fb_return(frame);
  cameraSetMode(CameraMode::Off);  // do not leave the sensor streaming (heat)
}

void printStatus() {
  Serial.println("\n=== Tiny AI: USB diagnostic ===");
  Serial.printf("Chip: %s, revision %d, %d cores\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
  Serial.printf("Flash: %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM: %s, total %u bytes, free %u bytes\n", psramFound() ? "FOUND" : "NOT FOUND", ESP.getPsramSize(),
                ESP.getFreePsram());
  Serial.printf("Reset reason: %d\n", static_cast<int>(esp_reset_reason()));
  Serial.println("Type s to print this report again.");
  Serial.printf("Wi-Fi status: %d | IP: %s\n", WiFi.status(), WiFi.localIP().toString().c_str());
  Serial.println("Type w to scan and test configured Wi-Fi.");
  Serial.println("Type g to scan and test campus guest Wi-Fi.");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  // The USB-serial driver treats a 100 ms pause in host reads as "unplugged" and then
  // silently drops bytes while reporting success, truncating mirror frames. Allow 1 s.
  Serial.setTxTimeoutMs(1000);
  const unsigned long start = millis();
  // Brief wait so early lines reach a connected PC; standalone boots must not stall here.
  while (!Serial && millis() - start < 800) { delay(10); }
  aiBegin();  // before any HTTPS (SNTP is plain UDP)
  clockBegin();
  printStatus();
  initDeviceUi();
  networkBegin();
}

void loop() {
  while (Serial.available()) {
    deviceNoteSerial();
    const char command = Serial.read();
    if (command == 's') printStatus();
    if (command == 'c') capture();
    if (command == 'j') capture(true);
    if (command == 'w') testWifi();
    if (command == 'g') testWifi("guest", "");
    handleDeviceButton(command);
  }
  deviceTick();
  networkTick();
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 2000) {
    lastHeartbeat = millis();
    Serial.printf("Alive: %lu seconds | free heap: %u bytes\n", millis() / 1000, ESP.getFreeHeap());
  }
  delay(1);
}
