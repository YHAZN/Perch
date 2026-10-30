#include "sd_card.h"
#include "board_pins.h"
#include "display.h"
#include <Arduino.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <FsLib/FsVolume.h>
#include <FsLib/FsFile.h>
#include <freertos/semphr.h>
#include <algorithm>
#include <string.h>

namespace {
sdmmc_card_t card = {};
bool ready = false;
esp_err_t lastError = ESP_ERR_INVALID_STATE;
const char *filesystem = "unknown";
uint32_t volumeStart = 0;
bool mounted = false;
SemaphoreHandle_t mutex = nullptr;
FsVolume volume;
char root[40] = {};
uint32_t entries = 0;
const char *lastPhotoOperation = "none";

class CardBlocks : public FsBlockDeviceInterface {
public:
  bool isBusy() override { return false; }
  bool syncDevice() override { return true; }  // IDF sector writes complete synchronously.
  Sector_t sectorCount() override { return card.csd.capacity; }
  bool readSector(Sector_t sector, uint8_t *dst) override { return readSectors(sector, dst, 1); }
  bool writeSector(Sector_t sector, const uint8_t *src) override { return writeSectors(sector, src, 1); }
  bool readSectors(Sector_t sector, uint8_t *dst, size_t count) override {
    if (!ready || !count || uint64_t(sector) + count > card.csd.capacity) return false;
    // Bound transfers below the shared display bus's DMA limit. JPEG buffers live
    // in PSRAM, so the IDF driver may also need an internal DMA bounce buffer.
    while (count) {
      const size_t batch = std::min<size_t>(count, 8);
      displayBusAcquire();
      lastError = sdmmc_read_sectors(&card, dst, sector, batch);
      displayBusRelease();
      if (lastError != ESP_OK) return false;
      sector += batch;
      dst += batch * 512;
      count -= batch;
    }
    return true;
  }
  bool writeSectors(Sector_t sector, const uint8_t *src, size_t count) override {
    if (!ready || !count || uint64_t(sector) + count > card.csd.capacity) return false;
    while (count) {
      const size_t batch = std::min<size_t>(count, 8);
      displayBusAcquire();
      lastError = sdmmc_write_sectors(&card, src, sector, batch);
      displayBusRelease();
      if (lastError != ESP_OK) return false;
      sector += batch;
      src += batch * 512;
      count -= batch;
    }
    return true;
  }
} blocks;

class Lock {
public:
  Lock() { if (mutex) xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { if (mutex) xSemaphoreGive(mutex); }
};

uint32_t hashBytes(const uint8_t *bytes, size_t length, uint32_t hash = 2166136261u) {
  while (length--) { hash ^= *bytes++; hash *= 16777619u; }
  return hash;
}

bool makeRoot() {
  if (volume.exists(root)) {
    FsFile dir;
    const bool ok = dir.open(&volume, root, O_RDONLY) && dir.isDir();
    dir.close();
    return ok;
  }
  return volume.mkdir(root);
}

String photoPath(uint32_t id) { return String(root) + "/" + id + ".jpg"; }

uint32_t le32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
const char *volumeType(const uint8_t *sector) {
  if (sector[510] != 0x55 || sector[511] != 0xaa) return "unknown";
  if (!memcmp(sector + 3, "EXFAT   ", 8)) return "exFAT";
  if (!memcmp(sector + 82, "FAT32   ", 8)) return "FAT32";
  if (!memcmp(sector + 54, "FAT16   ", 8)) return "FAT16";
  if (!memcmp(sector + 54, "FAT12   ", 8)) return "FAT12";
  return "unknown";
}
}

void sdCardBegin() {
  mutex = xSemaphoreCreateMutex();
  if (!mutex) { lastError = ESP_ERR_NO_MEM; return; }
  pinMode(PIN_LCD_CS, OUTPUT);
  digitalWrite(PIN_LCD_CS, HIGH);
  lastError = sdspi_host_init();
  if (lastError != ESP_OK) return;
  sdspi_device_config_t config = SDSPI_DEVICE_CONFIG_DEFAULT();
  config.host_id = SPI2_HOST;
  config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
  sdspi_dev_handle_t device = 0;
  lastError = sdspi_host_init_device(&config, &device);
  if (lastError != ESP_OK) return;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = device;
  host.max_freq_khz = 4000;
  host.command_timeout_ms = 1000;
  for (int attempt = 0; attempt < 3; ++attempt) {
    lastError = sdmmc_card_init(&host, &card);
    if (lastError == ESP_OK) break;
    delay(100);
  }
  if (lastError != ESP_OK) {
    sdspi_host_remove_device(device);
    return;
  }
  uint8_t *sector = static_cast<uint8_t *>(heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!sector) { lastError = ESP_ERR_NO_MEM; return; }
  lastError = sdmmc_read_sectors(&card, sector, 0, 1);
  if (lastError == ESP_OK) {
    filesystem = volumeType(sector);
    if (!strcmp(filesystem, "unknown") && sector[510] == 0x55 && sector[511] == 0xaa) {
      // Select a bounded MBR partition; never interpret arbitrary data as an address.
      const uint64_t capacity = card.csd.capacity;
      for (int i = 0; i < 4; ++i) {
        const uint8_t *entry = sector + 446 + i * 16;
        const uint32_t start = le32(entry + 8), count = le32(entry + 12);
        if (entry[4] && start && count && uint64_t(start) + count <= capacity) {
          volumeStart = start;
          break;
        }
      }
      if (volumeStart) {
        lastError = sdmmc_read_sectors(&card, sector, volumeStart, 1);
        if (lastError == ESP_OK) filesystem = volumeType(sector);
      }
    }
    ready = lastError == ESP_OK;
  }
  free(sector);
  if (!ready) return;
  // Mount the identified volume without changing its format or existing contents.
  mounted = volume.begin(&blocks, false, 0, volumeStart);
  if (!mounted) return;
  snprintf(root, sizeof(root), "/perch-%012llx", (unsigned long long)ESP.getEfuseMac());
  FsFile dir, entry;
  if (dir.openRoot(&volume)) {
    while (entry.openNext(&dir, O_RDONLY)) { ++entries; entry.close(); }
  }
}

bool sdCardReady() { return ready; }
bool sdCardAvailable() { return ready && mounted; }
uint64_t sdCardCapacity() { return ready ? uint64_t(card.csd.capacity) * card.csd.sector_size : 0; }

void sdCardReport() {
  Lock lock;
  if (!ready) {
    Serial.printf("SD_CARD ready=0 error=%s\n", esp_err_to_name(lastError));
    return;
  }
  const uint64_t bytes = uint64_t(card.csd.capacity) * card.csd.sector_size;
  Serial.printf("SD_CARD ready=1 capacity_bytes=%llu filesystem=%s volume_sector=%lu mounted=%d root_entries=%lu photo_operation=%s\n",
                (unsigned long long)bytes, filesystem, (unsigned long)volumeStart, mounted, (unsigned long)entries, lastPhotoOperation);
}

bool sdCardSelfTest() {
  Lock lock;
  if (!sdCardAvailable() || !makeRoot()) return false;
  const String name = String(root) + "/check-" + String(esp_random(), HEX) + ".bin";
  FsFile file;
  if (!file.open(&volume, name.c_str(), O_WRONLY | O_CREAT | O_EXCL)) return false;
  constexpr size_t length = 98317;
  uint8_t *payload = static_cast<uint8_t *>(ps_malloc(length));
  if (!payload) { file.close(); volume.remove(name.c_str()); return false; }
  for (size_t i = 0; i < length; ++i) payload[i] = uint8_t(i * 37 + 19);
  bool ok = file.write(payload, length) == length;
  free(payload);
  uint8_t buffer[512];
  ok = file.close() && ok;
  ok = ok && file.open(&volume, name.c_str(), O_RDONLY) && file.fileSize() == length;
  for (size_t offset = 0; offset < length && ok;) {
    const size_t n = std::min(sizeof(buffer), length - offset);
    ok = file.read(buffer, n) == (int)n;
    for (size_t i = 0; i < n && ok; ++i) ok = buffer[i] == uint8_t((offset + i) * 37 + 19);
    offset += n;
  }
  file.close();
  const bool removed = volume.remove(name.c_str());  // Only the file created above.
  Serial.printf("SD_TEST bytes=%u readback=%s cleanup=%s\n", unsigned(length), ok ? "PASS" : "FAIL",
                removed ? "PASS" : "FAIL");
  return ok && removed;
}

bool sdPhotoSave(uint32_t id, const uint8_t *data, size_t length, uint32_t &checksum) {
  Lock lock;
  auto fail = [](const char *stage) {
    lastPhotoOperation = stage;
    Serial.printf("SD_WRITE failed stage=%s error=%s\n", stage, esp_err_to_name(lastError));
    return false;
  };
  if (!id || !data || !length || !sdCardAvailable() || !makeRoot()) return fail("root");
  const String name = photoPath(id), temp = String(root) + "/" + id + ".tmp";
  if (volume.exists(name.c_str()) || volume.exists(temp.c_str())) return fail("exists");
  FsFile file;
  if (!file.open(&volume, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL)) {
    Serial.printf("SD_WRITE path=%s heap=%u\n", temp.c_str(), ESP.getFreeHeap());
    return fail("open");
  }
  bool ok = file.write(data, length) == length;
  ok = file.close() && ok;
  if (!ok || !volume.rename(temp.c_str(), name.c_str())) {
    fail(ok ? "rename" : "write");
    volume.remove(temp.c_str());
    return false;
  }
  lastPhotoOperation = "saved";
  checksum = hashBytes(data, length);
  return true;
}

bool sdPhotoLoad(uint32_t id, size_t length, uint32_t checksum, uint8_t *&data) {
  data = nullptr;
  Lock lock;
  if (!id || !length || length > 2 * 1024 * 1024 || !sdCardAvailable()) return false;
  FsFile file;
  if (!file.open(&volume, photoPath(id).c_str(), O_RDONLY) || file.fileSize() != length) return false;
  uint8_t *buffer = static_cast<uint8_t *>(ps_malloc(length));
  if (!buffer) return false;
  bool ok = file.read(buffer, length) == (int)length;
  file.close();
  ok = ok && hashBytes(buffer, length) == checksum;
  if (!ok) { free(buffer); return false; }
  data = buffer;
  return true;
}

namespace {
String assetPath(uint32_t id, SdPhotoAsset asset, const char *extension = ".bin") {
  return String(root) + "/" + id + (asset == SdPhotoAsset::Thumbnail ? "-thumb" : "-screen") + extension;
}
struct AssetHeader { uint32_t magic, length, checksum; };
constexpr uint32_t ASSET_MAGIC = 0x31534150;  // PAS1
constexpr size_t ASSET_LIMIT = 128 * 1024;
}

bool sdPhotoAssetSave(uint32_t id, SdPhotoAsset asset, const uint8_t *data, size_t length) {
  Lock lock;
  if (!id || !data || !length || length > ASSET_LIMIT || !sdCardAvailable() || !makeRoot()) return false;
  const String name = assetPath(id, asset), temp = assetPath(id, asset, ".tmp");
  if (volume.exists(name.c_str()) || volume.exists(temp.c_str())) return false;
  FsFile file;
  if (!file.open(&volume, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL)) return false;
  const AssetHeader header = {ASSET_MAGIC, uint32_t(length), hashBytes(data, length)};
  bool ok = file.write(&header, sizeof(header)) == sizeof(header) && file.write(data, length) == length;
  ok = file.close() && ok;
  if (ok && volume.rename(temp.c_str(), name.c_str())) return true;
  volume.remove(temp.c_str());
  return false;
}

bool sdPhotoAssetLoad(uint32_t id, SdPhotoAsset asset, uint8_t *&data, size_t &length) {
  data = nullptr;
  length = 0;
  Lock lock;
  if (!id || !sdCardAvailable()) return false;
  FsFile file;
  if (!file.open(&volume, assetPath(id, asset).c_str(), O_RDONLY)) return false;
  AssetHeader header = {};
  if (file.read(&header, sizeof(header)) != sizeof(header) || header.magic != ASSET_MAGIC ||
      !header.length || header.length > ASSET_LIMIT || file.fileSize() != sizeof(header) + header.length) {
    file.close();
    return false;
  }
  uint8_t *buffer = static_cast<uint8_t *>(ps_malloc(header.length));
  const bool read = buffer && file.read(buffer, header.length) == int(header.length);
  file.close();
  if (!read || hashBytes(buffer, header.length) != header.checksum) { free(buffer); return false; }
  data = buffer;
  length = header.length;
  return true;
}

bool sdPhotoRemove(uint32_t id) {
  Lock lock;
  if (!id || !sdCardAvailable()) return false;
  for (SdPhotoAsset asset : {SdPhotoAsset::Thumbnail, SdPhotoAsset::Screen}) {
    const String name = assetPath(id, asset);
    if (volume.exists(name.c_str()) && !volume.remove(name.c_str())) return false;
  }
  const String name = photoPath(id);
  return !volume.exists(name.c_str()) || volume.remove(name.c_str());
}

bool sdCardPhotoSelfTest() {
  constexpr size_t length = 98317;
  uint8_t *payload = static_cast<uint8_t *>(ps_malloc(length));
  if (!payload) return false;
  for (size_t i = 0; i < length; ++i) payload[i] = uint8_t(i * 17 + 23);
  const uint32_t id = esp_random() | 0x80000000u;
  uint32_t checksum = 0;
  bool ok = sdPhotoSave(id, payload, length, checksum);
  free(payload);
  if (!ok) return false;
  uint8_t *loaded = nullptr;
  ok = sdPhotoLoad(id, length, checksum, loaded);
  free(loaded);
  const bool removed = sdPhotoRemove(id);
  Serial.printf("SD_TEST photo_readback=%s cleanup=%s\n", ok ? "PASS" : "FAIL", removed ? "PASS" : "FAIL");
  return ok && removed;
}
