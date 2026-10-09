#pragma once
// XIAO ESP32-S3 Sense + Waveshare 1.83" touch LCD (ST7789 display, CST816 touch).
//
// Used inside the Sense board, no header pins needed: camera (DVP + SCCB) GPIO 10-18, 38-40,
// 47, 48; PDM microphone GPIO 41/42; microSD chip select GPIO 21 (also the orange user LED).
// All eleven header pins (D0-D10) are assigned below. The LCD has no MISO line and its own
// chip select, so it shares SCK/MOSI with the microSD slot; D9 (MISO) must stay free for SD.
// D6/D7 are UART0 TX/RX: they toggle during boot, which is why the panel is reset after boot.
constexpr int PIN_LCD_CS = 1;    // D0
constexpr int PIN_LCD_DC = 2;    // D1
constexpr int PIN_TP_INT = 3;    // D2
constexpr int PIN_TP_RST = 4;    // D3, touch reset (moves onto D6 with LCD_RST once the battery needs D3)
constexpr int PIN_I2C_SDA = 5;   // D4, touch (and any later I2C parts)
constexpr int PIN_I2C_SCL = 6;   // D5
constexpr int PIN_LCD_RST = 43;  // D6
constexpr int PIN_LCD_BL = 44;   // D7, backlight PWM
constexpr int PIN_SPI_SCK = 7;   // D8, shared with microSD
constexpr int PIN_SPI_MISO = 8;  // D9, microSD only
constexpr int PIN_SPI_MOSI = 9;  // D10, shared with microSD
constexpr int PIN_SD_CS = 21;    // internal, also the user LED
// Battery sense: BAT+ through a 1:2 divider (2 x 100 kOhm) to this pin. -1 = not wired yet.
// It goes on D3 (GPIO4); TP_RST then shares D6 with LCD_RST (both are active-low resets
// pulsed only at boot).
constexpr int PIN_BATTERY = -1;
constexpr float BATTERY_DIVIDER = 2.0f;
