// Part of device_ui.cpp (included there, one translation unit). JPEG sizes, decoding to the screen, thumbnails.
// ---------- images ----------
bool jpegSize(const uint8_t *b, size_t n, int &w, int &h) {
  for (size_t i = 2; i + 9 < n;) {
    if (b[i] != 0xFF) {
      ++i;
      continue;
    }
    const uint8_t m = b[i + 1];
    if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
      h = (b[i + 5] << 8) | b[i + 6];
      w = (b[i + 7] << 8) | b[i + 8];
      return true;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;
      continue;
    }
    i += 2 + ((b[i + 2] << 8) | b[i + 3]);
  }
  return false;
}
// Decode a JPEG and fill the portrait screen: scale to the screen height, centre-crop the width.
bool decodeToScreen(const uint8_t *jpeg, size_t len, int width, int height, uint16_t *out, jpg_scale_t scale,
                    uint8_t *&scratch, size_t &capacity) {
  const int div = scale == JPG_SCALE_8X ? 8 : scale == JPG_SCALE_4X ? 4 : scale == JPG_SCALE_2X ? 2 : 1;
  const int dw = width / div, dh = height / div;
  const size_t needed = dw * dh * 2;
  if (needed > capacity) {
    free(scratch);
    scratch = (uint8_t *)ps_malloc(needed);
    capacity = scratch ? needed : 0;
  }
  uint8_t *rgbScratch = scratch;
  if (!rgbScratch || !out || !jpg2rgb565(jpeg, len, rgbScratch, scale)) return false;
  const int cropW = min(dw, dh * W / H), left = (dw - cropW) / 2;
  for (int y = 0; y < H; ++y) {
    const int sy = y * dh / H;
    for (int x = 0; x < W; ++x) {
      const size_t o = (sy * dw + left + x * cropW / W) * 2;
      out[y * W + x] = rgbScratch[o] | (uint16_t(rgbScratch[o + 1]) << 8);
    }
  }
  return true;
}
// Smallest decode that still covers the screen height (decoding at 1/4 or 1/8 is much faster).
jpg_scale_t scaleFor(int h) {
  return h / 8 >= H ? JPG_SCALE_8X : h / 4 >= H ? JPG_SCALE_4X : h / 2 >= H ? JPG_SCALE_2X : JPG_SCALE_NONE;
}
bool decodeStoredWith(const uint8_t *jpeg, size_t len, uint16_t *out, uint8_t *&scratch, size_t &capacity) {
  int w, h;
  return jpegSize(jpeg, len, w, h) && decodeToScreen(jpeg, len, w, h, out, scaleFor(h), scratch, capacity);
}
bool decodeStored(const uint8_t *jpeg, size_t len, uint16_t *out) {
  return decodeStoredWith(jpeg, len, out, rgbScratch, rgbCapacity);
}
// 64x64 thumbnail from the centre square of a 240x284 screen image.
void makeThumb(const uint16_t *src, uint16_t *out) {
  const int top = (H - W) / 2;
  for (int y = 0; y < THUMB; ++y)
    for (int x = 0; x < THUMB; ++x) out[y * THUMB + x] = src[(top + y * W / THUMB) * W + x * W / THUMB];
}
bool loadPhotoInto(uint32_t id, uint16_t *out) {
  if (id == latestPhotoId && jpegBytes) {
    memcpy(out, photoPixels, W * H * 2);
    return true;
  }
  uint8_t *jpeg;
  size_t len;
  // The small screen-sized copy decodes in ~0.1 s; the full photo is only a fallback.
  if (!loadPhotoScreen(id, jpeg, len) && !loadPhoto(id, jpeg, len)) return false;
  const bool ok = decodeStored(jpeg, len, out);
  free(jpeg);
  return ok;
}
