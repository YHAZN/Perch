// Compile-only protocol checks. No Arduino runtime or Bluetooth hardware required.
// Include the implementation so its constant tables are checked, not a duplicate model.
#include "../src/remote_profiles.cpp"

static_assert(sizeof(windows) / sizeof(windows[0]) == 6, "Windows layout must fill its six tiles");
static_assert(sizeof(mac) / sizeof(mac[0]) == 6 && sizeof(ipad) / sizeof(ipad[0]) == 6 &&
                  sizeof(iphone) / sizeof(iphone[0]) == 6,
              "Every displayed Apple shortcut tile needs a command");
static_assert(windows[3].key == 0x16 && windows[3].modifiers == 0x08,
              "Windows Search is Win-S; Win-Space switches keyboard layouts");
static_assert(mac[0].key == 0x2B && mac[0].modifiers == 0x08 &&
                  ipad[0].key == 0x2B && ipad[0].modifiers == 0x08,
              "Mac and iPad app switching uses Command-Tab, not Alt-Tab");
static_assert(mac[4].modifiers == 0x08 && ipad[4].modifiers == 0x08 && iphone[4].modifiers == 0x08 &&
                  windows[4].modifiers == 0x01,
              "Apple Copy must use Command; Windows Copy must use Control");
static_assert(iphone[0].key == 0x2B && iphone[0].modifiers == 0 &&
                  iphone[1].key == 0x2B && iphone[1].modifiers == 0x02 &&
                  iphone[2].key == 0x2C && iphone[2].modifiers == 0,
              "iPhone Full Keyboard Access uses Tab, Shift-Tab and Space");
