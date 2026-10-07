#pragma once
#include <stdint.h>

// Bluetooth LE keyboard + media keys. Any computer or phone pairs with "Perch" from its
// own Bluetooth settings; no app is needed. Started on first use (it costs ~40 KB RAM).
bool remoteBegin();
// Stop Bluetooth and free its memory (needed before HTTPS requests).
void remoteEnd();
bool remoteStarted();
bool remoteConnected();
// Press and release one key (USB HID usage id), e.g. 0x4F right arrow.
void remoteKey(uint8_t usage);
// Press and release one consumer-control key, e.g. 0xCD play/pause.
void remoteMedia(uint16_t usage);

namespace RemoteKey {
constexpr uint8_t Right = 0x4F, Left = 0x50, Space = 0x2C;
}
namespace RemoteMedia {
constexpr uint16_t PlayPause = 0xCD, Next = 0xB5, Previous = 0xB6, VolumeUp = 0xE9, VolumeDown = 0xEA;
}
