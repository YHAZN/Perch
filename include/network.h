#pragma once
#include <Arduino.h>

// Wi-Fi runs in the background from boot and reconnects by itself. Networks are tried in
// order: ones saved on the device (most recent first), the build-time test network, then
// the open "guest" network.
void networkBegin();
void networkTick();
bool networkConnected();
void networkSetEnabled(bool on);
bool networkEnabled();
String networkName();  // SSID when connected, else ""

// Saved networks (NVS). Saving moves a network to the front and joins it now.
constexpr int SAVED_NETWORKS = 5;
void networkSave(const String &ssid, const String &password);
void networkForget(const String &ssid);
bool networkIsSaved(const String &ssid);

// Scanning without blocking the UI. While the Wi-Fi screen is open, automatic retries pause
// so they do not cancel the scan.
struct ScanResult {
  String ssid;
  int rssi;
  bool secured;
};
void networkScanStart();
// -1 while scanning, else the number of results (strongest first, duplicates removed).
int networkScanResults(ScanResult *out, int max);
void networkPauseRetries(bool paused);
