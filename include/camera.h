#pragma once

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
