#pragma once
#include <stdint.h>

// Wall clock without a clock battery. Sources, best first: Wi-Fi time (SNTP),
// the PC over USB ('Z' command), then the time this firmware was built.
void clockBegin();
void clockSet(uint32_t epoch);
bool clockKnown();
