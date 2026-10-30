#include "network_attempt.h"

constexpr bool manualJoinOwnsRadio() {
  NetworkAttempt a;
  a.start(100);
  if (!a.joining() || a.scansAllowed()) return false;
  a.poll(20000, false, NetworkJoinError::None);
  if (!a.joining() || a.scansAllowed()) return false;
  a.poll(25100, false, NetworkJoinError::None);
  return a.state() == NetworkJoinState::Failed && a.error() == NetworkJoinError::Timeout && a.scansAllowed();
}
constexpr bool transientFailureDoesNotAbort() {
  NetworkAttempt a;
  a.start(0);
  a.poll(1000, false, NetworkJoinError::NotFound);
  a.poll(2000, false, NetworkJoinError::None);
  a.poll(4000, false, NetworkJoinError::None);
  if (!a.joining()) return false;
  a.poll(6000, true, NetworkJoinError::None);
  return a.state() == NetworkJoinState::Connected && a.scansAllowed();
}
constexpr bool rejectionHasResult() {
  NetworkAttempt a;
  a.start(0);
  a.poll(1000, false, NetworkJoinError::Rejected);
  if (!a.joining()) return false;
  a.poll(3000, false, NetworkJoinError::Rejected);
  return a.state() == NetworkJoinState::Failed && a.error() == NetworkJoinError::Rejected;
}
constexpr bool retryAndCancel() {
  NetworkAttempt a;
  a.start(0);
  a.poll(25000, false, NetworkJoinError::None);
  a.start(30000);
  if (a.error() != NetworkJoinError::None || !a.joining()) return false;
  a.cancel();
  a.poll(31000, true, NetworkJoinError::None);
  return a.state() == NetworkJoinState::Idle && a.scansAllowed();
}
constexpr bool deadlineAndRollover() {
  NetworkAttempt a;
  a.start(0xFFFFFC18u); // 1000 ms before rollover
  a.poll(24000, false, NetworkJoinError::None);
  if (a.error() != NetworkJoinError::Timeout) return false;
  a.start(0);
  a.poll(25000, true, NetworkJoinError::None);
  return a.state() == NetworkJoinState::Connected;
}
static_assert(manualJoinOwnsRadio(), "Scans must not cancel a manual join; joining must time out");
static_assert(transientFailureDoesNotAbort(), "A transient scan failure must not defeat a successful retry");
static_assert(rejectionHasResult(), "A persistent rejection needs a visible failure result");
static_assert(retryAndCancel(), "Retry must clear errors; a cancelled join must stay cancelled");
static_assert(deadlineAndRollover(), "Timeout must survive rollover and accept success at the deadline");
