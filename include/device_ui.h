#pragma once
void initDeviceUi();
void handleDeviceButton(char command);
// Call every loop: physical touch input and the on-LCD live viewfinder.
void deviceTick();
// Call whenever a serial byte arrives, so the LCD preview yields to the PC mirror.
void deviceNoteSerial();
