#pragma once
#include <Arduino.h>

#include "network.h"

// One AI request at a time, on its own FreeRTOS task, so the UI never freezes.
enum class AiState { Idle, Working, Done, Failed, Cancelled };
// Copies the photo and key. Returns false if a previous request is still finishing.
// `wav` (optional) is the spoken question; only Gemini receives it.
bool aiStart(bool gemini, const String &key, const uint8_t *jpeg, size_t length, const uint8_t *wav = nullptr,
             size_t wavLength = 0);
// Stop waiting for the current request. Its result is discarded when it finishes.
// The upload may already have reached the provider; it is never retried automatically.
void aiCancel();
// Non-blocking. For Done/Failed, `text` receives the answer or the error message and
// the state returns to Idle. Cancelled results are consumed silently.
AiState aiPoll(String &text);
bool aiBusy();
// Call once at boot, before any HTTPS: lets TLS use PSRAM.
void aiBegin();

// Developer: unauthenticated upload of `bytes` to the Gemini endpoint (nothing billed).
void aiUploadTest(size_t bytes);
// Developer: free check that the key is valid and can see the model.
void aiKeyCheck(const String &key);
// The UI reports when flash is busy (photo still saving); requests wait for it.
void aiSetFlashBusy(bool (*busy)());
