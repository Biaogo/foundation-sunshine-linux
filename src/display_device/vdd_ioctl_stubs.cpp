/**
 * @file vdd_ioctl.cpp (non-Windows stub)
 * @brief Stub implementation of the ZakoVDD IOCTL transport for platforms
 *        where the driver does not exist (Linux/macOS).
 *
 * The real implementation is Windows-only (SetupAPI + winioctl). On other
 * platforms every probe reports "not installed" / "interface missing" so
 * callers can branch on the same typed results as on Windows. This keeps
 * `session.cpp` and the WebUI/tray code compilable without forking the
 * whole display_device layer.
 */

#include "vdd_ioctl.h"

namespace display_device::vdd_ioctl {

  std::uint32_t
  required_sealed_frame_channel_flags() {
    return 0;
  }

  adapter_status_t
  query_adapter_status() {
    return {};
  }

  bool
  adapter_present() {
    return false;
  }

  result
  send_command(const std::wstring &command) {
    (void) command;
    return result::interface_missing;
  }

  bool
  ping() {
    return false;
  }

  frame_channel_status
  query_frame_channel_caps(frame_channel_caps &caps) {
    caps = {};
    return frame_channel_status::interface_missing;
  }

  frame_channel_open_status
  open_frame_channel(const frame_channel_open_request &request,
                     frame_channel_open_response &response,
                     bool log_failures) {
    (void) request;
    (void) response;
    (void) log_failures;
    return frame_channel_open_status::interface_missing;
  }

}  // namespace display_device::vdd_ioctl
