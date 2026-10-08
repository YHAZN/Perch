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
    LittleFS.mkdir("/queue");
    LittleFS.mkdir("/chats");
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
  // Make room by removing the oldest photos first, except ones still waiting in the queue.
  std::vector<uint32_t> existing = ids("/photos", ".jpg");
  QueuedAsk queued[16];
  const int queuedCount = queueList(queued, 16);
  auto isQueued = [&](uint32_t id) {
    for (int i = 0; i < queuedCount; ++i)
      if (queued[i].photoId == id) return true;
    return false;
  };
  while (storageFreeBytes() < length + THUMB_BYTES + PHOTO_RESERVE && !existing.empty()) {
    const uint32_t oldest = existing.back();
    existing.pop_back();
    if (isQueued(oldest)) continue;
    LittleFS.remove(path("/photos", oldest, ".jpg"));
    LittleFS.remove(path("/photos", oldest, ".thm"));
    LittleFS.remove(path("/photos", oldest, ".scr"));
    LittleFS.remove(path("/photos", oldest, ".gfu"));
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

// Where a photo was uploaded for the AI (Gemini Files API), so a chat does not upload it again:
// "/photos/<id>.gfu" = uri, then "<expires> <keyTag>" (keyTag tells keys apart; not the key).
bool photoUriSave(uint32_t id, const String &uri, uint32_t expires, uint32_t keyTag) {
  if (!mounted) return false;
  const String body = uri + "\n" + expires + " " + keyTag;
  return writeFile(path("/photos", id, ".gfu"), reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
}
bool photoUriLoad(uint32_t id, String &uri, uint32_t &expires, uint32_t &keyTag) {
  if (!mounted) return false;
  File f = LittleFS.open(path("/photos", id, ".gfu"), "r");
  if (!f) return false;
  uri = f.readStringUntil('\n');
  expires = f.parseInt();
  keyTag = (uint32_t)strtoul(f.readString().c_str(), nullptr, 10);
  f.close();
  uri.trim();
  return uri.length() > 0;
}
void photoUriForget(uint32_t id) {
  if (mounted) LittleFS.remove(path("/photos", id, ".gfu"));
}

// Upload links of photos deleted here; the UI asks the provider to delete those copies too.
std::vector<String> forgottenUploads;
void noteUpload(const String &file) {
  File f = LittleFS.open(file, "r");
  if (!f) return;
  String uri = f.readStringUntil('\n');
  f.close();
  uri.trim();
  if (uri.startsWith("https://")) forgottenUploads.push_back(uri);
}
std::vector<String> storageTakeForgottenUploads() {
  std::vector<String> out;
  out.swap(forgottenUploads);
  return out;
}

// Chats: "/chats/<id>.json", written whole each time it changes (they are small text).
uint32_t chatNewId() { return mounted ? nextId() : 0; }
bool chatSave(uint32_t id, const String &json) {
  if (!mounted || !id) return false;
  std::vector<uint32_t> existing = ids("/chats", ".json");
  while ((int)existing.size() >= CHAT_KEEP && std::find(existing.begin(), existing.end(), id) == existing.end()) {
    LittleFS.remove(path("/chats", existing.back(), ".json"));
    existing.pop_back();
  }
  return writeFile(path("/chats", id, ".json"), reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
}
bool chatLoad(uint32_t id, String &json) {
  if (!mounted) return false;
  File f = LittleFS.open(path("/chats", id, ".json"), "r");
  if (!f) return false;
  json = f.readString();
  f.close();
  return true;
}
int chatList(uint32_t *out, int max) {
  if (!mounted) return 0;
  const std::vector<uint32_t> all = ids("/chats", ".json");
  const int n = std::min<int>(max, all.size());
  for (int i = 0; i < n; ++i) out[i] = all[i];
  return n;
}
bool chatDelete(uint32_t id) { return mounted && LittleFS.remove(path("/chats", id, ".json")); }

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

// Queue file: "gemini|gpt <photoId> <when> <failed>".
uint32_t queueAdd(uint32_t photoId, bool gemini, uint32_t when) {
  if (!mounted) return 0;
  const uint32_t id = nextId();
  const String body = String(gemini ? "gemini " : "gpt ") + photoId + " " + when + " 0";
  return writeFile(path("/queue", id, ".q"), reinterpret_cast<const uint8_t *>(body.c_str()), body.length()) ? id : 0;
}

int queueList(QueuedAsk *out, int max) {
  if (!mounted) return 0;
  std::vector<uint32_t> all = ids("/queue", ".q");
  std::reverse(all.begin(), all.end());  // oldest first
  int n = 0;
  for (uint32_t id : all) {
    if (n >= max) break;
    File f = LittleFS.open(path("/queue", id, ".q"), "r");
    if (!f) continue;
    const String line = f.readString();
    f.close();
    QueuedAsk q;
    q.id = id;
    q.gemini = !line.startsWith("gpt");
    int a = line.indexOf(' '), b = line.indexOf(' ', a + 1), c = line.indexOf(' ', b + 1);
    q.photoId = line.substring(a + 1, b).toInt();
    q.when = strtoul(line.substring(b + 1, c).c_str(), nullptr, 10);
    q.failed = c > 0 && line.substring(c + 1).toInt() != 0;
    out[n++] = q;
  }
  return n;
}

bool queueRemove(uint32_t id) {
  if (!mounted) return false;
  LittleFS.remove(path("/queue", id, ".wav"));
  return LittleFS.remove(path("/queue", id, ".q"));
}

bool queueSaveAudio(uint32_t id, const uint8_t *wav, size_t length) {
  return mounted && writeFile(path("/queue", id, ".wav"), wav, length);
}

bool queueLoadAudio(uint32_t id, uint8_t *&wav, size_t &length) {
  wav = nullptr;
  length = 0;
  if (!mounted) return false;
  File f = LittleFS.open(path("/queue", id, ".wav"), "r");
  if (!f) return false;
  length = f.size();
  wav = static_cast<uint8_t *>(ps_malloc(length));
  const bool ok = wav && f.read(wav, length) == length;
  f.close();
  if (!ok) {
    free(wav);
    wav = nullptr;
    length = 0;
  }
  return ok;
}

bool queueSetFailed(uint32_t id, bool failed) {
  QueuedAsk all[16];
  const int n = queueList(all, 16);
  for (int i = 0; i < n; ++i) {
    if (all[i].id != id) continue;
    const String body =
        String(all[i].gemini ? "gemini " : "gpt ") + all[i].photoId + " " + all[i].when + (failed ? " 1" : " 0");
    return writeFile(path("/queue", id, ".q"), reinterpret_cast<const uint8_t *>(body.c_str()), body.length());
  }
  return false;
}

int storageForgetSince(time_t since) {
  if (!mounted) return 0;
  int removed = 0;
  const char *dirs[] = {"/answers", "/photos", "/queue"};
  for (const char *dir : dirs) {
    std::vector<String> doomed;
    File d = LittleFS.open(dir);
    if (!d) continue;
    for (File f = d.openNextFile(); f; f = d.openNextFile())
      if (f.getLastWrite() >= since) doomed.push_back(String(dir) + "/" + f.name());
    d.close();
    for (const String &path : doomed) {
      if (path.endsWith(".gfu")) noteUpload(path);
      removed += LittleFS.remove(path);
    }
  }
  return removed;
}

bool deletePhoto(uint32_t id) {
  if (!mounted) return false;
  QueuedAsk queued[16];
  const int n = queueList(queued, 16);
  for (int i = 0; i < n; ++i)
    if (queued[i].photoId == id) return false;
  LittleFS.remove(path("/photos", id, ".thm"));
  LittleFS.remove(path("/photos", id, ".scr"));
  noteUpload(path("/photos", id, ".gfu"));
  LittleFS.remove(path("/photos", id, ".gfu"));
  return LittleFS.remove(path("/photos", id, ".jpg"));
}

bool deleteAnswer(uint32_t id) {
  if (!mounted) return false;
  LittleFS.remove(path("/answers", id, ".thm"));
  return LittleFS.remove(path("/answers", id, ".txt"));
}
