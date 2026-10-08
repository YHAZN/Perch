#include "display.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include "board_pins.h"

namespace {
constexpr int W = 240;
// Waveshare's demo drives this panel with no RAM offset and colour inversion on.
constexpr int GAP_X = 0, GAP_Y = 0;
constexpr uint8_t TOUCH_ADDR = 0x15;
constexpr int BL_CHANNEL = 7;

esp_lcd_panel_handle_t panel = nullptr;
void (*drawDone)() = nullptr;
bool touchFound = false;

bool IRAM_ATTR transferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  if (drawDone) drawDone();
  return false;
}

bool touchWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}
}  // namespace

bool displayBegin(uint32_t spiHz, void (*onDrawDone)()) {
  drawDone = onDrawDone;
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
  bus.max_transfer_sz = W * DISPLAY_CHUNK_ROWS * 2 + 8;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t ioConfig = {};
  ioConfig.dc_gpio_num = PIN_LCD_DC;
  ioConfig.cs_gpio_num = PIN_LCD_CS;
  ioConfig.pclk_hz = spiHz;
  ioConfig.lcd_cmd_bits = 8;
  ioConfig.lcd_param_bits = 8;
  ioConfig.spi_mode = 0;
  ioConfig.trans_queue_depth = 4;
  ioConfig.on_color_trans_done = transferDone;
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

void displayDraw(int x1, int y1, int x2, int y2, const uint16_t *pixels) {
  if (panel) esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
}

bool touchAvailable() { return touchFound; }

// The last raw touch readings (for checking how the panel reads near its edges: 'J').
int rawX[16], rawY[16], rawCount = 0;
int touchRecent(int *xs, int *ys, int max) {
  const int n = min(max, min(rawCount, 16));
  for (int i = 0; i < n; ++i) {
    const int k = (rawCount - n + i) % 16;
    xs[i] = rawX[k];
    ys[i] = rawY[k];
  }
  return n;
}
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
  static int lastX = -1, lastY = -1;
  if (x != lastX || y != lastY) {
    rawX[rawCount % 16] = x;
    rawY[rawCount % 16] = y;
    ++rawCount;
    lastX = x;
    lastY = y;
  }
  // Near the rounded edges the panel can report a little past the screen: that is still a
  // finger on the glass, so clamp it rather than dropping the touch.
  if (x >= W + 40 || y >= 284 + 40) return false;
  x = min(x, W - 1);
  y = min(y, 283);
  return true;
}
