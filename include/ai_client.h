#pragma once
#include <Arduino.h>

// Wi-Fi runs in the background from boot and reconnects by itself.
void networkBegin();
void networkTick();
bool networkConnected();

// One AI request at a time, on its own FreeRTOS task, so the UI never freezes.
enum class AiState { Idle, Working, Done, Failed, Cancelled };
// Copies the photo and key. Returns false if a previous request is still finishing.
bool aiStart(bool gemini, const String &key, const uint8_t *jpeg, size_t length);
// Stop waiting for the current request. Its result is discarded when it finishes.
// The upload may already have reached the provider; it is never retried automatically.
void aiCancel();
// Non-blocking. For Done/Failed, `text` receives the answer or the error message and
// the state returns to Idle. Cancelled results are consumed silently.
AiState aiPoll(String &text);
bool aiBusy();
