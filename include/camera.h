#pragma once
#include <stdint.h>

// The OV3660 runs in one of two driver configurations. Switching re-initialises the
// camera driver (a few hundred ms), so the UI switches only when it must.
enum class CameraMode {
  Off,      // driver stopped: no power or PSRAM bandwidth used
  Still,    // 2048x1536 JPEG, one frame on demand: photos for Photos and Ask
  Preview,  // 320x240 raw RGB565, continuous: the viewfinder, no JPEG decode needed
};

bool cameraSetMode(CameraMode mode);
CameraMode cameraMode();
// Raw preview frames are 320x240 big-endian RGB565.
constexpr int PREVIEW_W = 320, PREVIEW_H = 240;
// OV5640 only: focus once on the scene (blocks up to timeoutMs). False if focus did not lock
// (or the sensor has no autofocus); a photo can still be taken.
bool cameraFocus(uint32_t timeoutMs);
// Developer: print the sensor's current exposure and gain.
void cameraReport();
// Gain ceiling used for photos (gainceiling_t value, 0 = 2x ... 6 = 128x).
void cameraSetStillGain(int ceiling);
// Developer: photo brightness target (-2..2) and longest exposure in frame times (1..4).
void cameraSetStillExposure(int aeLevel, int maxFrames);
// Developer: photo tuning, keys dn (denoise), sh (sharpness), hts (line length), set (settle frames), sat.
bool cameraTune(const char *key, int value);
