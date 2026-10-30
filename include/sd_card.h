#pragma once
#include <stdint.h>
#include <stddef.h>

// Probe/mount on the display's already initialized SPI bus, before display traffic.
// Supports FAT/exFAT without formatting. File operations are serialized across tasks.
void sdCardBegin();
bool sdCardReady();
bool sdCardAvailable();
uint64_t sdCardCapacity();
void sdCardReport();
bool sdCardSelfTest();
bool sdCardPhotoSelfTest();

// Full-resolution photos live in a device-specific directory. Existing files are
// never overwritten; thumbnails, indexes and chats remain in internal flash.
bool sdPhotoSave(uint32_t id, const uint8_t *data, size_t length, uint32_t &checksum);
bool sdPhotoLoad(uint32_t id, size_t length, uint32_t checksum, uint8_t *&data);
bool sdPhotoRemove(uint32_t id);
