#pragma once
#include <stdint.h>

#if __cplusplus >= 201402L
#define NETWORK_EVAL constexpr
#else
#define NETWORK_EVAL
#endif

enum class NetworkJoinState { Idle, Joining, Connected, Failed };
enum class NetworkJoinError { None, NotFound, Rejected, Timeout };

// Manual joins own the radio until success or a bounded failure. UI scans and
// background candidate rotation must not interrupt them, even on the Wi-Fi page.
class NetworkAttempt {
 public:
  NETWORK_EVAL void start(uint32_t now) {
    state_ = NetworkJoinState::Joining;
    error_ = observed_ = NetworkJoinError::None;
    started_ = errorSince_ = now;
  }
  NETWORK_EVAL void cancel() { state_ = NetworkJoinState::Idle; error_ = NetworkJoinError::None; }
  NETWORK_EVAL void poll(uint32_t now, bool connected, NetworkJoinError error) {
    if (state_ != NetworkJoinState::Joining) return;
    if (connected) { state_ = NetworkJoinState::Connected; error_ = NetworkJoinError::None; return; }
    if (error != observed_) { observed_ = error; errorSince_ = now; }
    if (error != NetworkJoinError::None && now - errorSince_ >= 2000) {
      state_ = NetworkJoinState::Failed; error_ = error; return;
    }
    if (now - started_ >= 25000) { state_ = NetworkJoinState::Failed; error_ = NetworkJoinError::Timeout; }
  }
  constexpr NetworkJoinState state() const { return state_; }
  constexpr NetworkJoinError error() const { return error_; }
  constexpr bool joining() const { return state_ == NetworkJoinState::Joining; }
  constexpr bool scansAllowed() const { return !joining(); }
 private:
  NetworkJoinState state_ = NetworkJoinState::Idle;
  NetworkJoinError error_ = NetworkJoinError::None, observed_ = NetworkJoinError::None;
  uint32_t started_ = 0, errorSince_ = 0;
};
#undef NETWORK_EVAL
