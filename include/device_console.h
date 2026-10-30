#pragma once
#include <Arduino.h>

// USB and authenticated wireless requests share one command parser on the UI task.
// Wireless output is isolated from background logs and never written from an ISR.
class DeviceConsole : public Stream {
public:
  using Print::write;
  bool wirelessRequest() const;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *data, size_t length) override;
};
DeviceConsole &deviceConsole();
void wirelessLinkBegin();
void wirelessLinkTick(bool suspended);
void wirelessLinkPoll();
void wirelessLinkFinishCommand();
bool wirelessLinkRunning();
bool wirelessLinkEnabled();
void wirelessLinkSetEnabled(bool enabled);
String wirelessLinkAddress();
// Physical USB only: provision a trusted certificate and token to the local bridge.
void wirelessLinkProvision();
