#include "camera.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_log.h>

namespace {
CameraMode mode = CameraMode::Off;

camera_config_t baseConfig() {
  // Seeed XIAO ESP32-S3 Sense camera connector mapping.
  camera_config_t config = {};
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.pin_xclk = 10;
  config.pin_sccb_sda = 40;
  config.pin_sccb_scl = 39;
  config.pin_d0 = 15;
  config.pin_d1 = 17;
  config.pin_d2 = 18;
  config.pin_d3 = 16;
  config.pin_d4 = 14;
  config.pin_d5 = 12;
  config.pin_d6 = 11;
  config.pin_d7 = 48;
  config.pin_vsync = 38;
  config.pin_href = 47;
  config.pin_pclk = 13;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  return config;
}
}  // namespace

CameraMode cameraMode() { return mode; }

bool cameraSetMode(CameraMode next) {
  if (next == mode) return true;
  if (mode != CameraMode::Off) esp_camera_deinit();
  mode = CameraMode::Off;
  if (next == CameraMode::Off) return true;
  if (!psramFound()) return false;
  camera_config_t config = baseConfig();
  if (next == CameraMode::Still) {
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QXGA;
    config.jpeg_quality = 8;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  } else {
    // Raw pixels: converting costs ~13 ms per frame vs ~80 ms to decode even a small JPEG.
    // Measured on the OV3660: ~10.5 FPS either way; the sensor, not the code, sets the rate.
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.xclk_freq_hz = 24000000;
    config.fb_count = 3;  // slack so the sensor rarely finds every buffer busy
    config.grab_mode = CAMERA_GRAB_LATEST;
  }
  // The driver's 2 KB task overflows its stack when it logs a frame overrun ("EV-VSYNC-OVF"),
  // which crashes the board. Dropped frames are harmless; keep its logging silent.
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  esp_log_level_set("camera", ESP_LOG_NONE);
  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("CAMERA INIT FAILED: %s (0x%x)\n", esp_err_to_name(result), result);
    return false;
  }
  sensor_t *sensor = esp_camera_sensor_get();
  Serial.printf("CAMERA mode %d sensor PID 0x%04x\n", (int)next, sensor->id.PID);
  // Correct the mirrored worksheet in the sensor, so photos and preview match the scene.
  sensor->set_hmirror(sensor, !sensor->status.hmirror);
  if (next == CameraMode::Preview) {
    // Indoors, auto-exposure stretches each frame to gather light and the viewfinder drops
    // to ~10 FPS. For framing, prefer more gain (a little noise) over long exposures.
    sensor->set_aec2(sensor, 0);
    sensor->set_gainceiling(sensor, GAINCEILING_32X);
  }
  mode = next;
  return true;
}
