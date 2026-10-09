#include "camera.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_log.h>
#include <ESP32_OV5640_AF.h>

namespace {
CameraMode mode = CameraMode::Off;
uint16_t sensorPid = 0;  // known after the first start
OV5640 autofocus;
// Photos are hand-held: a full-size frame takes ~0.3 s to read out, and letting exposure run
// that long (or two frames, as before) smeared every indoor photo. Exposure is capped in time
// instead, and gain makes up the light: some grain is far easier to read than motion blur.
int stillGainCeiling = GAINCEILING_32X;
int stillAeLevel = 1;         // photos aim a little brighter than the sensor default
int stillExposureFrames = 1;  // >1 lets night mode stretch exposure over several frames
int stillMaxExposureMs = 40;  // 1/25 s
float stillLineUs = 150;      // one sensor line at full size; measured from the burst (frame time / VTS)
int stillVts = 0;
// Photo tuning (developer '7' command). -100 = leave the driver default.
int stillDenoise = -100, stillSharpness = -100, stillHts = 0, stillSettle = 0, stillSaturation = -100;

// OV5640 autofocus: Omnivision's focus firmware runs on the sensor's own MCU and is lost on
// power-up, so it is loaded once (~4 KB over SCCB) and reloaded if the sensor reports it
// missing. Status register 0x3029: 0x7F no firmware, 0x70 idle, 0x10 focused.
bool afLoaded = false;  // since this boot (the sensor loses it only on power loss)
bool lensHeld = false;  // the live view focused and the lens was frozen for the photo
bool afLoad(sensor_t *s) {
  const uint8_t status = s->get_reg(s, 0x3029, 0xFF);
  // Before loading, the status register can read anything; only trust it after a load.
  if (afLoaded && status != 0x7F && status != 0x7E) return true;
  if (!autofocus.start(s)) {
    Serial.println("CAMERA af: sensor id check failed");
    return false;
  }
  const uint32_t t = millis();
  const uint8_t rc = autofocus.focusInit();
  afLoaded = rc == 0;
  Serial.printf("CAMERA af firmware %s in %lums (status was %02X, now %02X)\n", afLoaded ? "loaded" : "FAILED",
                (unsigned long)(millis() - t), status, s->get_reg(s, 0x3029, 0xFF));
  return afLoaded;
}
// Send one AF command; the firmware clears 0x3023 when it has finished acting on it
// (for a single focus, that is when focusing is done).
bool afCommand(sensor_t *s, uint8_t command, uint32_t timeoutMs) {
  s->set_reg(s, 0x3023, 0xFF, 0x01);
  s->set_reg(s, 0x3022, 0xFF, command);
  const uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (s->get_reg(s, 0x3023, 0xFF) == 0x00) return true;
    delay(10);
  }
  return false;
}

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
  if (next == CameraMode::Still && mode == CameraMode::Off && (sensorPid == 0 || sensorPid == OV5640_PID)) {
    // Focus needs fast frames: a full-size focus search never finished within seconds.
    // Focus briefly in preview mode, then switch to full size with the lens held.
    if (cameraSetMode(CameraMode::Preview) && afLoaded) {
      sensor_t *s = esp_camera_sensor_get();
      const uint32_t t = millis();
      uint8_t status = 0;
      while (millis() - t < 2500) {
        status = s->get_reg(s, 0x3029, 0xFF);
        if (status == 0x10 || status == 0x20) break;
        delay(30);
      }
      Serial.printf("CAMERA prefocus %02X in %lums\n", status, (unsigned long)(millis() - t));
    }
  }
  lensHeld = false;
  if (mode == CameraMode::Preview && next == CameraMode::Still && afLoaded) {
    // Continuous focus has been tracking the scene at ~27 FPS; freeze the lens where it is.
    // A full-size focus search would take seconds (a few frames per second at 2048x1536).
    sensor_t *s = esp_camera_sensor_get();
    const uint8_t status = s ? s->get_reg(s, 0x3029, 0xFF) : 0;
    lensHeld = s && (status == 0x10 || status == 0x20);
    if (lensHeld && !afCommand(s, 0x06, 1000))
      Serial.println("CAMERA focus pause not acknowledged");  // keep lens position
    Serial.printf("CAMERA focus %s from live view (status %02X)\n", lensHeld ? "held" : "not locked", status);
  }
  if (mode != CameraMode::Off) {
    // The Sense board wires no power-down pin, so without this the sensor stays powered and
    // warm while "off". Software standby (0x3008 bit 6) keeps its registers and focus firmware.
    sensor_t *s = esp_camera_sensor_get();
    if (s && next == CameraMode::Off && s->id.PID == OV5640_PID) s->set_reg(s, 0x3008, 0x40, 0x40);
    esp_camera_deinit();
  }
  mode = CameraMode::Off;
  if (next == CameraMode::Off) return true;
  if (!psramFound()) return false;
  camera_config_t config = baseConfig();
  if (next == CameraMode::Still) {
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QXGA;
    // The driver's JPEG buffer holds ~630 KB. Measured on the OV5640 at 2048x1536: quality 8
    // overflowed it on busy scenes (no frame at all); 12 gives ~430 KB with headroom.
    // Two buffers let the burst take consecutive frames (~0.3 s apart instead of ~0.5 s).
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  } else {
    // Raw pixels: converting costs ~13 ms per frame vs ~80 ms to decode even a small JPEG.
    // Measured on the OV3660: ~10.5 FPS either way; the sensor, not the code, sets the rate.
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    // OV3660: 24 MHz input raises its frame rate. OV5640: keep 20 MHz (24 adds stripes);
    // its PLL is raised after start instead.
    if (sensorPid != OV5640_PID) config.xclk_freq_hz = 24000000;
    config.fb_count = 3;  // slack so the sensor rarely finds every buffer busy
    config.grab_mode = CAMERA_GRAB_LATEST;
  }
  // The driver's 2 KB task overflows its stack when it logs a frame overrun ("EV-VSYNC-OVF"),
  // which crashes the board. Dropped frames are harmless; keep its logging silent.
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  esp_log_level_set("camera", ESP_LOG_NONE);
  esp_log_level_set("gdma", ESP_LOG_NONE);  // harmless "no peripheral is connected" on every stop
  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("CAMERA INIT FAILED: %s (0x%x)\n", esp_err_to_name(result), result);
    return false;
  }
  sensor_t *sensor = esp_camera_sensor_get();
  sensorPid = sensor->id.PID;
  if (sensorPid == OV5640_PID) sensor->set_reg(sensor, 0x3008, 0x40, 0x00);  // wake from standby
  Serial.printf("CAMERA mode %d sensor PID 0x%04x\n", (int)next, sensor->id.PID);
  // Correct the mirrored worksheet in the sensor, so photos and preview match the scene.
  sensor->set_hmirror(sensor, !sensor->status.hmirror);
  if (next == CameraMode::Still && sensor->id.PID == OV5640_PID) {
    // No reset line: preview settings survive a driver restart, so photos set their own.
    // Low gain plus a longer exposure gives less noise (the purple speckle and column lines
    // get worse with gain); a burst of three then keeps the sharpest against hand shake.
    const int vts = (sensor->get_reg(sensor, 0x380E, 0xFF) << 8) | sensor->get_reg(sensor, 0x380F, 0xFF);
    stillVts = vts;
    int maxLines = vts * stillExposureFrames;
    if (stillMaxExposureMs > 0) maxLines = min(maxLines, max(16, (int)(stillMaxExposureMs * 1000 / stillLineUs)));
    Serial.printf("CAMERA photo exposure cap %d lines (%.0f ms)\n", maxLines, maxLines * stillLineUs / 1000);
    sensor->set_reg(sensor, 0x3A02, 0xFF, maxLines >> 8);
    sensor->set_reg(sensor, 0x3A03, 0xFF, maxLines & 0xFF);
    sensor->set_reg(sensor, 0x3A14, 0xFF, maxLines >> 8);
    sensor->set_reg(sensor, 0x3A15, 0xFF, maxLines & 0xFF);
    sensor->set_reg(sensor, 0x3A00, 0x04, stillExposureFrames > 1 ? 0x04 : 0x00);  // night mode
    sensor->set_gainceiling(sensor, (gainceiling_t)stillGainCeiling);
    sensor->set_ae_level(sensor, stillAeLevel);
    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    if (stillDenoise != -100 && sensor->set_denoise) sensor->set_denoise(sensor, stillDenoise);
    if (stillSharpness != -100 && sensor->set_sharpness) sensor->set_sharpness(sensor, stillSharpness);
    if (stillSaturation != -100) sensor->set_saturation(sensor, stillSaturation);
    if (stillHts > 0) {
      // Longer lines let exposure grow without analog gain (esp32-camera issue #229).
      sensor->set_reg(sensor, 0x380C, 0xFF, stillHts >> 8);
      sensor->set_reg(sensor, 0x380D, 0xFF, stillHts & 0xFF);
    }
    // Let exposure and white balance settle on the new settings before anyone keeps a frame.
    for (int i = 0; i < stillSettle; ++i) {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) esp_camera_fb_return(f);
    }
  }
  if (next == CameraMode::Preview && sensor->id.PID == OV5640_PID) sensor->set_ae_level(sensor, 0);
  if (next == CameraMode::Preview) {
    // Indoors, auto-exposure stretches each frame to gather light and the viewfinder drops
    // to ~10 FPS. For framing, prefer more gain (a little noise) over long exposures.
    sensor->set_aec2(sensor, 0);
    sensor->set_gainceiling(sensor, GAINCEILING_32X);
    if (sensor->id.PID == OV5640_PID) {
      // The driver's QVGA clock (PLL multiplier 8) caps the OV5640 at ~8 FPS. 24 measured
      // ~27 FPS with clean frames; higher multipliers overrun the ESP32-S3 camera input.
      sensor->set_reg(sensor, 0x3036, 0xFF, config.xclk_freq_hz > 20000000 ? 0x18 : 0x1C);
      // Night mode: only when gain runs out does the sensor stretch frames (to at most 2x),
      // so bright scenes stay ~27 FPS and dim rooms trade speed for a usable picture.
      const int vts = (sensor->get_reg(sensor, 0x380E, 0xFF) << 8) | sensor->get_reg(sensor, 0x380F, 0xFF);
      sensor->set_reg(sensor, 0x3A02, 0xFF, (vts * 2) >> 8);
      sensor->set_reg(sensor, 0x3A03, 0xFF, (vts * 2) & 0xFF);
      sensor->set_reg(sensor, 0x3A14, 0xFF, (vts * 2) >> 8);
      sensor->set_reg(sensor, 0x3A15, 0xFF, (vts * 2) & 0xFF);
      sensor->set_reg(sensor, 0x3A00, 0x04, 0x04);
    }
  }
  if (sensor->id.PID == OV5640_PID && afLoad(sensor)) {
    // Live view keeps refocusing as you move; a photo focuses once, on purpose, first.
    if (next == CameraMode::Preview) afCommand(sensor, 0x04, 500);  // continuous
  }
  mode = next;
  return true;
}

bool cameraFocus(uint32_t) {
  // Focus happens in preview mode before every photo (see cameraSetMode); report the result.
  return lensHeld;
}

void cameraSetStillGain(int ceiling) { stillGainCeiling = ceiling; }

void cameraNoteFrameTime(uint32_t us) {
  // Consecutive full-size frames: frame time / lines per frame = the time of one line.
  if (stillVts > 0 && us > 20000 && us < 2000000) stillLineUs = (float)us / stillVts;
}

void cameraReport() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  const int exposure =
      ((s->get_reg(s, 0x3500, 0x0F) << 16) | (s->get_reg(s, 0x3501, 0xFF) << 8) | s->get_reg(s, 0x3502, 0xFF)) >> 4;
  const int gain = ((s->get_reg(s, 0x350A, 0x03) << 8) | s->get_reg(s, 0x350B, 0xFF));
  const int ceiling = ((s->get_reg(s, 0x3A18, 0x03) << 8) | s->get_reg(s, 0x3A19, 0xFF));
  const int vts = (s->get_reg(s, 0x380E, 0xFF) << 8) | s->get_reg(s, 0x380F, 0xFF);
  Serial.printf("CAMERA exposure=%d lines (frame %d) gain=%.1fx ceiling=%.1fx\n", exposure, vts, gain / 16.0f,
                ceiling / 16.0f);
}

void cameraSetStillExposure(int aeLevel, int maxFrames) {
  stillAeLevel = aeLevel;
  stillExposureFrames = maxFrames < 1 ? 1 : maxFrames > 4 ? 4 : maxFrames;
}

bool cameraTune(const char *key, int value) {
  const String k(key);
  if (k == "dn") stillDenoise = value;
  else if (k == "sh") stillSharpness = value;
  else if (k == "hts") stillHts = value;
  else if (k == "set") stillSettle = value;
  else if (k == "sat") stillSaturation = value;
  else if (k == "ms") stillMaxExposureMs = value;  // 0 = no time cap (frames only)
  else return false;
  return true;
}
