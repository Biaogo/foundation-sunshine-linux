/**
 * @file src/platform/linux/virtual_display.h
 * @brief Client-selectable virtual display identifiers for the Linux virtual-display feature.
 *
 * The virtual monitor itself is created/destroyed by the user's `global_prep_cmd` hooks
 * (`krfb-virtualmonitor` + `kscreen-doctor`) around a stream session; these identifiers are what a
 * client sees in the display list and may select, and what the launch path resolves to the real
 * capture output name.
 */
#pragma once

namespace platf {
  /// Client-visible id of the dynamic virtual monitor, captured through the KWin ScreenCast backend.
  inline constexpr auto VDISPLAY_KWIN_ID = "虚拟-KWin";

  /// Client-visible id of the dynamic virtual monitor, captured through the KMS backend (no compositor).
  inline constexpr auto VDISPLAY_KMS_ID = "虚拟-KMS";

  /**
   * @brief Capture output name the prep hooks give the virtual monitor.
   *
   * The do-hook starts `krfb-virtualmonitor --name SunshineVirt`, which kscreen exposes as the
   * `Virtual-SunshineVirt` output; a virtual pick is translated to this name before the capture
   * path resolves it.
   */
  inline constexpr auto VIRTUAL_DISPLAY_OUTPUT_NAME = "Virtual-SunshineVirt";

  /// Value exported as SUNSHINE_CLIENT_VIRTUAL_DISPLAY for a virtual-KWin pick.
  inline constexpr auto VIRTUAL_DISPLAY_HOOK_KWIN = "kwin";

  /// Value exported as SUNSHINE_CLIENT_VIRTUAL_DISPLAY for a virtual-KMS pick.
  inline constexpr auto VIRTUAL_DISPLAY_HOOK_KMS = "kms";

  /**
   * @brief Whether the display list served to clients may advertise the virtual ids.
   *
   * Keep this false until the launch path honours them (display-intent resolution plus the
   * `global_prep_cmd` do-hook that creates the monitor before the encoder probe runs). Advertising
   * an id that launch cannot honour makes a client that picks it receive a hard 503 instead of a
   * working stream, which is worse than not offering the feature at all.
   */
  inline constexpr bool OFFER_VIRTUAL_DISPLAY_IDS = false;
}  // namespace platf
