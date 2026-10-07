#include "storage.h"
#include <LittleFS.h>
#include <algorithm>
#include <vector>

namespace {
bool mounted = false;
constexpr size_t THUMB_BYTES = THUMB * THUMB * 2;
// Leave room for answers and filesystem overhead when making space for a photo.
constexpr size_t PHOTO_RESERVE = 160 * 1024;

uint32_t nextId() {
  uint32_t id = 1;
  File f = LittleFS.open("/seq", "r");
  if (f) {
    id = f.parseInt() + 1;
    f.close();
  }
  f = LittleFS.open("/seq", "w");
  if (f) {
    f.print(id);
    f.close();
  }
  return id;
}

// Ids of files named "<id><suffix>" in a directory, newest (highest) first.
std::vector<uint32_t> ids(const char *dir, const char *suffix) {
  std::vector<uint32_t> out;
  File d = LittleFS.open(dir);
  if (!d) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String name = f.name();
    if (name.endsWith(suffix)) out.push_back(name.toInt());
  }
  std::sort(out.begin(), out.end(), [](uint32_t a, uint32_t b) { return a > b; });
  return out;
}

String path(const char *dir, uint32_t id, const char *suffix) { return String(dir) + "/" + id + suffix; }

bool writeFile(const String &name, const uint8_t *data, size_t length) {
  // Write-then-rename: a power cut never leaves a half-written file under the real name.
  const String temp = name + ".tmp";
  File f = LittleFS.open(temp, "w");
  if (!f) return false;
  const bool ok = f.write(data, length) == length;
  f.close();
  if (!ok) {
    LittleFS.remove(temp);
    return false;
  }
  return LittleFS.rename(temp, name);
}

bool readThumb(const String &name, uint16_t *thumb) {
  File f = LittleFS.open(name, "r");
  if (!f) return false;
  const bool ok = f.read(reinterpret_cast<uint8_t *>(thumb), THUMB_BYTES) == THUMB_BYTES;
  f.close();
  return ok;
}
}  // namespace

bool storageBegin() {
  // Formats only if the partition holds no LittleFS yet.
  mounted = LittleFS.begin(true);
  if (mounted) {
    LittleFS.mkdir("/photos");
    LittleFS.mkdir("/answers");
    // Move the single answer kept by the previous firmware into the history.
    File old = LittleFS.open("/answer.txt", "r");
    if (old) {
      const bool gemini = old.readStringUntil('\n') != "gpt";
      const String text = old.readString();
      old.close();
      if (text.isEmpty() || saveAnswer(text, gemini, 0, 0, nullptr)) LittleFS.remove("/answer.txt");
    }
  }
  return mounted;
}

size_t storageFreeBytes() { return mounted ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0; }

uint32_t savePhoto(const uint8_t *jpeg, size_t length, const uint16_t *thumb) {
  if (!mounted) return 0;
  // Make room by removing the oldest photos first.
  std::vector<uint32_t> existing = ids("/photos", ".jpg");
  while (storageFreeBytes() < length + THUMB_BYTES + PHOTO_RESERVE && !existing.empty()) {
    const uint32_t oldest = existing.back();
    existing.pop_back();
    LittleFS.remove(path("/photos", oldest, ".jpg"));
    LittleFS.remove(path("/photos", oldest, ".thm"));
    LittleFS.remove(path("/photos", oldest, ".scr"));
  }
  const uint32_t id = nextId();
  if (!writeFile(path("/photos", id, ".thm"), reinterpret_cast<const uint8_t *>(thumb), THUMB_BYTES)) return 0;
  if (!writeFile(path("/photos", id, ".jpg"), jpeg, length)) {
    LittleFS.remove(path("/photos", id, ".thm"));
    return 0;
  }
  return id;
}

int listPhotos(uint32_t *out, int max) {
  if (!mounted) return 0;
  const std::vector<uint32_t> all = ids("/photos", ".jpg");
  const int n = std::min<int>(max, all.size());
  for (int i = 0; i < n; ++i) out[i] = all[i];
  return n;
}

bool loadPhoto(uint32_t id, uint8_t *&jpeg, size_t &length) {
  jpeg = nullptr;
  length = 0;
  if (!mounted) return false;
  File f = LittleFS.open(path("/photos", id, ".jpg"), "r");
  if (!f) return false;
  length = f.size();
  jpeg = static_cast<uint8_t *>(ps_malloc(length));
  const bool ok = jpeg && f.read(jpeg, length) == length;
  f.close();
  if (!ok) {
    free(jpeg);
    jpeg = nullptr;
    length = 0;
  }
  return ok;
}

bool loadPhotoThumb(uint32_t id, uint16_t *thumb) { return mounted && readThumb(path("/photos", id, ".thm"), thumb); }

bool savePhotoScreen(uint32_t id, const uint8_t *jpeg, size_t length) {
  return mounted && writeFile(path("/photos", id, ".scr"), jpeg, length);
}

bool loadPhotoScreen(uint32_t id, uint8_t *&jpeg, size_t &length) {
  jpeg = nullptr;
  length = 0;
  if (!mounted) return false;
  File f = LittleFS.open(path("/photos", id, ".scr"), "r");
  if (!f) return false;
  length = f.size();
  jpeg = static_cast<uint8_t *>(ps_malloc(length));
  const bool ok = jpeg && f.read(jpeg, length) == length;
  f.close();
  if (!ok) {
    free(jpeg);
    jpeg = nullptr;
    length = 0;
  }
  return ok;
}

// Answer file: "gemini|gpt <photoId> <when>\n" then the answer text.
uint32_t saveAnswer(const String &text, bool gemini, uint32_t photoId, uint32_t when, const uint16_t *thumb) {
  if (!mounted) return 0;
  std::vector<uint32_t> existing = ids("/answers", ".txt");
  while ((int)existing.size() >= ANSWER_KEEP) {
    const uint32_t oldest = existing.back();
    existing.pop_back();
    LittleFS.remove(path("/answers", oldest, ".txt"));
    LittleFS.remove(path("/answers", oldest, ".thm"));
  }
  const uint32_t id = nextId();
  const String body = String(gemini ? "gemini " : "gpt ") + photoId + " " + when + "\n" + text;
  if (thumb) writeFile(path("/answers", id, ".thm"), reinterpret_cast<const uint8_t *>(thumb), THUMB_BYTES);
  if (!writeFile(path("/answers", id, ".txt"), reinterpret_cast<const uint8_t *>(body.c_str()), body.length()))
    return 0;
  return id;
}

int listAnswers(uint32_t *out, int max) {
  if (!mounted) return 0;
  const std::vector<uint32_t> all = ids("/answers", ".txt");
  const int n = std::min<int>(max, all.size());
  for (int i = 0; i < n; ++i) out[i] = all[i];
  return n;
}

bool loadAnswer(uint32_t id, String &text, AnswerInfo &info) {
  if (!mounted) return false;
  File f = LittleFS.open(path("/answers", id, ".txt"), "r");
  if (!f) return false;
  const String header = f.readStringUntil('\n');
  text = f.readString();
  f.close();
  info.id = id;
  info.gemini = !header.startsWith("gpt");
  const int a = header.indexOf(' '), b = header.indexOf(' ', a + 1);
  info.photoId = a > 0 ? header.substring(a + 1, b).toInt() : 0;
  info.when = b > 0 ? strtoul(header.substring(b + 1).c_str(), nullptr, 10) : 0;
  return true;
}

bool loadAnswerThumb(uint32_t id, uint16_t *thumb) { return mounted && readThumb(path("/answers", id, ".thm"), thumb); }
