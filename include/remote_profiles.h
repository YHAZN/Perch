#pragma once
#include <stdint.h>

enum class RemotePlatform : uint8_t { Windows, Mac, IPad, IPhone, Count };

struct RemoteShortcut {
  const char *label;
  uint8_t key, modifiers;
};

const char *remotePlatformName(RemotePlatform platform);
uint8_t remoteShortcutCount(RemotePlatform platform);
const RemoteShortcut *remoteShortcut(RemotePlatform platform, uint8_t index);
RemoteShortcut remotePresentationShortcut(RemotePlatform platform);
