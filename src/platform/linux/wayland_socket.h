/**
 * @file src/platform/linux/wayland_socket.h
 * @brief Resolution of the Wayland socket used by the Linux backends.
 */
#pragma once

#include <string>

namespace platf {
  /**
   * @brief Resolve a connectable Wayland socket for this process.
   *
   * A long-lived service can inherit a `WAYLAND_DISPLAY` value from an earlier session while the
   * compositor is now listening on a different socket. A configured value is kept when it connects;
   * otherwise the lowest-numbered `wayland-<digits>` socket below `XDG_RUNTIME_DIR` is selected and
   * exported so child helpers inherit it.
   *
   * The resolution is cached, but a cached socket is checked again on every call. If it stopped
   * accepting connections, the runtime directory is scanned again.
   *
   * @return The socket name or absolute path to use; empty when no compositor is reachable.
   */
  std::string resolve_wayland_socket();
}  // namespace platf
