#pragma once
#include <Arduino.h>

#include "network.h"

// One AI request at a time, on its own FreeRTOS task, so the UI never freezes.
enum class AiState { Idle, Working, Done, Failed, Cancelled };

// Models offered in Settings. Names checked against the providers' docs (October 2026):
// ai.google.dev/gemini-api/docs/generate-content/thinking, developers.openai.com/api/docs/models
struct AiModel {
  const char *id;     // sent to the API
  const char *label;  // shown on the device
  const char *note;   // one short line under the label
};
extern const AiModel GEMINI_MODELS[];
extern const int GEMINI_MODEL_COUNT;
extern const AiModel GPT_MODELS[];
extern const int GPT_MODEL_COUNT;
// Effort / thinking level, both providers: "low", "medium", "high".
extern const char *const AI_EFFORTS[];
constexpr int AI_EFFORT_COUNT = 3;

struct AiOptions {
  bool gemini = true;
  String key;
  String model;           // AiModel::id
  String effort = "low";  // AI_EFFORTS
};

constexpr int AI_MAX_PAGES = 3;
// Ask about up to AI_MAX_PAGES photos (oldest first: earlier pages are context for the last).
// `question` is an optional typed or transcribed question. `wav` (optional, Gemini only) is a
// spoken question kept from offline use. Everything is copied. False if a request is running.
bool aiStart(const AiOptions &options, const uint8_t *const *jpegs, const size_t *lengths, int pages,
             const String &question, const uint8_t *wav = nullptr, size_t wavLength = 0);
// Speech to text for the hold-to-talk button: OpenAI gpt-transcribe for GPT, Gemini otherwise.
// The words arrive through aiPoll like an answer.
bool aiTranscribe(const AiOptions &options, const uint8_t *wav, size_t wavLength);
// Stop waiting for the current request. Its result is discarded when it finishes.
void aiCancel();
// Non-blocking. For Done/Failed, `text` receives the answer or the error message and
// the state returns to Idle. Cancelled results are consumed silently.
AiState aiPoll(String &text);
bool aiBusy();
// Call once at boot, before any HTTPS: lets TLS use PSRAM.
void aiBegin();

// Developer: build the exact request for `options` (tiny stand-in photo) and validate its JSON,
// printing the shape with image data elided. Nothing is sent.
void aiDryRun(const AiOptions &options, int pages, const String &question, bool transcribe);
// Developer: unauthenticated upload of `bytes` to the Gemini endpoint (nothing billed).
void aiUploadTest(size_t bytes);
// Developer: free check that the key is valid and can see the model.
void aiKeyCheck(const String &key);
// The UI reports when flash is busy (photo still saving); requests wait for it.
void aiSetFlashBusy(bool (*busy)());
