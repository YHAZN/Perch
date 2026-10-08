#pragma once
#include <stdint.h>

// ST7789P panel (240x284) and CST816D touch on the Waveshare 1.83" module.
// Largest area sent in one transfer; LVGL renders in chunks of this many rows.
constexpr int DISPLAY_CHUNK_ROWS = 40;

// spiHz: pixel clock. Jumper wires may need 10-20 MHz; a PCB or short ribbon handles 40 MHz.
// onDrawDone runs in interrupt context when each displayDraw() transfer has finished.
bool displayBegin(uint32_t spiHz, void (*onDrawDone)());
// Queue a big-endian RGB565 rectangle [x1,x2) x [y1,y2) and return immediately (DMA).
// `pixels` must stay valid, in DMA-capable memory, until onDrawDone fires.
void displayDraw(int x1, int y1, int x2, int y2, const uint16_t *pixels);
void displayBrightness(uint8_t level);
bool touchAvailable();
// Developer: the most recent distinct raw touch readings, oldest first.
int touchRecent(int *xs, int *ys, int max);
// Returns true while a finger is down; x/y are screen coordinates.
bool touchRead(int &x, int &y);
