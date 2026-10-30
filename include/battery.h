#pragma once

// Battery level from a 1:2 resistor divider on PIN_BATTERY (see board_pins.h).
// The XIAO ESP32-S3 charges a 1-cell LiPo by itself but has no pin that reports its voltage.
// Until the divider is wired (PIN_BATTERY < 0) everything here reports "unknown".
void batteryBegin();
// 0..100, or -1 when there is no battery sense (or no battery).
int batteryPercent();
// Cell voltage in volts, 0 when unknown.
float batteryVolts();

enum class PowerSource { Unknown, Usb, Battery };
enum class BatteryCharge { Unknown, NotCharging, Charging, Full };
struct BatteryStatus {
  int percent;              // -1 = unavailable; voltage-derived percentages are estimates
  float volts;             // 0 = unavailable
  bool usbHostConnected;   // USB data host detected; NOT proof of charging or battery presence
  PowerSource source;
  BatteryCharge charge;
};
BatteryStatus batteryStatus();
