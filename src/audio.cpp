#include "audio.h"
#include <Arduino.h>
#include <driver/i2s.h>

namespace {
constexpr size_t HEADER = 44;
constexpr size_t MAX_BYTES = MIC_RATE * 2 * MIC_MAX_SECONDS;
// PDM samples are quiet; this gain makes normal speech use a sensible part of the range.
constexpr int GAIN = 8;
// The PDM filter settles for a moment after starting; that start is a loud thump.
constexpr size_t SETTLE_BYTES = MIC_RATE * 2 / 2;  // 500 ms

uint8_t *buffer = nullptr;  // WAV header + PCM
volatile size_t pcmBytes = 0;
volatile bool recording = false, stopRequested = false;
volatile int level = 0;
bool driverReady = false;

void writeHeader(size_t dataBytes) {
  auto u32 = [](uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
  };
  auto u16 = [](uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
  };
  memcpy(buffer, "RIFF", 4);
  u32(buffer + 4, 36 + dataBytes);
  memcpy(buffer + 8, "WAVEfmt ", 8);
  u32(buffer + 16, 16);
  u16(buffer + 20, 1);  // PCM
  u16(buffer + 22, 1);  // mono
  u32(buffer + 24, MIC_RATE);
  u32(buffer + 28, MIC_RATE * 2);  // byte rate
  u16(buffer + 32, 2);             // block align
  u16(buffer + 34, 16);            // bits per sample
  memcpy(buffer + 36, "data", 4);
  u32(buffer + 40, dataBytes);
}

bool installDriver() {
  // ESP-IDF I2S driver in PDM receive mode (the Arduino I2S wrapper dropped half the samples).
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = MIC_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.dma_buf_count = 8;
  config.dma_buf_len = 512;
  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = 42;  // PDM clock
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = 41;  // PDM data
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  return true;
}

void recordTask(void *) {
  int16_t chunk[512];
  size_t settled = 0;
  i2s_zero_dma_buffer(I2S_NUM_0);
  while (!stopRequested && pcmBytes + sizeof(chunk) <= MAX_BYTES) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, chunk, sizeof(chunk), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) continue;
    if (settled < SETTLE_BYTES) {
      settled += got;
      continue;
    }
    const int samples = got / 2;
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
      const int v = constrain((int)chunk[i] * GAIN, -32768, 32767);
      chunk[i] = (int16_t)v;
      peak = max(peak, abs(v));
    }
    memcpy(buffer + HEADER + pcmBytes, chunk, samples * 2);
    pcmBytes += samples * 2;
    level = (level * 3 + min(100, peak * 100 / 20000)) / 4;
  }
  writeHeader(pcmBytes);
  recording = false;
  vTaskDelete(nullptr);
}
}  // namespace

bool micStart() {
  if (recording) return true;
  if (!buffer) buffer = (uint8_t *)ps_malloc(HEADER + MAX_BYTES);
  if (!buffer) return false;
  if (!driverReady) driverReady = installDriver();
  if (!driverReady) return false;
  pcmBytes = 0;
  level = 0;
  stopRequested = false;
  recording = true;
  if (xTaskCreatePinnedToCore(recordTask, "mic", 6144, nullptr, 3, nullptr, 0) != pdPASS) {
    recording = false;
    return false;
  }
  return true;
}

void micStop() {
  if (!recording) return;
  stopRequested = true;
  while (recording) delay(2);
}

bool micRecording() { return recording; }
int micLevel() { return recording ? level : 0; }
float micSeconds() { return pcmBytes / (2.0f * MIC_RATE); }

const uint8_t *micWav(size_t &length) {
  if (!buffer || recording || !pcmBytes) {
    length = 0;
    return nullptr;
  }
  length = HEADER + pcmBytes;
  return buffer;
}
