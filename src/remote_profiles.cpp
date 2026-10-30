#include "remote_profiles.h"
#include "remote.h"

namespace {
using namespace RemoteKey;
using namespace RemoteModifier;
constexpr RemoteShortcut windows[] = {{"Switch app", Tab, Alt}, {"Desktop", D, Windows}, {"Tasks", Tab, Windows},
                                      {"Search", S, Windows},   {"Copy", C, Control},    {"Paste", V, Control}};
constexpr RemoteShortcut mac[] = {{"Switch app", Tab, Command}, {"Search", Space, Command}, {"Overview", Up, Control},
                                  {"Select all", A, Command},   {"Copy", C, Command},       {"Paste", V, Command}};
constexpr RemoteShortcut ipad[] = {{"Switch app", Tab, Command}, {"Search", Space, Command}, {"Select all", A, Command},
                                   {"Undo", Z, Command},         {"Copy", C, Command},       {"Paste", V, Command}};
// Navigation requires Full Keyboard Access enabled on the phone.
constexpr RemoteShortcut iphone[] = {{"Next", Tab, 0},   {"Previous", Tab, Shift}, {"Activate", Space, 0},
                                     {"Enter", 0x28, 0}, {"Copy", C, Command},     {"Paste", V, Command}};
}  // namespace

const char *remotePlatformName(RemotePlatform platform) {
  switch (platform) {
    case RemotePlatform::Mac: return "Mac";
    case RemotePlatform::IPad: return "iPad";
    case RemotePlatform::IPhone: return "iPhone";
    default: return "Windows";
  }
}

uint8_t remoteShortcutCount(RemotePlatform platform) { return 6; }

const RemoteShortcut *remoteShortcut(RemotePlatform platform, uint8_t index) {
  if (index >= remoteShortcutCount(platform)) return nullptr;
  switch (platform) {
    case RemotePlatform::Mac: return &mac[index];
    case RemotePlatform::IPad: return &ipad[index];
    case RemotePlatform::IPhone: return &iphone[index];
    default: return &windows[index];
  }
}

RemoteShortcut remotePresentationShortcut(RemotePlatform platform) {
  if (platform == RemotePlatform::Windows) return {"Present", F5, 0};
  if (platform == RemotePlatform::Mac) return {"Keynote", P, uint8_t(Alt | Command)};
  // Mobile presentation start commands depend on the app. Do not invent a global one.
  return {nullptr, 0, 0};
}
