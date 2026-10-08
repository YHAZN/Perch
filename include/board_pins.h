#pragma once
// XIAO ESP32-S3 Sense header pins. See docs/PINS.md before changing.
constexpr int PIN_LCD_CS = 1;    // D0
constexpr int PIN_LCD_DC = 2;    // D1
constexpr int PIN_TP_INT = 3;    // D2
constexpr int PIN_TP_RST = 4;    // D3, touch reset (moves onto D6 with LCD_RST once the battery needs D3)
constexpr int PIN_I2C_SDA = 5;   // D4
constexpr int PIN_I2C_SCL = 6;   // D5
constexpr int PIN_LCD_RST = 43;  // D6
constexpr int PIN_LCD_BL = 44;   // D7
constexpr int PIN_SPI_SCK = 7;   // D8, shared with microSD
constexpr int PIN_SPI_MISO = 8;  // D9, microSD only
constexpr int PIN_SPI_MOSI = 9;  // D10, shared with microSD
constexpr int PIN_SD_CS = 21;    // internal, also the user LED
// Battery sense: BAT+ through a 1:2 divider (2 x 100 kOhm) to this pin. -1 = not wired yet.
// When wired on D3 (GPIO4), TP_RST moves off D3 (see docs/PINS.md, "Battery").
constexpr int PIN_BATTERY = -1;
constexpr float BATTERY_DIVIDER = 2.0f;
