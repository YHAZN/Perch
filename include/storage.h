#pragma once
#include <Arduino.h>

// Durable device storage on the internal flash filesystem (LittleFS, "spiffs" partition).
bool storageBegin();
bool saveLastAnswer(const String &answer, bool gemini);
bool loadLastAnswer(String &answer, bool &gemini);
