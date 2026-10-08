// Part of device_ui.cpp (included there, one translation unit). LVGL glue: flush, touch with system edges, USB mirror
// frames, test injection.
// ---------- LVGL glue ----------
// Animation probe: timestamps of completed frames, for the 'F' command.
uint32_t frameStamps[64];
volatile int frameCount = 0;
bool probing = false;
void flush(lv_display_t *, const lv_area_t *area, uint8_t *pixels) {
  const int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  // Keep the full-screen copy for the USB mirror (little-endian, before the swap).
  for (int y = 0; y < h; ++y) memcpy(framebuffer + (area->y1 + y) * W + area->x1, pixels + y * w * 2, w * 2);
  // The panel wants big-endian pixels. Send by DMA and return: LVGL renders the next
  // chunk into the other buffer meanwhile; transferDone() releases this one.
  lv_draw_sw_rgb565_swap(pixels, w * h);
  displayDraw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, reinterpret_cast<uint16_t *>(pixels));
  if (probing && lv_display_flush_is_last(display) && frameCount < 64) frameStamps[frameCount++] = micros();
}
void drawDone() { lv_display_flush_ready(display); }
void readTouch(lv_indev_t *, lv_indev_data_t *data) {
  int x = 0, y = 0;
  bool pressed;
  if (injecting) {
    x = injectX;
    y = injectY;
    pressed = true;
  } else pressed = touchRead(x, y);
  static unsigned long moveAt = 0;
  static int speed = 0;   // px/s, smoothed; positive = upward
  static int speedX = 0;  // px/s, smoothed; positive = rightward
  // An edge touch is a candidate until the finger shows its direction. Until then the app
  // sees nothing; if it turns out to be a scroll or a tap, the app gets it after all.
  static bool committed = false;
  static bool passThrough = false;  // candidate rejected: this touch belongs to the app
  static int tapPending = 0;        // 2 = report a press at (tapX, tapY), 1 = then a release
  static int tapX = 0, tapY = 0;
  if (pressed) {
    lastActivity = millis();
    if (screenAsleep) {
      wakeScreen();
      swallowTouch = true;
    } else if (screenDim) wakeScreen();
  }
  if (swallowTouch) {  // the touch that woke the screen does nothing else
    if (!pressed) swallowTouch = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (tapPending && !pressed) {
    data->point.x = tapX;
    data->point.y = tapY;
    data->state = tapPending-- == 2 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return;
  }
  if (pressed) {
    const unsigned long now = millis();
    if (!fingerDown) {
      fingerDown = true;
      downX = x;
      downY = y;
      speed = speedX = 0;
      committed = passThrough = false;
      edge = (injecting && !injectEdges)                                 ? Edge::None
             : downY >= HOME_ZONE                                        ? Edge::Home
             : (downY < TOP_ZONE && !ccOpen)                             ? Edge::Control
             : (downX < BACK_ZONE && !ccOpen && current != Screen::Face) ? Edge::Back
                                                                         : Edge::None;
    } else if (now > moveAt) {
      speed = (speed + (lastY - y) * 1000 / (int)(now - moveAt)) / 2;
      speedX = (speedX + (x - lastX) * 1000 / (int)(now - moveAt)) / 2;
    }
    moveAt = now;
    lastX = x;
    lastY = y;
    const int dx = x - downX, dy = y - downY;
    if (edge != Edge::None && !committed) {
      // Commit when the movement matches the edge; give the touch to the app when it does not.
      if (edge == Edge::Home && -dy > 8 && -dy > abs(dx)) committed = true;
      else if (edge == Edge::Control && dy > 8 && dy > abs(dx)) committed = true;
      else if (edge == Edge::Back && dx > 12 && dx > 2 * abs(dy)) committed = true;
      else if ((edge == Edge::Back && abs(dy) > 10 && abs(dy) > dx) ||
               ((edge == Edge::Home || edge == Edge::Control) && abs(dx) > 12 && abs(dx) > abs(dy))) {
        edge = Edge::None;
        passThrough = true;
      }
    }
    if (committed) {
      if (edge == Edge::Home) {
        if (ccOpen) dragControlClosed(-dy);
        else dragHome(-dy);
      }
      if (edge == Edge::Control) dragControl(dy);
      if (edge == Edge::Back) dragBack(dx);
    }
  } else if (fingerDown) {
    fingerDown = false;
    if (committed) {
      if (edge == Edge::Home) {
        if (ccOpen) releaseControlClosed(max(0, downY - lastY), speed);
        else releaseHome(max(0, downY - lastY), speed);
      }
      if (edge == Edge::Control) releaseControl(max(0, lastY - downY), speed);
      if (edge == Edge::Back) releaseBack(max(0, lastX - downX), speedX);
    } else if (edge != Edge::None) {
      // A tap that started on an edge: deliver it to the app (back arrow, edge buttons).
      tapPending = 2;
      tapX = downX;
      tapY = downY;
    }
    edge = Edge::None;
    committed = false;
  }
  if (edge != Edge::None) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  (void)passThrough;
  data->point.x = pressed ? x : lastX;
  data->point.y = pressed ? y : lastY;
  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
uint32_t tick() { return millis(); }
const char *screenName() {
  if (ccOpen) return "control";
  if (current == Screen::Ask && !lv_obj_has_flag(busy, LV_OBJ_FLAG_HIDDEN)) return "busy";
  switch (current) {
    case Screen::Face: return "face";
    case Screen::Apps: return "apps";
    case Screen::Camera: return "home";  // historical mirror name for the camera view
    case Screen::Ask: return "ai";
    case Screen::Answer: return "answer";
    case Screen::Photos: return "photo";
    case Screen::History: return "history";
    case Screen::Remote: return "remote";
    case Screen::Wifi: return "wifi";
    case Screen::Gestures: return "gestures";
    case Screen::Password: return "password";
    case Screen::Settings: return "status";
    case Screen::Model: return "model";
    default: return "error";
  }
}
void settle() {
  // Let LVGL finish transitions so the USB mirror sees the resulting screen, not motion.
  const unsigned long start = millis();
  do {
    lv_timer_handler();
    delay(5);
  } while (lv_anim_count_running() > 1 && millis() - start < 700);
  lv_refr_now(display);
}
void sendFrame() {
  settle();
  const uint8_t *bytes = reinterpret_cast<uint8_t *>(framebuffer);
  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < W * H * 2; ++i) {
    checksum ^= bytes[i];
    checksum *= 16777619u;
  }
  Serial.printf("SCREEN_BEGIN %u %lu %s %d %d %08lx %s %d %d %d\n", W * H * 2, ++sequence, screenName(), 1, 1,
                (unsigned long)checksum, useGemini ? "gemini" : "gpt", settings.isKey("gemini-key"),
                settings.isKey("gpt-key"), current == Screen::Camera && !cameraOff);
  if (!sendAcknowledged(bytes, W * H * 2)) {
    Serial.println("\nSCREEN_ERROR USB write timed out");
    return;
  }
  Serial.println("\nSCREEN_END");
}
void injectTap(int x, int y, int holdMs = 80) {
  injectX = x;
  injectY = y;
  injecting = true;
  const unsigned long start = millis();
  do {
    lv_timer_handler();
    delay(20);
  } while (millis() - start < (unsigned long)holdMs);
  injecting = false;
  for (int i = 0; i < 4; ++i) {
    lv_timer_handler();
    delay(20);
  }
}
// A finger drag from (x1,y1) to (x2,y2) over `ms`, then release. Edges apply.
void injectDrag(int x1, int y1, int x2, int y2, int ms) {
  injecting = true;
  injectEdges = true;
  const int steps = max(4, ms / 20);
  for (int i = 0; i <= steps; ++i) {
    injectX = x1 + (x2 - x1) * i / steps;
    injectY = y1 + (y2 - y1) * i / steps;
    lv_timer_handler();
    delay(20);
  }
  injecting = false;
  for (int i = 0; i < 6; ++i) {
    lv_timer_handler();
    delay(20);
  }
  injectEdges = false;
}
void tuneCamera(char profile) {
  previewWanted = false;
  if (!cameraSwitch(CameraMode::Still)) return;
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;
  static bool saved = false;
  static int sharpness, contrast, denoise;
  if (!saved) {
    sharpness = sensor->status.sharpness;
    contrast = sensor->status.contrast;
    denoise = sensor->status.denoise;
    saved = true;
  }
  int errors = 0;
  errors |= sensor->set_framesize(sensor, profile == '5' ? FRAMESIZE_UXGA : FRAMESIZE_QXGA);
  errors |= sensor->set_quality(sensor, profile == '1' ? 8 : profile == '5' ? 1 : 4);
  errors |= sensor->set_contrast(sensor, (profile == '3' || profile == '4') ? 1 : contrast);
  errors |= sensor->set_sharpness(sensor, (profile == '3' || profile == '4') ? 2 : sharpness);
  errors |= sensor->set_denoise(sensor, (profile == '3' || profile == '4') ? 0 : denoise);
  errors |= sensor->set_exposure_ctrl(sensor, profile == '4' ? 0 : 1);
  errors |= sensor->set_gain_ctrl(sensor, 1);
  if (profile == '4') errors |= sensor->set_aec_value(sensor, 300);
  Serial.printf("TUNING profile=%c errors=%d\n", profile, errors);
  for (int i = 0; i < 3; ++i) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
    delay(100);
  }
}
void previewToUsb() {
  if (current != Screen::Camera || cameraOff) {
    Serial.println("CAPTURE FAILED Preview is not active");
    return;
  }
  if (!ensurePreviewMode(8000)) {
    Serial.println("CAPTURE FAILED Preview setup failed");
    return;
  }
  xSemaphoreTake(camLock, portMAX_DELAY);
  camera_fb_t *f = esp_camera_fb_get();
  if (!f) {
    xSemaphoreGive(camLock);
    Serial.println("CAPTURE FAILED No preview frame");
    return;
  }
  // Preview frames are raw pixels; compress one for the PC mirror.
  uint8_t *jpeg = nullptr;
  size_t len = 0;
  const bool raw = f->format != PIXFORMAT_JPEG;
  const bool encoded = raw ? frame2jpg(f, 60, &jpeg, &len) : true;
  if (!raw) {
    len = f->len;
    jpeg = (uint8_t *)ps_malloc(len);
    if (jpeg) memcpy(jpeg, f->buf, len);
  }
  esp_camera_fb_return(f);
  xSemaphoreGive(camLock);
  if (!encoded || !jpeg) {
    Serial.println("CAPTURE FAILED Preview encode failed");
    return;
  }
  Serial.printf("JPEG_BEGIN %u\n", len);
  const bool sent = sendAcknowledged(jpeg, len);
  free(jpeg);
  Serial.println(sent ? "\nJPEG_END" : "\nCAPTURE FAILED Preview transfer failed");
}
void restoreLatestPhoto() {
  // The newest stored photo comes back after a reboot, so Ask and the face have it.
  refreshPhotos();
  if (!photoCount) return;
  uint8_t *jpeg;
  size_t len;
  if (!loadPhoto(photoIds[0], jpeg, len)) return;
  if (loadPhotoInto(photoIds[0], photoPixels)) {
    savedJpeg = jpeg;
    jpegBytes = jpegCapacity = len;
    latestPhotoId = photoIds[0];
  } else {
    free(jpeg);
  }
}
