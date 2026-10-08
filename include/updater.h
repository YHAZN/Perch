#pragma once
#include <Arduino.h>

// Software updates over Wi-Fi. A published manifest (small JSON) names the newest version:
//   {"version":"0.5.1","url":"https://.../firmware.bin","size":1655008,"sha256":"<64 hex>",
//    "signature":"<base64 DER ECDSA P-256 over the image>"}
// The device downloads the image into the other app slot while it keeps running, checks
// size, SHA-256 and the release signature, and only then marks it to boot; any failure leaves
// the current firmware.
// Nothing happens until a manifest address is saved (Settings shows "Not set up").
enum class UpdateState { Idle, Checking, UpToDate, Available, Installing, Ready, Failed };

void updaterBegin();
// The manifest address (HTTPS). Empty = updates not set up.
String updaterUrl();
void updaterSetUrl(const String &url);
// Background check / install. Both return false if one is already running.
bool updaterCheck();
bool updaterInstall();
UpdateState updaterState();
String updaterAvailableVersion();
int updaterProgress();  // 0..100 while installing
String updaterError();
// True if `a` is newer than `b` ("0.5.10" > "0.5.9").
bool versionNewer(const String &a, const String &b);
// Developer: does `signature` (base64) sign an image with this SHA-256 (hex)?
bool updaterVerify(const String &shaHex, const String &signature);
