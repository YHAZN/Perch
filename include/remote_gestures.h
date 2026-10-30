#pragma once
#include <stdint.h>

// C++14 tests evaluate the same state machine at compile time. Arduino's C++11
// build uses ordinary methods, with no runtime/framework dependency.
#if __cplusplus >= 201402L
#define REMOTE_EVAL constexpr
#else
#define REMOTE_EVAL
#endif

namespace RemoteGesture {
enum class Mode { Pointer, Media, Slides, Navigation };
enum class Kind {
  None, Motion, Scroll, Click, RightClick, DragBegin, DragEnd,
  Next, Previous, PlayPause, Mute, Volume, FocusNext, FocusPrevious, Up, Down, Activate, Enter
};
struct Event {
  Kind kind;
  int x, y;
  constexpr Event(Kind k = Kind::None, int a = 0, int b = 0) : kind(k), x(a), y(b) {}
};
constexpr int magnitude(int v) { return v < 0 ? -v : v; }

class Surface {
 public:
  REMOTE_EVAL Event press(Mode mode, int x, int y, uint32_t now, int width) {
    const bool doubleTap = tapArmed_ && mode == Mode::Pointer && mode_ == mode && now - lastTap_ <= 300 &&
                           magnitude(x - tapX_) <= 24 && magnitude(y - tapY_) <= 24;
    mode_ = mode;
    active_ = true;
    moved_ = held_ = false;
    edgeScroll_ = mode == Mode::Pointer && x >= width - 40;
    drag_ = doubleTap && !edgeScroll_;
    tapArmed_ = false;
    sx_ = x_ = x;
    sy_ = y_ = y;
    started_ = now;
    carry_ = axis_ = 0;
    return Event(drag_ ? Kind::DragBegin : Kind::None);
  }

  REMOTE_EVAL Event move(int x, int y) {
    if (!active_ || held_) return Event();
    int dx = x - x_, dy = y - y_;
    const bool wasMoved = moved_;
    x_ = x;
    y_ = y;
    if (magnitude(x - sx_) > 4 || magnitude(y - sy_) > 4) moved_ = true;
    if (mode_ == Mode::Pointer) {
      if (edgeScroll_) {
        carry_ -= dy;
        const int steps = carry_ / 9;
        carry_ -= steps * 9;
        return Event(steps ? Kind::Scroll : Kind::None, steps);
      }
      if (!moved_) return Event();
      if (!wasMoved) { dx = x - sx_; dy = y - sy_; }
      const int distance = magnitude(dx) + magnitude(dy);
      const int gain = distance > 14 ? 3 : distance > 5 ? 2 : 1;
      return Event(Kind::Motion, dx * gain, dy * gain);
    }
    if (mode_ == Mode::Media) {
      if (!axis_ && (magnitude(x - sx_) > 12 || magnitude(y - sy_) > 12)) {
        axis_ = magnitude(y - sy_) > magnitude(x - sx_) ? 2 : 1;
        if (axis_ == 2) carry_ = sy_ - y;
      } else if (axis_ == 2) carry_ -= dy;
      if (axis_ == 2) {
        const int steps = carry_ / 18;
        carry_ -= steps * 18;
        return Event(steps ? Kind::Volume : Kind::None, steps);
      }
    }
    return Event();
  }

  REMOTE_EVAL Event hold(uint32_t now) {
    if (!active_ || moved_ || held_ || drag_ || edgeScroll_ || now - started_ < 550) return Event();
    held_ = true;
    if (mode_ == Mode::Pointer) return Event(Kind::RightClick);
    if (mode_ == Mode::Media) return Event(Kind::Mute);
    if (mode_ == Mode::Slides) return Event(Kind::Previous);
    return Event(Kind::Enter);
  }

  REMOTE_EVAL Event release(uint32_t now, bool cancelled = false) {
    if (!active_) return Event();
    active_ = false;
    if (drag_) { drag_ = false; return Event(Kind::DragEnd); }
    if (cancelled || held_ || edgeScroll_) { tapArmed_ = false; return Event(); }
    const int dx = x_ - sx_, dy = y_ - sy_;
    if (mode_ == Mode::Pointer && !moved_ && now - started_ <= 350) {
      tapArmed_ = true;
      lastTap_ = now;
      tapX_ = x_;
      tapY_ = y_;
      return Event(Kind::Click);
    }
    if ((mode_ == Mode::Media && axis_ == 1) || mode_ == Mode::Slides) {
      if (magnitude(dx) >= 36 && magnitude(dx) > magnitude(dy))
        return Event(dx < 0 ? Kind::Next : Kind::Previous);
    }
    if (mode_ == Mode::Navigation && (magnitude(dx) >= 36 || magnitude(dy) >= 36)) {
      if (magnitude(dx) > magnitude(dy)) return Event(dx < 0 ? Kind::FocusNext : Kind::FocusPrevious);
      return Event(dy < 0 ? Kind::Up : Kind::Down);
    }
    if (!moved_ && now - started_ <= 350) {
      if (mode_ == Mode::Media) return Event(Kind::PlayPause);
      if (mode_ == Mode::Slides) return Event(Kind::Next);
      if (mode_ == Mode::Navigation) return Event(Kind::Activate);
    }
    return Event();
  }

 private:
  Mode mode_ = Mode::Pointer;
  bool active_ = false, moved_ = false, held_ = false, drag_ = false, edgeScroll_ = false, tapArmed_ = false;
  int x_ = 0, y_ = 0, sx_ = 0, sy_ = 0, carry_ = 0, axis_ = 0, tapX_ = 0, tapY_ = 0;
  uint32_t started_ = 0, lastTap_ = 0;
};
}  // namespace RemoteGesture
#undef REMOTE_EVAL
