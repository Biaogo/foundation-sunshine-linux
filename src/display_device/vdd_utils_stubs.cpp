/**
 * @file vdd_utils.cpp (non-Windows stub)
 * @brief Stub implementation of the ZakoVDD virtual display utilities for
 *        platforms where the driver does not exist (Linux/macOS).
 *
 * The real implementation drives the Windows-only ZakoVDD driver through the
 * IOCTL transport (see vdd_ioctl.cpp). On other platforms every operation is
 * a no-op that reports "not installed", which is the semantically correct
 * state: the web UI and tray render VDD as unavailable, and streaming code
 * paths that consult VDD prerequisites fail fast.
 */

#include "vdd_utils.h"

#include <boost/optional.hpp>

#include "src/display_device/display_device.h"
#include "src/display_device/parsed_config.h"
#include "src/display_device/to_string.h"
#include "src/logging.h"

namespace display_device {
  namespace vdd_utils {

    const std::chrono::milliseconds kDefaultDebounceInterval { 2000 };

    vdd_status_t
    get_vdd_status() {
      vdd_status_t status;
      status.state = "not_installed";
      return status;
    }

    bool
    hardware_cursor_export_enabled(std::string value) {
      (void) value;
      return false;
    }

    bool
    ensure_hardware_cursor_enabled_for_capture(bool *changed) {
      if (changed) {
        *changed = false;
      }
      return false;
    }

    set_vdd_result
    set_vdd_session_mode(const parsed_config_t &config, const VddSettings &settings) {
      (void) config;
      (void) settings;
      return set_vdd_result::interface_missing;
    }

    std::string
    generate_client_guid(const std::string &identifier) {
      // Match the Windows contract: empty identifier yields an empty GUID.
      return {};
    }

    physical_size_t
    get_client_physical_size(const std::string &client_name) {
      (void) client_name;
      return {};
    }

    bool
    create_vdd_monitor(const std::string &client_identifier, const hdr_brightness_t &hdr_brightness, const physical_size_t &physical_size) {
      (void) client_identifier;
      (void) hdr_brightness;
      (void) physical_size;
      BOOST_LOG(error) << "VDD: virtual display driver is only available on Windows";
      return false;
    }

    bool
    create_vdd_monitor_noninteractive() {
      BOOST_LOG(error) << "VDD: virtual display driver is only available on Windows";
      return false;
    }

    bool
    destroy_vdd_monitor() {
      return false;
    }

    void
    destroy_vdd_monitor_nolog() {
    }

    void
    disable_enable_vdd() {
    }

    bool
    toggle_display_power() {
      return false;
    }

    bool
    is_display_on() {
      return false;
    }

    bool
    set_hdr_state(bool enable_hdr) {
      (void) enable_hdr;
      return false;
    }

    bool
    ensure_vdd_extended_mode(const std::string &device_id, const std::vector<std::string> &physical_devices_to_preserve) {
      (void) device_id;
      (void) physical_devices_to_preserve;
      return false;
    }

    bool
    apply_vdd_prep(const std::string &vdd_device_id, parsed_config_t::vdd_prep_e vdd_prep,
      const boost::optional<device_info_map_t> &pre_vdd_devices) {
      (void) vdd_device_id;
      (void) vdd_prep;
      (void) pre_vdd_devices;
      return false;
    }

    VddSettings
    prepare_vdd_settings(const parsed_config_t &config) {
      (void) config;
      return {};
    }

    bool
    is_mode_advertised(const std::string &device_id, const display_mode_t &requested_mode) {
      (void) device_id;
      (void) requested_mode;
      return false;
    }

    bool
    wait_for_mode_publication(const std::string &device_id, const display_mode_t &requested_mode) {
      (void) device_id;
      (void) requested_mode;
      return false;
    }

  }  // namespace vdd_utils
}  // namespace display_device
