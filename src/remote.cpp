#include "remote.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

namespace {
// Reports: 1 = keyboard (8 bytes), 2 = consumer control (2 bytes), 3 = relative mouse (4 bytes).
// clang-format off
const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0, 0x05,
    0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x01, 0xA1, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75,
    0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09,
    0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95,
    0x03, 0x81, 0x06, 0xC0, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85,
    0x02, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75,
    0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};
// clang-format on

bool started = false;
volatile bool connected = false;
volatile uint32_t disconnects = 0;
NimBLEHIDDevice *hid = nullptr;
NimBLECharacteristic *keyboard = nullptr, *consumer = nullptr, *mouse = nullptr;
uint8_t mouseButtons = 0;

class Callbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override { connected = true; }
  void onDisconnect(NimBLEServer *) override {
    connected = false;
    mouseButtons = 0;
    ++disconnects;
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
  mouse = hid->inputReport(3);
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
uint32_t remoteDisconnectCount() { return disconnects; }

void remoteKey(uint8_t usage, uint8_t modifiers) {
  if (!connected || !keyboard) return;
  uint8_t report[8] = {modifiers, 0, usage, 0, 0, 0, 0, 0};
  keyboard->setValue(report, sizeof(report));
  keyboard->notify();
  delay(8);
  memset(report, 0, sizeof(report));
  keyboard->setValue(report, sizeof(report));
  keyboard->notify();
}

void remoteMouseMove(int16_t x, int16_t y, int16_t wheel) {
  if (!connected || !mouse) return;
  // Split long deltas rather than wrapping an 8-bit relative HID report.
  do {
    const int8_t dx = constrain(x, -127, 127), dy = constrain(y, -127, 127);
    const int8_t dz = constrain(wheel, -127, 127);
    const uint8_t report[4] = {mouseButtons, uint8_t(dx), uint8_t(dy), uint8_t(dz)};
    mouse->setValue(report, sizeof(report));
    mouse->notify();
    x -= dx;
    y -= dy;
    wheel -= dz;
  } while (connected && (x || y || wheel));
}

void remoteMouseButtons(uint8_t buttons) {
  mouseButtons = connected ? buttons & 0x07 : 0;
  remoteMouseMove(0, 0);
}

void remoteMouseClick(uint8_t button) {
  if (!connected) return;
  const uint8_t held = mouseButtons;
  remoteMouseButtons(held | button);
  delay(8);
  remoteMouseButtons(held);
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

// Disabling Bluetooth releases its internal allocations. Reopening Remote restarts
// advertising and a bonded computer can reconnect without pairing again.
void remoteEnd() {
  if (!started) return;
  remoteMouseButtons(0);
  delete hid;
  hid = nullptr;
  keyboard = consumer = mouse = nullptr;
  NimBLEDevice::deinit(true);
  started = false;
  connected = false;
}
