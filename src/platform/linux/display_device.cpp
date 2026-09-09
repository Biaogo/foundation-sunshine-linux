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
#ifdef SUNSHINE_BUILD_DRM
    // kmsgrab exposes monitors as numeric ids ("0", "1", ...). Offer them as
    // additional picks so clients can stream the raw DRM output — required
    // pre-login (SDDM) where no Wayland compositor (and thus no kwin
    // capture) exists. Prefer a friendly label when we can correlate the
    // connector name through the Wayland monitor list.
    int kms_index = 0;
    for (const auto &name : platf::display_names(platf::mem_type_e::unknown)) {
      // kwin names ("HDMI-A-1", "Virtual-...") overlap with nothing here:
      // kms ids are pure digits, so duplicates in the map can't happen.
      if (std::find_if(devices.begin(), devices.end(), [&](const auto &entry) {
            return entry.first == name;
          }) != devices.end()) {
        ++kms_index;
        continue;
      }
      std::string label = name;
#ifdef SUNSHINE_BUILD_KWIN
      // Best effort: if a KWin output shares the numeric position with the
      // kms monitor, append its name for readability. Pure cosmetic.
      const auto kwin_names = platf::kwin_display_names();
      if (kms_index < static_cast<int>(kwin_names.size())) {
        label += " (kms: " + kwin_names[kms_index] + ")";
      }
#endif
      devices[name] = device_info_t {
        label,
        name,
        device_state_e::active,
        hdr_state_e::unknown
      };
      ++kms_index;
    }
#endif

    // Virtual display picks (Linux):
    //   虚拟-KWin — the dynamic virtual monitor. Backed by the force-enabled
    //     dead internal panel (eDP-1): the global_prep_cmd do-hook enables it
    //     at stream start and the undo-hook disables it on disconnect, so it
    //     behaves like a display that appears/disappears per session.
    //   虚拟-KMS — the same idea served through the KMS capture backend,
    //     usable pre-login (SDDM) where no Wayland compositor exists.
    // resolve_display_intent() translates these ids to their real targets
    // (eDP-1 / primary kms monitor) before validation.
    devices[VDISPLAY_KWIN_ID] = device_info_t {
      VDISPLAY_KWIN_ID,
      VDISPLAY_KWIN_ID,
      device_state_e::active,
      hdr_state_e::unknown
    };
    devices[VDISPLAY_KMS_ID] = device_info_t {
      VDISPLAY_KMS_ID,
      VDISPLAY_KMS_ID,
      device_state_e::active,
      hdr_state_e::unknown
    };
    return devices;
  }

  std::string
  get_display_name(const std::string &value) {
    // Map virtual picks to their backing capture output names. video.cpp
    // resolves config.display_name through this before matching against
    // the live output list — without the translation a disabled eDP-1
    // makes the poll either fail or grab whichever output comes first.
    if (value == VDISPLAY_KWIN_ID || value == VDISPLAY_KMS_ID) {
      // The virtual monitor is created/removed by the prep-cmd do/undo
      // hooks (krfb-virtualmonitor + kscreen enable) — not a fixed
      // physical output. video.cpp polls for it by this exact name;
      // the hooks name the krfb output "Virtual-SunshineVirt".
      return "Virtual-SunshineVirt";
    }
    // Not implemented otherwise — passthrough the value
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

    // Virtual picks validate by construction: resolve_display_intent has
    // already translated them to their real targets (eDP-1 / primary kms).
    if (device_id == VDISPLAY_KWIN_ID || device_id == VDISPLAY_KMS_ID) {
      return device_id;
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
