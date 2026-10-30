#pragma once
#include <stdint.h>

// Bluetooth LE keyboard, mouse and media keys. Any computer or phone pairs with "Perch" from its
// own Bluetooth settings; no app is needed. Started on first use (it costs ~40 KB RAM).
bool remoteBegin();
// Stop Bluetooth and free its memory when disabled or needed by the low-memory fallback.
void remoteEnd();
bool remoteStarted();
bool remoteConnected();
uint32_t remoteDisconnectCount();
// Press and release one key (USB HID usage id), e.g. 0x4F right arrow.
void remoteKey(uint8_t usage, uint8_t modifiers = 0);
// Press and release one consumer-control key, e.g. 0xCD play/pause.
void remoteMedia(uint16_t usage);
void remoteMouseMove(int16_t x, int16_t y, int16_t wheel = 0);
void remoteMouseClick(uint8_t button);
void remoteMouseButtons(uint8_t buttons);

namespace RemoteKey {
constexpr uint8_t Right = 0x4F, Left = 0x50, Space = 0x2C;
constexpr uint8_t Tab = 0x2B, Escape = 0x29, F5 = 0x3E, C = 0x06, D = 0x07, S = 0x16, V = 0x19;
}  // namespace RemoteKey
namespace RemoteModifier {
constexpr uint8_t Control = 0x01, Alt = 0x04, Windows = 0x08;
}
namespace RemoteMouse {
constexpr uint8_t Left = 0x01, Right = 0x02;
}
namespace RemoteMedia {
constexpr uint16_t PlayPause = 0xCD, Next = 0xB5, Previous = 0xB6, Mute = 0xE2, VolumeUp = 0xE9, VolumeDown = 0xEA;
}
