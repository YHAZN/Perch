#include "clock.h"
#include <Arduino.h>
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>
#include "build_time.h"

namespace {
// US Eastern time. Applied before any source sets the time.
const char *TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";
// Typical delay between the build stamp and the board booting after upload.
constexpr uint32_t BUILD_TO_BOOT_SECONDS = 45;
}  // namespace

void clockSet(uint32_t epoch) {
  timeval now = {static_cast<time_t>(epoch), 0};
  settimeofday(&now, nullptr);
}

bool clockKnown() { return time(nullptr) > 1700000000; }

void clockBegin() {
  setenv("TZ", TIMEZONE, 1);
  tzset();
  // The clock survives software restarts; only seed it after a real power-up.
  if (clockKnown()) return;
  // Use the build stamp only on the first boot of a new build: on later power-ups it
  // would be stale, and no clock is better than a confidently wrong one.
  Preferences store;
  store.begin("perch-clock", false);
  if (store.getULong("seed", 0) != PERCH_BUILD_EPOCH) {
    clockSet(PERCH_BUILD_EPOCH + BUILD_TO_BOOT_SECONDS);
    store.putULong("seed", PERCH_BUILD_EPOCH);
  }
  store.end();
}
