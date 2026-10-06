#include "display.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/semphr.h>
#include "board_pins.h"

namespace {
constexpr int W = 240, H = 284;
// Waveshare's demo drives this panel with no RAM offset and colour inversion on.
constexpr int GAP_X = 0, GAP_Y = 0;
constexpr int STRIP_ROWS = 24;
// 40 MHz is fine on a PCB; long loose jumper wires need a slower clock.
constexpr uint32_t SPI_HZ = 10 * 1000 * 1000;
constexpr uint8_t TOUCH_ADDR = 0x15;
constexpr int BL_CHANNEL = 7;

esp_lcd_panel_handle_t panel = nullptr;
uint16_t *strips[2] = {nullptr, nullptr};
SemaphoreHandle_t stripFree = nullptr;
bool touchFound = false;
uint32_t lastPresentMicros = 0;

bool IRAM_ATTR stripDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(stripFree, &woken);
  return woken == pdTRUE;
}

bool touchWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}
}  // namespace

bool displayBegin() {
  // Keep the microSD card deselected so LCD traffic on the shared bus is ignored.
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_LCD_BL, BL_CHANNEL);
  ledcWrite(BL_CHANNEL, 0);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = PIN_SPI_MISO;
  bus.sclk_io_num = PIN_SPI_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = W * STRIP_ROWS * 2 + 8;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  stripFree = xSemaphoreCreateCounting(2, 2);
  for (auto &strip : strips)
    strip = (uint16_t *)heap_caps_malloc(W * STRIP_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!stripFree || !strips[0] || !strips[1]) return false;

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t ioConfig = {};
  ioConfig.dc_gpio_num = PIN_LCD_DC;
  ioConfig.cs_gpio_num = PIN_LCD_CS;
  ioConfig.pclk_hz = SPI_HZ;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  ioConfig.spi_mode = 0;
  ioConfig.trans_queue_depth = 4;
  ioConfig.on_color_trans_done = stripDone;
  if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioConfig, &io) != ESP_OK) return false;

  esp_lcd_panel_dev_config_t panelConfig = {};
  panelConfig.reset_gpio_num = PIN_LCD_RST;
  panelConfig.color_space = ESP_LCD_COLOR_SPACE_RGB;
  panelConfig.bits_per_pixel = 16;
  if (esp_lcd_new_panel_st7789(io, &panelConfig, &panel) != ESP_OK) return false;
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_invert_color(panel, true);
  esp_lcd_panel_set_gap(panel, GAP_X, GAP_Y);
  esp_lcd_panel_disp_on_off(panel, true);

  // Reset the touch controller, then give it time to boot.
  pinMode(PIN_TP_INT, INPUT);
  pinMode(PIN_TP_RST, OUTPUT);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(60);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR);
  touchFound = Wire.endTransmission() == 0;
  if (touchFound) {
    touchWrite(0xFA, 0x40);  // interrupt periodically while touched
    touchWrite(0xFE, 0x01);  // stay awake so polling always gets an answer
  }
  return true;
}

void displayBrightness(uint8_t level) { ledcWrite(BL_CHANNEL, level); }

void displayPresent(const uint16_t *pixels, int y0, int y1) {
  if (!panel || !pixels) return;
  const uint32_t start = micros();
  int index = 0;
  for (int y = max(0, y0); y < min(H, y1); y += STRIP_ROWS, index ^= 1) {
    const int rows = min(STRIP_ROWS, min(H, y1) - y);
    xSemaphoreTake(stripFree, portMAX_DELAY);
    // Framebuffer is little-endian RGB565; the panel expects big-endian.
    const uint16_t *source = pixels + y * W;
    uint16_t *target = strips[index];
    for (int i = 0; i < rows * W; ++i) target[i] = __builtin_bswap16(source[i]);
    esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + rows, target);
  }
  // Wait until both strips are back so callers may reuse the framebuffer freely.
  xSemaphoreTake(stripFree, portMAX_DELAY);
  xSemaphoreTake(stripFree, portMAX_DELAY);
  xSemaphoreGive(stripFree);
  xSemaphoreGive(stripFree);
  if (y0 <= 0 && y1 >= H) lastPresentMicros = micros() - start;
}

uint32_t displayLastPresentMicros() { return lastPresentMicros; }

bool touchAvailable() { return touchFound; }

bool touchRead(int &x, int &y) {
  if (!touchFound) return false;
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  const uint8_t fingers = Wire.read();
  const uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if ((fingers & 0x0F) == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return x < W && y < H;
}
