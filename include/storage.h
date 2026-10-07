#pragma once
#include <Arduino.h>

// Durable device storage on the internal flash filesystem (LittleFS, 1.5 MB "spiffs"
// partition) until the SD card takes over. Survives reboots and power loss.
bool storageBegin();

// Thumbnails are 64x64 little-endian RGB565, stored beside each photo and answer.
constexpr int THUMB = 64;

// Photo gallery. Oldest photos are removed when space runs low. Returns 0 on failure.
uint32_t savePhoto(const uint8_t *jpeg, size_t length, const uint16_t *thumb);
// Newest first. Returns the number of ids written.
int listPhotos(uint32_t *ids, int max);
// Allocates in PSRAM; the caller frees `jpeg`.
bool loadPhoto(uint32_t id, uint8_t *&jpeg, size_t &length);
bool loadPhotoThumb(uint32_t id, uint16_t *thumb);
// Screen-sized JPEG (240x284) kept beside each photo so Photos opens quickly.
bool savePhotoScreen(uint32_t id, const uint8_t *jpeg, size_t length);
bool loadPhotoScreen(uint32_t id, uint8_t *&jpeg, size_t &length);

// Answer history, newest first, capped at ANSWER_KEEP entries.
constexpr int ANSWER_KEEP = 20;
struct AnswerInfo {
  uint32_t id = 0;
  uint32_t photoId = 0;
  uint32_t when = 0;  // epoch seconds, 0 if the clock was unknown
  bool gemini = true;
};
uint32_t saveAnswer(const String &text, bool gemini, uint32_t photoId, uint32_t when, const uint16_t *thumb);
int listAnswers(uint32_t *ids, int max);
bool loadAnswer(uint32_t id, String &text, AnswerInfo &info);
bool loadAnswerThumb(uint32_t id, uint16_t *thumb);

size_t storageFreeBytes();
