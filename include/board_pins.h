#pragma once
// XIAO ESP32-S3 Sense header pins. See docs/PINS.md before changing.
constexpr int PIN_LCD_CS = 1;      // D0
constexpr int PIN_LCD_DC = 2;      // D1
constexpr int PIN_TP_INT = 3;      // D2
constexpr int PIN_BATTERY = 4;     // D3, reserved for battery divider
constexpr int PIN_I2C_SDA = 5;     // D4
constexpr int PIN_I2C_SCL = 6;     // D5
constexpr int PIN_PANEL_RST = 43;  // D6, LCD_RST and TP_RST together
constexpr int PIN_LCD_BL = 44;     // D7
constexpr int PIN_SPI_SCK = 7;     // D8, shared with microSD
constexpr int PIN_SPI_MISO = 8;    // D9, microSD only
constexpr int PIN_SPI_MOSI = 9;    // D10, shared with microSD
constexpr int PIN_SD_CS = 21;      // internal, also the user LED
