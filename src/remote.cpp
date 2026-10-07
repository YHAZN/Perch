#include "remote.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

namespace {
// Report 1: boot-style keyboard (modifiers, reserved, 6 keys). Report 2: one consumer usage.
const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,  // Usage Page (Desktop), Usage (Keyboard), Collection (App)
    0x85, 0x01,                          // Report ID 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,  // Usage Page (Keys), modifiers E0-E7
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  // 8 bits, Input (Data, Var, Abs)
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                          // reserved byte, Input (Const)
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,  // 6 key codes, Input (Data, Array)
    0xC0,                                // End Collection
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,  // Usage Page (Consumer), Usage (Control), Collection (App)
    0x85, 0x02,                          // Report ID 2
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,  // one 16-bit usage, Input (Data, Array)
    0xC0,                                // End Collection
};

bool started = false;
volatile bool connected = false;
NimBLEHIDDevice *hid = nullptr;
NimBLECharacteristic *keyboard = nullptr, *consumer = nullptr;

class Callbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override { connected = true; }
  void onDisconnect(NimBLEServer *) override {
    connected = false;
    NimBLEDevice::startAdvertising();  // stay pairable for the next computer
  }
};
}  // namespace

bool remoteBegin() {
  if (started) return true;
  NimBLEDevice::init("Perch");
  // "Just works" pairing with bonding: the computer remembers the device.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new Callbacks());
  hid = new NimBLEHIDDevice(server);
  keyboard = hid->inputReport(1);
  consumer = hid->inputReport(2);
  hid->manufacturer()->setValue("Perch");
  hid->pnp(0x02, 0x303A, 0x0001, 0x0100);
  hid->hidInfo(0x00, 0x02);
  hid->reportMap((uint8_t *)REPORT_MAP, sizeof(REPORT_MAP));
  hid->startServices();
  hid->setBatteryLevel(100);
  NimBLEAdvertising *advertising = server->getAdvertising();
  advertising->setAppearance(HID_KEYBOARD);
  advertising->addServiceUUID(hid->hidService()->getUUID());
  advertising->start();
  started = true;
  return true;
}

bool remoteStarted() { return started; }
bool remoteConnected() { return connected; }

void remoteKey(uint8_t usage) {
  if (!connected || !keyboard) return;
  uint8_t report[8] = {0, 0, usage, 0, 0, 0, 0, 0};
  keyboard->setValue(report, sizeof(report));
  keyboard->notify();
  delay(8);
  memset(report, 0, sizeof(report));
  keyboard->setValue(report, sizeof(report));
  keyboard->notify();
}

void remoteMedia(uint16_t usage) {
  if (!connected || !consumer) return;
  uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  consumer->setValue(report, sizeof(report));
  consumer->notify();
  delay(8);
  report[0] = report[1] = 0;
  consumer->setValue(report, sizeof(report));
  consumer->notify();
}

// Bluetooth holds ~100 KB of internal RAM; HTTPS needs it back. Reopening Remote restarts
// advertising and a bonded computer reconnects by itself.
void remoteEnd() {
  if (!started) return;
  delete hid;
  hid = nullptr;
  keyboard = consumer = nullptr;
  NimBLEDevice::deinit(true);
  started = false;
  connected = false;
}
