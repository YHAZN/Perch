#pragma once
#include <stdint.h>
void initDeviceUi();
void handleDeviceButton(char command);
// Call every loop: physical touch input and the on-LCD live viewfinder.
void deviceTick();
// Milliseconds the main loop may rest before the next deviceTick().
uint32_t deviceIdleMs();
// Call whenever a serial byte arrives, so the LCD preview yields to the PC mirror.
void deviceNoteSerial();
