#pragma once
#include <stddef.h>
#include <stdint.h>

// Built-in PDM microphone on the XIAO ESP32-S3 Sense (clock GPIO42, data GPIO41).
// Records 16 kHz mono 16-bit into PSRAM on its own task; at most MIC_MAX_SECONDS.
constexpr int MIC_RATE = 16000;
constexpr int MIC_MAX_SECONDS = 15;

bool micStart();
void micStop();
bool micRecording();
// Recent loudness 0..100 for the on-screen level ring.
int micLevel();
float micSeconds();
// The last recording as a complete WAV file (header + PCM) in PSRAM; nullptr if none.
// Valid until the next micStart().
const uint8_t *micWav(size_t &length);
