#include "battery.h"
#include <Arduino.h>
#include "board_pins.h"
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
#include <HWCDC.h>
#endif

namespace {
float smoothed = 0;
unsigned long lastRead = 0;

// Resting voltage of a 1-cell LiPo against charge left (typical curve, under light load).
int percentFor(float v) {
  static const float volts[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.75f, 3.80f, 3.87f, 3.95f, 4.05f, 4.15f};
  static const int pct[] = {0, 5, 10, 20, 30, 40, 55, 70, 85, 100};
  if (v <= volts[0]) return 0;
  for (int i = 1; i < 10; ++i)
    if (v < volts[i]) return pct[i - 1] + (int)((v - volts[i - 1]) / (volts[i] - volts[i - 1]) * (pct[i] - pct[i - 1]));
  return 100;
}

float readVolts() {
  if (PIN_BATTERY < 0) return 0;
  uint32_t mv = 0;
  for (int i = 0; i < 8; ++i) mv += analogReadMilliVolts(PIN_BATTERY);
  return mv / 8.0f * BATTERY_DIVIDER / 1000.0f;
}
}  // namespace

void batteryBegin() {
  if (PIN_USB_PRESENT >= 0) pinMode(PIN_USB_PRESENT, INPUT);
  if (PIN_CHARGING >= 0) pinMode(PIN_CHARGING, INPUT_PULLUP);
  if (PIN_CHARGE_DONE >= 0) pinMode(PIN_CHARGE_DONE, INPUT_PULLUP);
  if (PIN_BATTERY < 0) return;
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);  // full 0-3.1 V range
  smoothed = readVolts();
}

float batteryVolts() {
  if (PIN_BATTERY < 0) return 0;
  // Read at most every 5 s and smooth: the voltage sags with Wi-Fi and screen load.
  if (millis() - lastRead > 5000 || lastRead == 0) {
    lastRead = millis();
    const float v = readVolts();
    smoothed = smoothed <= 0.1f ? v : smoothed * 0.8f + v * 0.2f;
  }
  return smoothed;
}

int batteryPercent() {
  const float v = batteryVolts();
  // Below 2.5 V there is no cell connected (the pin just floats near ground).
  if (v < 2.5f || v > 4.35f) return -1;
  return percentFor(v);
}

BatteryStatus batteryStatus() {
  BatteryStatus state = {batteryPercent(), batteryVolts(), false, PowerSource::Unknown, BatteryCharge::Unknown};
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  state.usbHostConnected = HWCDC::isPlugged();
#endif
  // No USB host does not imply battery power: wall chargers have no USB data host.
  if (PIN_USB_PRESENT >= 0) {
    const bool usbPower = digitalRead(PIN_USB_PRESENT) == HIGH;
    if (usbPower) state.source = PowerSource::Usb;
    else if (state.percent >= 0) state.source = PowerSource::Battery;
    if (!usbPower) state.charge = BatteryCharge::NotCharging;
    else if (PIN_CHARGING >= 0 && digitalRead(PIN_CHARGING) == LOW)
      state.charge = BatteryCharge::Charging;
    else if (PIN_CHARGE_DONE >= 0 && digitalRead(PIN_CHARGE_DONE) == LOW)
      state.charge = BatteryCharge::Full;
    else if (PIN_CHARGING >= 0 && PIN_CHARGE_DONE >= 0)
      state.charge = BatteryCharge::NotCharging;
  }
  // Never infer charge completion from USB presence, voltage, or elapsed time.
  return state;
}
