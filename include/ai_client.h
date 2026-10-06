#pragma once
#include <Arduino.h>
bool requestImageAnswer(bool gemini, const String &key, const uint8_t *jpeg, size_t length, String &answer);
