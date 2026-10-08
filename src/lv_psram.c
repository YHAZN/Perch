// LVGL's allocator (LV_STDLIB_CUSTOM): every widget, style and label lives in PSRAM.
// Internal RAM is kept for what needs it: Wi-Fi/lwIP buffers, Bluetooth, TLS and the camera
// and display DMA. Hundreds of UI objects in internal RAM left too little for Bluetooth plus
// the camera; sending all mallocs to PSRAM instead broke large Wi-Fi uploads (lwIP buffers).
#include <string.h>
#include <esp_heap_caps.h>
#include "lvgl.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  (void)mem;
  (void)bytes;
  return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, LV_CAPS);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);  // PSRAM full: anything
}
void *lv_realloc_core(void *p, size_t new_size) {
  void *q = heap_caps_realloc(p, new_size, LV_CAPS);
  return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}
void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  memset(mon_p, 0, sizeof(*mon_p));
  mon_p->total_size = heap_caps_get_total_size(LV_CAPS);
  mon_p->free_size = heap_caps_get_free_size(LV_CAPS);
  mon_p->free_biggest_size = heap_caps_get_largest_free_block(LV_CAPS);
}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
