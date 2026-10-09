#pragma once
#include <Arduino.h>
#include <vector>

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
size_t photoBytes(uint32_t id);  // size of the full photo, 0 if missing
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

// Offline queue: questions asked without Wi-Fi, answered when it returns. Their photos are
// protected from the space clean-up until the answer arrives.
struct QueuedAsk {
  uint32_t id = 0;
  uint32_t photoId = 0;
  uint32_t when = 0;
  bool gemini = true;
  bool failed = false;  // gave up after an error; retried only when the user asks
};
uint32_t queueAdd(uint32_t photoId, bool gemini, uint32_t when);
int queueList(QueuedAsk *out, int max);  // oldest first
bool queueRemove(uint32_t id);
bool queueSetFailed(uint32_t id, bool failed);
// Optional spoken question kept with a queued item (WAV). Removed with the item.
bool queueSaveAudio(uint32_t id, const uint8_t *wav, size_t length);
bool queueLoadAudio(uint32_t id, uint8_t *&wav, size_t &length);

// Privacy: delete answers, photos and waiting questions written at or after `since`
// (file times; needs the clock to have been known when they were written).
int storageForgetSince(time_t since);
// Delete one photo (and its thumbnail and screen copy) or one answer. A photo still waiting
// in the offline queue is kept (returns false).
bool deletePhoto(uint32_t id);
bool deleteAnswer(uint32_t id);

size_t storageFreeBytes();

// Chats (Ask app conversations): one JSON file each, newest first, the last CHAT_KEEP kept.
constexpr int CHAT_KEEP = 12;
uint32_t chatNewId();
bool chatSave(uint32_t id, const String &json);
bool chatLoad(uint32_t id, String &json);
int chatList(uint32_t *ids, int max);
bool chatDelete(uint32_t id);

// Where a photo was uploaded for the AI (Gemini Files API keeps files 48 h).
bool photoUriSave(uint32_t id, const String &uri, uint32_t expires, uint32_t keyTag);
bool photoUriLoad(uint32_t id, String &uri, uint32_t &expires, uint32_t &keyTag);
void photoUriForget(uint32_t id);
// Upload links of photos deleted since the last call (to delete the provider's copies too).
std::vector<String> storageTakeForgottenUploads();
