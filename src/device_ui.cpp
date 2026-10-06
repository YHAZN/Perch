#include "device_ui.h"
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <math.h>
#include "ai_client.h"

namespace {
constexpr int W = 240, H = 284;
constexpr int TEXT_TOP = 55, LINE_HEIGHT = 22, FOOTER_TOP = H - 36;
constexpr int LINES_PER_PAGE = (FOOTER_TOP - TEXT_TOP - 4) / LINE_HEIGHT + 1;
constexpr uint16_t BG = 0x0841, PANEL = 0x18e3, INK = 0xf7be, MUTED = 0x9cd3, ACCENT = 0x4d9f;
class Surface : public Adafruit_GFX {
 public:
  uint16_t *pixels = nullptr;
  Surface() : Adafruit_GFX(W, H) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (pixels && x >= 0 && y >= 0 && x < W && y < H) pixels[y * W + x] = color;
  }
} screen;
enum class View { Desk, Home, Photo, AI, History, Answer, Status, Error };
enum class AppId { Camera, AI, Photos, History, Settings };
struct AppEntry {
  const char *name;
  char command;
  uint16_t tint;
};
const AppEntry apps[] = {{"Camera", 'C', 0x245f},
                         {"AI", 'A', 0x03b5},
                         {"Photos", 'p', 0x7997},
                         {"History", 'H', 0x5a9f},
                         {"Settings", 'i', 0x4208}};
View view = View::Desk;
AppId activeApp = AppId::Camera;
Preferences settings;
bool useGemini = true;
bool liveRequested = false;
uint16_t *livePixels = nullptr;
std::vector<String> lines;
int page = 0;
uint16_t *photo = nullptr;
bool rotatePhoto = false;
size_t jpegBytes = 0;
uint8_t *savedJpeg = nullptr, *rgbScratch = nullptr;
size_t jpegCapacity = 0, rgbCapacity = 0;
int photoWidth = 0, photoHeight = 0;
String problem;
String lastAnswer;
bool lastAnswerGemini = true;
unsigned long sequence = 0;
const char *demo =
    "Sample answer\n\nThis text lives on the ESP32. It is not an AI analysis of your photo.\n\nThe device controls the "
    "layout, line wrapping, pages and every pixel you see.\n\nThe finished flow is capture, send to AI, then read the "
    "answer. No cloud request is sent in this demo.";

// Render fonts at twice the target size, then blend coverage into RGB565.
// This smooths the glyph edges on the actual device framebuffer.
void smoothText(const String &text, int x, int baseline, uint16_t color, const GFXfont *font) {
  GFXcanvas1 glyphs(480, 96);
  if (!glyphs.getBuffer()) return;
  glyphs.fillScreen(0);
  glyphs.setFont(font);
  glyphs.setTextColor(1);
  glyphs.setTextWrap(false);
  glyphs.setCursor(0, 72);
  glyphs.print(text);
  for (int gy = 0; gy < 48; ++gy)
    for (int gx = 0; gx < 240; ++gx) {
      int coverage = 0;
      for (int sy = 0; sy < 2; ++sy)
        for (int sx = 0; sx < 2; ++sx) coverage += glyphs.getPixel(gx * 2 + sx, gy * 2 + sy) ? 1 : 0;
      int px = x + gx, py = baseline - 36 + gy;
      if (!coverage || px < 0 || px >= W || py < 0 || py >= H) continue;
      uint16_t bg = screen.pixels[py * W + px];
      int r = (((color >> 11) & 31) * coverage + ((bg >> 11) & 31) * (4 - coverage) + 2) / 4;
      int g = (((color >> 5) & 63) * coverage + ((bg >> 5) & 63) * (4 - coverage) + 2) / 4;
      int b = ((color & 31) * coverage + (bg & 31) * (4 - coverage) + 2) / 4;
      screen.drawPixel(px, py, (r << 11) | (g << 5) | b);
    }
}
void small(const String &text, int x, int y, uint16_t color = MUTED) {
  smoothText(text, x, y + 8, color, &FreeSans18pt7b);
}
void title(const char *text, int y) { smoothText(text, 14, y, INK, &FreeSansBold24pt7b); }
void body(const String &text, int x, int baseline, uint16_t color = INK) {
  smoothText(text, x, baseline, color, &FreeSans18pt7b);
}
void header(const char *label) {
  screen.fillScreen(BG);
  body(label, view == View::Error ? 38 : 18, 23, INK);
  if (view == View::Error) {
    screen.drawLine(23, 10, 16, 17, MUTED);
    screen.drawLine(16, 17, 23, 24, MUTED);
  }
}
void cameraIcon(int x, int y, uint16_t color) {
  screen.drawRoundRect(x - 10, y - 7, 20, 14, 3, color);
  screen.drawCircle(x, y, 4, color);
  screen.drawFastHLine(x - 4, y - 10, 8, color);
}
void appIcon(int index, int x, int y) {
  screen.fillRoundRect(x - 24, y - 24, 48, 48, 13, apps[index].tint);
  if (index == 0) cameraIcon(x, y, INK);
  else if (index == 1) {
    screen.drawRoundRect(x - 13, y - 10, 26, 19, 5, INK);
    screen.drawLine(x - 7, y + 9, x - 7, y + 15, INK);
    screen.drawLine(x - 7, y + 15, x, y + 9, INK);
    screen.drawFastHLine(x - 7, y - 3, 14, INK);
    screen.drawFastHLine(x - 7, y + 2, 10, INK);
  } else if (index == 2) {
    screen.drawRoundRect(x - 13, y - 12, 26, 24, 4, INK);
    screen.fillCircle(x - 5, y - 5, 3, INK);
    screen.drawLine(x - 11, y + 9, x - 3, y, INK);
    screen.drawLine(x - 3, y, x + 3, y + 5, INK);
    screen.drawLine(x + 3, y + 5, x + 7, y, INK);
    screen.drawLine(x + 7, y, x + 11, y + 7, INK);
  } else if (index == 3) {
    screen.drawCircle(x, y, 12, INK);
    screen.drawLine(x, y, x, y - 7, INK);
    screen.drawLine(x, y, x + 7, y + 3, INK);
  } else {
    screen.drawCircle(x, y, 10, INK);
    screen.drawCircle(x, y, 4, INK);
    for (int j = 0; j < 8; ++j) {
      float a = j * PI / 4;
      screen.drawLine(x + 10 * cos(a), y + 10 * sin(a), x + 14 * cos(a), y + 14 * sin(a), INK);
    }
  }
}
int appX(int i) { return i == 4 ? 120 : (i % 2 ? 176 : 64); }
int appY(int i) { return 48 + (i / 2) * 84; }
void homeIndicator() { screen.fillRoundRect(96, 278, 48, 3, 2, MUTED); }
void footer(const char *, const String &) {
  if (view == View::Home) {
    screen.fillRect(0, 212, W, 72, BG);
    screen.drawCircle(120, 246, 24, INK);
    screen.drawCircle(120, 246, 23, INK);
    screen.fillCircle(120, 246, 18, INK);
    if (jpegBytes) {
      screen.drawRoundRect(25, 234, 30, 24, 5, MUTED);
      screen.drawLine(29, 253, 38, 245, INK);
      screen.drawLine(38, 245, 51, 254, INK);
    }
  } else if (view == View::AI) {
    screen.fillRoundRect(24, 220, 192, 40, 20, ACCENT);
    body(jpegBytes ? "Ask AI" : "Capture & ask", jpegBytes ? 94 : 60, 246, BG);
  }
  homeIndicator();
}
int textWidth(const String &text) {
  int16_t x, y;
  uint16_t w, h;
  screen.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  return (w + 1) / 2;
}
void wrap(const char *text) {
  lines.clear();
  screen.setFont(&FreeSans18pt7b);
  String line, word;
  auto append = [&]() {
    if (word.isEmpty()) return;
    String candidate = line + (line.isEmpty() ? "" : " ") + word;
    if (textWidth(candidate) <= 210) {
      line = candidate;
      word = "";
      return;
    }
    if (!line.isEmpty()) {
      lines.push_back(line);
      line = "";
    }
    for (size_t i = 0; i < word.length(); ++i) {
      if (textWidth(line + word[i]) > 210) {
        lines.push_back(line);
        line = "";
      }
      line += word[i];
    }
    word = "";
  };
  for (const char *p = text; *p; ++p) {
    if (*p == ' ' || *p == '\n') {
      append();
      if (*p == '\n') {
        lines.push_back(line);
        line = "";
      }
    } else word += *p;
  }
  append();
  if (!line.isEmpty()) lines.push_back(line);
  if (lines.empty()) lines.push_back("");
  page = 0;
}
int pageCount() { return max(1, (static_cast<int>(lines.size()) + LINES_PER_PAGE - 1) / LINES_PER_PAGE); }
void render() {
  screen.setTextWrap(false);
  switch (view) {
    case View::Desk: {
      screen.fillScreen(BG);
      for (int i = 0; i < 5; ++i) {
        int x = appX(i), y = appY(i);
        appIcon(i, x, y);
        screen.setFont(&FreeSans18pt7b);
        body(apps[i].name, x - textWidth(apps[i].name) / 2, y + 39, INK);
      }
      break;
    }
    case View::Home:
      header("Camera");
      screen.fillRect(0, 32, 240, 180, PANEL);
      if (liveRequested && livePixels) {
        for (int y = 0; y < 180; ++y)
          for (int x = 0; x < 240; ++x) screen.drawPixel(x, y + 32, livePixels[(y * 108 / 180) * 144 + x * 144 / 240]);
      } else {
        cameraIcon(120, 113, INK);
        body("Tap to preview", 66, 147, MUTED);
      }
      footer("", "");
      break;
    case View::Photo:
      header("Photos");

      if (photo && jpegBytes) {
        for (int y = 0; y < 180; ++y)
          for (int x = 0; x < 240; ++x) screen.drawPixel(x, y + 32, photo[(y * 162 / 180) * 216 + x * 216 / 240]);
      }
      if (!jpegBytes) {
        cameraIcon(120, 117, MUTED);
        body("No photos", 83, 159, MUTED);
      }
      footer("", "");
      break;
    case View::AI:
      header("AI");
      small(useGemini ? "Gemini" : "GPT", 132, 14);
      cameraIcon(210, 17, INK);
      if (photo && jpegBytes) {
        for (int y = 0; y < 180; ++y)
          for (int x = 0; x < 240; ++x) screen.drawPixel(x, y + 32, photo[(y * 162 / 180) * 216 + x * 216 / 240]);
      } else {
        screen.drawRoundRect(46, 64, 148, 112, 16, PANEL);
        cameraIcon(120, 111, MUTED);
        body("Take a photo", 73, 151, MUTED);
      }
      footer("", "");
      break;
    case View::History:
      header("History");
      if (lastAnswer.isEmpty()) {
        body("No answers yet", 61, 137, MUTED);
      } else {
        screen.fillRoundRect(16, 54, 208, 72, 14, PANEL);
        body("Last answer", 28, 80);
        body(lastAnswerGemini ? "Gemini" : "GPT", 28, 107, MUTED);
        screen.drawLine(201, 80, 207, 86, MUTED);
        screen.drawLine(207, 86, 201, 92, MUTED);
      }
      footer("", "");
      break;
    case View::Answer:
      header(activeApp == AppId::History ? "History" : "AI");
      small(String(page + 1) + "/" + String(pageCount()), 178, 14);
      for (int i = 0; i < LINES_PER_PAGE; ++i) {
        int index = page * LINES_PER_PAGE + i;
        if (index < (int)lines.size()) body(lines[index], 14, TEXT_TOP + i * LINE_HEIGHT);
      }
      footer("SWIPE / HOME", String(page + 1) + " / " + String(pageCount()));
      break;
    case View::Status:
      header("Settings");
      screen.fillRoundRect(16, 52, 208, 44, 12, PANEL);
      body("AI provider", 28, 80);
      body(useGemini ? "Gemini" : "GPT", 145, 80);
      body("API key", 18, 133, MUTED);
      body(settings.isKey(useGemini ? "gemini-key" : "gpt-key") ? "Saved" : "Missing", 145, 133);
      body("Wi-Fi", 18, 177, MUTED);
      body(WiFi.status() == WL_CONNECTED ? "Connected" : "Offline", 125, 177);
      footer("RUNNING ON ESP32", "V0.2");
      break;
    case View::Error:
      header("Notice");
      wrap(problem.c_str());
      for (int i = 0; i < 6 && i < (int)lines.size(); ++i) body(lines[i], 14, TEXT_TOP + i * LINE_HEIGHT, MUTED);
      if (activeApp == AppId::AI && jpegBytes) {
        screen.fillRoundRect(16, 200, 208, 38, 12, PANEL);
        body("Retry same photo", 40, 225, INK);
      }
      footer("PRESS CAPTURE", "USB DISPLAY");
      break;
  }
}
// Send bounded, paced bursts; the host provisions a large receive buffer.
bool sendAcknowledged(const uint8_t *bytes, size_t length) {
  size_t offset = 0;
  unsigned long started = millis();
  while (offset < length && millis() - started < 12000) {
    offset += Serial.write(bytes + offset, min((size_t)256, length - offset));
    delay(2);
  }
  return offset == length;
}
void sendFrame() {
  if (!screen.pixels) {
    Serial.println("SCREEN_ERROR framebuffer unavailable");
    return;
  }
  render();
  const uint8_t *bytes = reinterpret_cast<uint8_t *>(screen.pixels);
  uint32_t checksum = 2166136261u;
  for (size_t i = 0; i < W * H * 2; ++i) {
    checksum ^= bytes[i];
    checksum *= 16777619u;
  }
  const char *name = view == View::Desk      ? "desk"
                     : view == View::Home    ? "home"
                     : view == View::Photo   ? "photo"
                     : view == View::AI      ? "ai"
                     : view == View::History ? "history"
                     : view == View::Answer  ? "answer"
                     : view == View::Status  ? "status"
                                             : "error";
  Serial.printf("SCREEN_BEGIN %u %lu %s %d %d %08lx %s %d %d %d\n", W * H * 2, ++sequence, name, page + 1, pageCount(),
                (unsigned long)checksum, useGemini ? "gemini" : "gpt", settings.isKey("gemini-key"),
                settings.isKey("gpt-key"), liveRequested && view == View::Home);
  if (!sendAcknowledged(bytes, W * H * 2)) {
    Serial.println("\nSCREEN_ERROR USB write timed out");
    return;
  }
  Serial.println("\nSCREEN_END");
}
void tuneCamera(char profile) {
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
  Serial.printf(
      "TUNING profile=%c errors=%d quality=%d sharpness=%d contrast=%d denoise=%d autoExposure=%d exposureLines=%d\n",
      profile, errors, sensor->status.quality, sensor->status.sharpness, sensor->status.contrast,
      sensor->status.denoise, sensor->status.aec, sensor->status.aec_value);
  for (int i = 0; i < 3; ++i) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame) esp_camera_fb_return(frame);
    delay(100);
  }
}
bool previewActive = false;
framesize_t stillSize = FRAMESIZE_QXGA;
int stillQuality = 8;
void stopPreview() {
  if (!previewActive) return;
  sensor_t *sensor = esp_camera_sensor_get();
  sensor->set_framesize(sensor, stillSize);
  sensor->set_quality(sensor, stillQuality);
  previewActive = false;
  for (int i = 0; i < 2; ++i) {
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
  }
}
void previewFrame(bool deviceScreen = false) {
  if (view != View::Home || !liveRequested) {
    stopPreview();
    Serial.println("CAPTURE FAILED Preview is not active");
    return;
  }
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    Serial.println("CAPTURE FAILED Camera unavailable");
    return;
  }
  if (!previewActive) {
    stillSize = sensor->status.framesize;
    stillQuality = sensor->status.quality;
    if (sensor->set_framesize(sensor, FRAMESIZE_QVGA) || sensor->set_quality(sensor, 20)) {
      sensor->set_framesize(sensor, stillSize);
      sensor->set_quality(sensor, stillQuality);
      Serial.println("CAPTURE FAILED Preview setup failed");
      return;
    }
    previewActive = true;
    for (int i = 0; i < 2; ++i) {
      camera_fb_t *f = esp_camera_fb_get();
      if (f) esp_camera_fb_return(f);
    }
  }
  camera_fb_t *f = esp_camera_fb_get();
  if (!f) {
    Serial.println("CAPTURE FAILED No preview frame");
    return;
  }
  if (deviceScreen) {
    size_t needed = f->width * f->height * 2;
    if (needed > rgbCapacity) {
      free(rgbScratch);
      rgbScratch = (uint8_t *)ps_malloc(needed);
      rgbCapacity = rgbScratch ? needed : 0;
    }
    if (!livePixels) livePixels = (uint16_t *)ps_malloc(144 * 108 * 2);
    bool decoded = rgbScratch && livePixels && jpg2rgb565(f->buf, f->len, rgbScratch, JPG_SCALE_NONE);
    if (decoded)
      for (int y = 0; y < 108; ++y)
        for (int x = 0; x < 144; ++x) {
          size_t offset = ((y * f->height / 108) * f->width + x * f->width / 144) * 2;
          livePixels[y * 144 + x] = rgbScratch[offset] | (uint16_t(rgbScratch[offset + 1]) << 8);
        }
    esp_camera_fb_return(f);
    if (!decoded) {
      Serial.println("SCREEN_ERROR Preview decode failed");
      return;
    }
    sendFrame();
    return;
  }
  Serial.printf("JPEG_BEGIN %u\n", f->len);
  bool sent = sendAcknowledged(f->buf, f->len);
  esp_camera_fb_return(f);
  Serial.println(sent ? "\nJPEG_END" : "\nCAPTURE FAILED Preview transfer failed");
}
void takePhoto() {
  stopPreview();
  jpegBytes = 0;
  if (!esp_camera_sensor_get()) {
    problem = "Camera is unavailable.";
    view = View::Error;
    return;
  }
  camera_fb_t *queued = esp_camera_fb_get();
  if (queued) esp_camera_fb_return(queued);
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    problem = "No camera frame.";
    view = View::Error;
    return;
  }
  if (frame->format != PIXFORMAT_JPEG || frame->len < 4 || frame->buf[0] != 0xff || frame->buf[1] != 0xd8 ||
      frame->buf[frame->len - 2] != 0xff || frame->buf[frame->len - 1] != 0xd9) {
    esp_camera_fb_return(frame);
    problem = "Incomplete photo. Try again.";
    view = View::Error;
    return;
  }
  int decodedWidth = frame->width / 4, decodedHeight = frame->height / 4;
  size_t needed = decodedWidth * decodedHeight * 2;
  if (needed > rgbCapacity) {
    free(rgbScratch);
    rgbScratch = (uint8_t *)ps_malloc(needed);
    rgbCapacity = rgbScratch ? needed : 0;
  }
  uint8_t *rgb = rgbScratch;
  Serial.printf("CAPTURE_FRAME %u %u %u\n", frame->width, frame->height, frame->len);
  if (!photo) photo = (uint16_t *)ps_malloc(216 * 162 * 2);
  bool decoded = rgb && photo && jpg2rgb565(frame->buf, frame->len, rgb, JPG_SCALE_4X);
  if (decoded) {
    // 180-degree rotation and scaling happen on the board.
    for (int y = 0; y < 162; ++y)
      for (int x = 0; x < 216; ++x) {
        int sourceY = y * decodedHeight / 162, sourceX = x * decodedWidth / 216;
        if (rotatePhoto) {
          sourceY = decodedHeight - 1 - sourceY;
          sourceX = decodedWidth - 1 - sourceX;
        }
        size_t source = (sourceY * decodedWidth + sourceX) * 2;
        photo[y * 216 + x] = rgb[source] | (uint16_t(rgb[source + 1]) << 8);
      }
    if (frame->len > jpegCapacity) {
      free(savedJpeg);
      savedJpeg = (uint8_t *)ps_malloc(frame->len);
      jpegCapacity = savedJpeg ? frame->len : 0;
    }
    if (savedJpeg) {
      memcpy(savedJpeg, frame->buf, frame->len);
      jpegBytes = frame->len;
    } else jpegBytes = 0;
    photoWidth = frame->width;
    photoHeight = frame->height;
    view = View::Photo;
  } else {
    problem = "Image decode failed.";
    view = View::Error;
  }
  esp_camera_fb_return(frame);
}
void solveSavedPhoto() {
  activeApp = AppId::AI;
  stopPreview();
  if (!jpegBytes) {
    problem = "Capture a photo first.";
    view = View::Error;
    return;
  }
  String key = settings.getString(useGemini ? "gemini-key" : "gpt-key", "");
  String answer;
  bool ok = requestImageAnswer(useGemini, key, savedJpeg, jpegBytes, answer);
  key = "";
  if (ok) {
    // Persist only successful real answers. Demo and failed attempts cannot replace them.
    lastAnswer = answer;
    lastAnswerGemini = useGemini;
    settings.putString("last-answer", lastAnswer);
    settings.putBool("answer-gemini", lastAnswerGemini);
    wrap(lastAnswer.c_str());
    view = View::Answer;
  } else {
    problem = answer;
    view = View::Error;
  }
}
}  // namespace
void initDeviceUi() {
  settings.begin("tiny-ai", false);
  useGemini = settings.getBool("gemini", true);
  lastAnswer = settings.getString("last-answer", "");
  lastAnswerGemini = settings.getBool("answer-gemini", true);
  screen.pixels = (uint16_t *)ps_malloc(W * H * 2);
  wrap(demo);
}
void handleDeviceButton(char command) {
  if (command == 'T') {
    String xy = Serial.readStringUntil('\n');
    int comma = xy.indexOf(',');
    if (comma < 1) {
      Serial.println("SCREEN_ERROR Invalid touch");
      return;
    }
    int x = xy.substring(0, comma).toInt(), y = xy.substring(comma + 1).toInt();
    if (x < 0 || x >= W || y < 0 || y >= H) {
      sendFrame();
      return;
    }
    char action = 'f';
    if (view != View::Desk && y >= 274) action = 'h';
    else if (view == View::Desk) {
      for (int i = 0; i < 5; ++i) {
        int cx = appX(i), cy = appY(i);
        if (x >= cx - 42 && x < cx + 42 && y >= cy - 28 && y < cy + 43) action = apps[i].command;
      }
    } else if (view == View::Home) {
      if (y >= 216 && y < 274 && x >= 84 && x <= 156) action = 'a';
      else if (jpegBytes && y >= 216 && y < 274 && x < 72) action = 'p';
      else if (y >= 32 && y < 212) action = 'L';
    } else if (view == View::AI) {
      if (y < 32 && x >= 190) action = 'Q';
      else if (y >= 220 && y < 260 && x >= 24 && x <= 216) action = jpegBytes ? 'q' : 'Q';
    } else if (view == View::History && !lastAnswer.isEmpty() && y >= 54 && y < 126) action = 'b';
    else if (view == View::Status && y >= 52 && y < 96) action = 'P';
    else if (view == View::Error) {
      if (y < 32 && x < 36) action = 'X';
      else if (activeApp == AppId::AI && jpegBytes && y >= 200 && y < 238) action = 'q';
    }
    handleDeviceButton(action);
    return;
  }
  if (command == 'K') {
    String input = Serial.readStringUntil('\n');
    if (input.length() < 1 || input.length() > 513 || (input[0] != 'G' && input[0] != 'O')) {
      Serial.println("SCREEN_ERROR Invalid key settings");
      return;
    }
    String key = input.substring(1);
    for (size_t i = 0; i < key.length(); ++i) {
      char c = key[i];
      if ((unsigned char)c < 33 || (unsigned char)c > 126) {
        Serial.println("SCREEN_ERROR Invalid key characters");
        return;
      }
    }
    const char *name = input[0] == 'G' ? "gemini-key" : "gpt-key";
    bool ok = key.isEmpty() ? (!settings.isKey(name) || settings.remove(name))
                            : settings.putString(name, key) == key.length();
    key = "";
    input = "";
    if (!ok) {
      Serial.println("SCREEN_ERROR Key could not be saved");
      return;
    }
    sendFrame();
    return;
  }
  if (command == 'N') {
    previewFrame(true);
    return;
  }
  if (command == 'n') {
    previewFrame();
    return;
  }
  if (command == 'e') {
    stopPreview();
    Serial.println("PREVIEW_STOPPED");
    return;
  }
  if (command >= '1' && command <= '5') stopPreview();
  switch (command) {
    case 'o':
      if (!savedJpeg || !jpegBytes) {
        Serial.println("CAPTURE FAILED Capture a photo first.");
        return;
      }
      Serial.printf("JPEG_BEGIN %u\n", jpegBytes);
      if (!sendAcknowledged(savedJpeg, jpegBytes)) {
        Serial.println("\nCAPTURE FAILED USB write timed out");
        return;
      }
      Serial.println("\nJPEG_END");
      return;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
      tuneCamera(command);
      takePhoto();
      break;
    case 'L':
      liveRequested = !liveRequested;
      if (!liveRequested) stopPreview();
      break;
    case 'G':
      useGemini = true;
      settings.putBool("gemini", true);
      view = View::Home;
      break;
    case 'O':
      useGemini = false;
      settings.putBool("gemini", false);
      view = View::Home;
      break;
    case 'P':
      useGemini = !useGemini;
      settings.putBool("gemini", useGemini);
      break;
    case 'C':
      activeApp = AppId::Camera;
      view = View::Home;
      liveRequested = true;
      break;
    case 'X':
      if (activeApp == AppId::Camera) view = View::Home;
      else if (activeApp == AppId::Photos) view = View::Photo;
      else if (activeApp == AppId::Settings) view = View::Status;
      else if (activeApp == AppId::AI) view = View::AI;
      else view = View::History;
      break;
    case 'q': solveSavedPhoto(); break;
    case 'A':
      activeApp = AppId::AI;
      view = View::AI;
      break;
    case 'H':
      activeApp = AppId::History;
      view = View::History;
      break;
    case 'b':
      activeApp = AppId::History;
      wrap(lastAnswer.isEmpty() ? "No saved answers." : lastAnswer.c_str());
      view = View::Answer;
      break;
    case 'a':
      activeApp = AppId::Camera;
      takePhoto();
      if (view == View::Photo) activeApp = AppId::Photos;
      break;
    case 'Q':
      activeApp = AppId::AI;
      takePhoto();
      if (view == View::Photo) solveSavedPhoto();
      break;
    case 't':
      if (view == View::Answer) {
        view = View::Home;
      } else {
        takePhoto();
      }
      break;
    case 'd':
      wrap(demo);
      view = View::Answer;
      break;
    case 'u':
      if (view == View::Answer && page > 0) --page;
      break;
    case 'v':
      if (view == View::Answer && page < pageCount() - 1) ++page;
      break;
    case 'h': view = View::Desk; break;
    case 'p':
      activeApp = AppId::Photos;
      view = View::Photo;
      break;
    case 'i':
      activeApp = AppId::Settings;
      view = View::Status;
      break;
    case 'r':
      rotatePhoto = !rotatePhoto;
      if (photo) {
        for (int i = 0; i < 216 * 162 / 2; ++i) {
          uint16_t value = photo[i];
          photo[i] = photo[216 * 162 - 1 - i];
          photo[216 * 162 - 1 - i] = value;
        }
        view = View::Photo;
      }
      break;
    case 'f': break;
    default: return;
  }
  if (view != View::Home) stopPreview();
  sendFrame();
}
