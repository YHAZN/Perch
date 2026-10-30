// The compiler executes these state-machine tests. No display or connected host needed.
#include "remote_gestures.h"
using namespace RemoteGesture;

constexpr bool pointer() {
  Surface s;
  s.press(Mode::Pointer, 50, 50, 0, 224);
  if (s.move(53, 51).kind != Kind::None || s.release(100).kind != Kind::Click) return false;
  if (s.press(Mode::Pointer, 52, 50, 200, 224).kind != Kind::DragBegin) return false;
  if (s.hold(800).kind != Kind::None || s.move(80, 70).kind != Kind::Motion) return false;
  return s.release(900, true).kind == Kind::DragEnd && s.release(920).kind == Kind::None;
}
constexpr bool scrollAndHold() {
  Surface s;
  s.press(Mode::Pointer, 210, 50, 0, 224);
  if (s.move(80, 77).kind != Kind::Scroll || s.release(300).kind != Kind::None) return false;
  s.press(Mode::Pointer, 60, 70, 1000, 224);
  if (s.hold(1549).kind != Kind::None || s.hold(1550).kind != Kind::RightClick) return false;
  return s.hold(1600).kind == Kind::None && s.move(100, 90).kind == Kind::None &&
         s.release(1700).kind == Kind::None;
}
constexpr bool cancelledTap() {
  Surface s;
  s.press(Mode::Pointer, 70, 70, 0, 224);
  return s.release(100, true).kind == Kind::None &&
         s.press(Mode::Pointer, 70, 70, 200, 224).kind == Kind::None;
}
constexpr bool media() {
  Surface s;
  s.press(Mode::Media, 100, 100, 0, 224);
  const auto volume = s.move(102, 64);
  if (volume.kind != Kind::Volume || volume.x != 2 || s.release(200).kind != Kind::None) return false;
  s.press(Mode::Media, 100, 100, 500, 224);
  s.move(40, 105);
  if (s.release(800).kind != Kind::Next) return false;
  s.press(Mode::Media, 100, 100, 1000, 224);
  return s.hold(1550).kind == Kind::Mute && s.release(1600).kind == Kind::None;
}
constexpr bool slidesAndNavigation() {
  Surface s;
  s.press(Mode::Slides, 100, 100, 0, 224);
  if (s.release(100).kind != Kind::Next) return false;
  s.press(Mode::Slides, 100, 100, 500, 224);
  if (s.hold(1100).kind != Kind::Previous || s.release(1200).kind != Kind::None) return false;
  s.press(Mode::Navigation, 120, 100, 2000, 224);
  s.move(70, 100);
  if (s.release(2200).kind != Kind::FocusNext) return false;
  s.press(Mode::Navigation, 120, 100, 2500, 224);
  s.move(120, 50);
  if (s.release(2800).kind != Kind::Up) return false;
  s.press(Mode::Navigation, 100, 100, 3000, 224);
  return s.release(3100).kind == Kind::Activate;
}
static_assert(pointer(), "Tap jitter, double-tap drag or cancellation regression");
static_assert(scrollAndHold(), "Edge-scroll latching or right-click consumed touch regression");
static_assert(cancelledTap(), "Cancelled input must not click or arm a double tap");
static_assert(media(), "Media axis locking or release action regression");
static_assert(slidesAndNavigation(), "Slide or keyboard navigation gesture regression");
constexpr bool clockRollover() {
  Surface s;
  s.press(Mode::Pointer, 60, 70, 0xFFFFFFF0u, 224);
  return s.hold(550).kind == Kind::RightClick && s.release(600).kind == Kind::None;
}
static_assert(clockRollover(), "Hold timing must survive the millis counter wrapping");
