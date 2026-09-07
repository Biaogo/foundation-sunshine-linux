/**
 * @file session_listener.cpp (non-Windows stub)
 * @brief No-op implementation of the session event listener for platforms
 *        without WTS session notifications.
 */

#include "session_listener.h"

namespace display_device {

  bool
  SessionEventListener::init() {
    return false;
  }

  void
  SessionEventListener::deinit() {
  }

  bool
  SessionEventListener::is_event_based() {
    return false;
  }

  void
  SessionEventListener::add_unlock_task(UnlockCallback task) {
    (void) task;
  }

  void
  SessionEventListener::clear_unlock_task() {
  }

}  // namespace display_device
