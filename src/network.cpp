#include "network.h"
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <vector>
#include "wifi_secrets.h"

namespace {
const char *GUEST_SSID = "guest";
constexpr unsigned long RETRY_MS = 20000;
Preferences store;
bool radioOn = true;
bool paused = false;
unsigned long lastAttempt = 0;
int candidate = 0;

struct Candidate {
  String ssid, password;
};
std::vector<Candidate> candidates() {
  std::vector<Candidate> list;
  for (int i = 0; i < SAVED_NETWORKS; ++i) {
    const String ssid = store.getString(("s" + String(i)).c_str(), "");
    if (ssid.length()) list.push_back({ssid, store.getString(("p" + String(i)).c_str(), "")});
  }
  if (strcmp(WIFI_TEST_SSID, "your-network") != 0) list.push_back({WIFI_TEST_SSID, WIFI_TEST_PASSWORD});
  list.push_back({GUEST_SSID, ""});
  return list;
}
void joinNext() {
  const std::vector<Candidate> list = candidates();
  if (list.empty()) return;
  const Candidate &c = list[candidate % list.size()];
  ++candidate;
  if (c.password.length()) WiFi.begin(c.ssid.c_str(), c.password.c_str());
  else WiFi.begin(c.ssid.c_str());
  lastAttempt = millis();
}
}  // namespace

void networkBegin() {
  store.begin("perch-wifi", false);
  // Wi-Fi time replaces any build or USB time once it arrives. SNTP retries until online.
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "time.google.com", "pool.ntp.org");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  joinNext();
}

void networkTick() {
  if (radioOn && !paused && WiFi.status() != WL_CONNECTED && millis() - lastAttempt > RETRY_MS) {
    WiFi.disconnect();
    joinNext();
  }
}

void networkSetEnabled(bool on) {
  // Control Center Wi-Fi toggle: off turns the radio off entirely (saves power).
  radioOn = on;
  if (on) {
    WiFi.mode(WIFI_STA);
    candidate = 0;
    joinNext();
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

bool networkEnabled() { return radioOn; }
bool networkConnected() { return WiFi.status() == WL_CONNECTED; }
String networkName() { return networkConnected() ? WiFi.SSID() : String(); }

void networkSave(const String &ssid, const String &password) {
  // Most recent first: shift the others down, dropping the oldest and any old copy.
  std::vector<Candidate> saved;
  saved.push_back({ssid, password});
  for (int i = 0; i < SAVED_NETWORKS; ++i) {
    const String s = store.getString(("s" + String(i)).c_str(), "");
    if (s.length() && s != ssid) saved.push_back({s, store.getString(("p" + String(i)).c_str(), "")});
  }
  for (int i = 0; i < SAVED_NETWORKS; ++i) {
    const bool used = i < (int)saved.size();
    store.putString(("s" + String(i)).c_str(), used ? saved[i].ssid : "");
    store.putString(("p" + String(i)).c_str(), used ? saved[i].password : "");
  }
  radioOn = true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  candidate = 0;
  joinNext();
}

void networkForget(const String &ssid) {
  std::vector<Candidate> kept;
  for (int i = 0; i < SAVED_NETWORKS; ++i) {
    const String s = store.getString(("s" + String(i)).c_str(), "");
    if (s.length() && s != ssid) kept.push_back({s, store.getString(("p" + String(i)).c_str(), "")});
  }
  for (int i = 0; i < SAVED_NETWORKS; ++i) {
    const bool used = i < (int)kept.size();
    store.putString(("s" + String(i)).c_str(), used ? kept[i].ssid : "");
    store.putString(("p" + String(i)).c_str(), used ? kept[i].password : "");
  }
  if (networkName() == ssid) WiFi.disconnect();
}

bool networkIsSaved(const String &ssid) {
  for (int i = 0; i < SAVED_NETWORKS; ++i)
    if (store.getString(("s" + String(i)).c_str(), "") == ssid) return true;
  return false;
}

void networkPauseRetries(bool p) { paused = p; }

void networkScanStart() {
  if (!radioOn) {
    radioOn = true;
    WiFi.mode(WIFI_STA);
  }
  // A retry in progress would cancel the scan: stop trying while the list is shown.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false, false, 300);
}

int networkScanResults(ScanResult *out, int max) {
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n < 0) return 0;
  std::vector<ScanResult> all;
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    auto same = std::find_if(all.begin(), all.end(), [&](const ScanResult &r) { return r.ssid == ssid; });
    if (same != all.end()) {
      same->rssi = std::max(same->rssi, (int)WiFi.RSSI(i));
      continue;
    }
    all.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  std::sort(all.begin(), all.end(), [](const ScanResult &a, const ScanResult &b) { return a.rssi > b.rssi; });
  const int count = std::min<int>(max, all.size());
  for (int i = 0; i < count; ++i) out[i] = all[i];
  return count;
}
