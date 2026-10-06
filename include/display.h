#pragma once
#include <stdint.h>

// ST7789P panel (240x284) and CST816D touch on the Waveshare 1.83" module.
// spiHz: pixel clock. Jumper wires may need 10-20 MHz; a PCB or short ribbon handles 40 MHz.
bool displayBegin(uint32_t spiHz);
// Push rows [y0, y1) of a 240-wide little-endian RGB565 framebuffer.
void displayPresent(const uint16_t *pixels, int y0 = 0, int y1 = 284);
void displayBrightness(uint8_t level);
bool touchAvailable();
// Returns true while a finger is down; x/y are screen coordinates.
bool touchRead(int &x, int &y);
// Microseconds the last full displayPresent() took (for FPS measurements).
uint32_t displayLastPresentMicros();
