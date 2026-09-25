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

#include <memory>
#include <string>

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
   * @brief Owns the dynamically created virtual monitor for one stream session.
   *
   * Step 2 of docs/virtual-display-linux.md: instead of relying on the user's `global_prep_cmd`
   * hook, Sunshine itself starts `krfb-virtualmonitor`, waits for the compositor to enumerate the
   * output, makes it live with `kscreen-doctor` and kills the helper again when the session ends
   * (the output disappears with it). Nothing is created before a client actually asks for it.
   */
  class virtual_display_t {
  public:
    virtual_display_t() = default;
    ~virtual_display_t();

    virtual_display_t(const virtual_display_t &) = delete;
    virtual_display_t &operator=(const virtual_display_t &) = delete;

    /**
     * @brief Start the helper and make the output usable for capture.
     *
     * @param width Requested width in pixels.
     * @param height Requested height in pixels.
     * @param fps Requested refresh rate.
     * @return True when the compositor enumerates the created output.
     */
    bool start(int width, int height, int fps);

    /// Whether this object currently owns a running helper process.
    bool active() const;

    /// KWin output name of the created monitor (empty when inactive).
    const std::string &output_name() const;

    /// Kill the helper process; the output disappears with it.
    void stop();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Opaque state, keeps this header free of process types.
  };

  /**
   * @brief Report whether the built-in virtual monitor can be created on this host.
   *
   * @return True when `krfb-virtualmonitor` and `kscreen-doctor` are both reachable.
   */
  bool virtual_display_available();

  /**
   * @brief Whether the display list served to clients may advertise the virtual ids.
   *
   * Flipped on 2026-09-25 after the launch path was verified end-to-end on the target host: the
   * host-config (默认) pick ran the global do-hook before the encoder probe and streamed through
   * KWin ScreenCast with the virtual monitor created by the hook. Both virtual ids resolve to the
   * same plumbing (id -> VIRTUAL_DISPLAY_OUTPUT_NAME + the hook switch), so they share that path.
   * Turn it back off if a client picking `虚拟-KWin`/`虚拟-KMS` cannot start a session: an id that
   * launch cannot honour yields a hard 503 instead of a working stream.
   */
  inline constexpr bool OFFER_VIRTUAL_DISPLAY_IDS = true;
}  // namespace platf
