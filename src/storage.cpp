#include "storage.h"
#include <LittleFS.h>

namespace {
bool mounted = false;
const char *ANSWER = "/answer.txt";
const char *ANSWER_TMP = "/answer.tmp";
}  // namespace

bool storageBegin() {
  // Formats only if the partition holds no LittleFS yet (it was unused before).
  mounted = LittleFS.begin(true);
  return mounted;
}

// File layout: provider line ("gemini" or "gpt"), then the answer text.
bool saveLastAnswer(const String &answer, bool gemini) {
  if (!mounted) return false;
  File file = LittleFS.open(ANSWER_TMP, "w");
  if (!file) return false;
  const String header = gemini ? "gemini\n" : "gpt\n";
  const bool written = file.print(header) == header.length() && file.print(answer) == answer.length();
  file.close();
  if (!written) {
    LittleFS.remove(ANSWER_TMP);
    return false;
  }
  // LittleFS rename replaces the old file atomically, so a power cut keeps one whole answer.
  return LittleFS.rename(ANSWER_TMP, ANSWER);
}

bool loadLastAnswer(String &answer, bool &gemini) {
  if (!mounted || !LittleFS.exists(ANSWER)) return false;
  File file = LittleFS.open(ANSWER, "r");
  if (!file) return false;
  gemini = file.readStringUntil('\n') != "gpt";
  answer = file.readString();
  file.close();
  return true;
}
