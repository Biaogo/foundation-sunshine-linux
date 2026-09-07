/**
 * @file session_listener.h (non-Windows stub)
 * @brief No-op replacement for the Windows SessionEventListener (WTS
 *        lock/unlock notifications) on platforms without session events.
 *
 * The Windows implementation listens for session lock/unlock events so that
 * pending display-state restores can run as soon as the user unlocks. There
 * is no portable equivalent; on Linux/macOS tasks are dropped and the class
 * simply reports "not event based".
 */

#pragma once

// standard includes
#include <functional>

namespace display_device {

  /**
   * @brief Stub session event listener for non-Windows platforms.
   */
  class SessionEventListener {
  public:
    using UnlockCallback = std::function<void()>;

    /**
     * @brief Initialize the session event listener.
     * @returns False: event-based listening is not available on this platform.
     */
    static bool
    init();

    /**
     * @brief Cleanup and unregister the session event listener.
     */
    static void
    deinit();

    /**
     * @brief Check if event-based listening is active.
     */
    static bool
    is_event_based();

    /**
     * @brief Add a task to be executed on unlock.
     * @note Dropped immediately on this platform.
     */
    static void
    add_unlock_task(UnlockCallback task);

    /**
     * @brief Clear the pending unlock task.
     */
    static void
    clear_unlock_task();
  };

}  // namespace display_device
