// local includes
#include "src/display_device/settings.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

#include <algorithm>

namespace display_device {

  device_info_map_t
  enum_available_devices() {
    // Linux has no Windows-style display topology DB. Populate the map from
    // the live capture-backend enumeration so that client-picked displays
    // validate correctly (mirrors the IDs used by kwingrab/kmsgrab: the
    // output name for kwin capture, numeric ids for kms).
    device_info_map_t devices;
#ifdef SUNSHINE_BUILD_KWIN
    for (const auto &name : platf::kwin_display_names()) {
      if (name.empty()) {
        continue;
      }
      devices[name] = device_info_t {
        name,
        name,
        device_state_e::active,
        hdr_state_e::unknown
      };
    }
#endif
    return devices;
  }

  std::string
  get_display_name(const std::string &value) {
    // Not implemented, but just passthrough the value
    return value;
  }

  std::string
  find_one_of_the_available_devices(const std::string &device_id) {
    // Match against the live output list of the active capture backend.
    // Output names double as device ids on Linux (kwin capture uses the
    // compositor output name; kmsgrab uses numeric ids which we also accept).
    if (device_id.empty()) {
      return {};
    }

#ifdef SUNSHINE_BUILD_KWIN
    if (config::video.capture == "kwin") {
      const auto names = platf::kwin_display_names();
      if (std::find(names.begin(), names.end(), device_id) != names.end()) {
        return device_id;
      }
    }
#endif
#ifdef SUNSHINE_BUILD_DRM
    if (config::video.capture == "kms") {
      const auto names = platf::display_names(platf::mem_type_e::unknown);
      if (std::find(names.begin(), names.end(), device_id) != names.end()) {
        return device_id;
      }
    }
#endif

    // Fall back: accept when the requested id matches any enumerated name of
    // the auto-selected backend.
    const auto names = platf::display_names(platf::mem_type_e::unknown);
    if (std::find(names.begin(), names.end(), device_id) != names.end()) {
      return device_id;
    }
    return {};
  }

  std::string
  find_device_by_friendlyname(const std::string &friendly_name) {
    // Not implemented: no Windows display topology on this platform.
    (void) friendly_name;
    return {};
  }

  device_display_mode_map_t
  get_current_display_modes(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_display_modes(const device_display_mode_map_t &) {
    // Not implemented
    return false;
  }

  bool
  is_primary_device(const std::string &) {
    // Not implemented
    return false;
  }

  bool
  set_as_primary_device(const std::string &) {
    // Not implemented
    return false;
  }

  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_hdr_states(const hdr_state_map_t &) {
    // Not implemented
    return false;
  }

  active_topology_t
  get_current_topology() {
    // Not implemented
    return {};
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    // Not implemented
    return false;
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) {
    // Not implemented
    return false;
  }

  bool
  set_topology(const active_topology_t &) {
    // Not implemented
    return false;
  }

  struct settings_t::audio_data_t {
    // Not implemented
  };

  struct settings_t::persistent_data_t {
    // Not implemented
  };

  settings_t::settings_t() {
    // Not implemented
  }

  settings_t::~settings_t() {
    // Not implemented
  }

  bool
  settings_t::is_changing_settings_going_to_fail() const {
    // Not implemented
    return false;
  }

  settings_t::apply_result_t
  settings_t::apply_config(
    const parsed_config_t &config,
    const rtsp_stream::launch_session_t &session,
    const boost::optional<active_topology_t> &pre_saved_initial_topology) {
    // Not implemented
    (void) config;
    (void) session;
    (void) pre_saved_initial_topology;
    return { apply_result_t::result_e::success };
  }

  bool
  settings_t::revert_settings(revert_reason_e reason, bool skip_vdd_destroy) {
    // Not implemented
    (void)reason;  // Unused parameter
    (void)skip_vdd_destroy;  // Unused parameter
    return true;
  }

  void
  settings_t::reset_persistence() {
    // Not implemented
  }

  void
  settings_t::capture_audio_sink() {
    // Not implemented: audio sink preservation is Windows-specific.
  }

  void
  settings_t::release_audio_sink() {
    // Not implemented: audio sink preservation is Windows-specific.
  }

  bool
  settings_t::has_persistent_data() const {
    // Not implemented
    return false;
  }

  bool
  settings_t::is_vdd_in_initial_topology() const {
    // Not implemented: no VDD topology on this platform.
    return false;
  }

  void
  settings_t::remove_vdd_from_initial_topology(const std::string &vdd_id) {
    // Not implemented: no VDD topology on this platform.
    (void) vdd_id;
  }

  void
  settings_t::replace_vdd_id(const std::string &old_id, const std::string &new_id) {
    // Not implemented: no VDD topology on this platform.
    (void) old_id;
    (void) new_id;
  }

}  // namespace display_device
