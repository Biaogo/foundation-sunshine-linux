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
#include <string_view>
#include <vector>

namespace platf {
  /// Client-visible id of the dynamic virtual monitor, captured through the KWin ScreenCast backend.
  inline constexpr auto VDISPLAY_KWIN_ID = "虚拟-KWin";

  /// Client-visible id of the dynamic virtual monitor, captured through the KMS backend (no compositor).
  inline constexpr auto VDISPLAY_KMS_ID = "虚拟-KMS";

  /**
   * @brief Name a virtual monitor helper reports to the compositor by default.
   *
   * The helper runs as `krfb-virtualmonitor --name SunshineVirt`, which kscreen exposes as the
   * `Virtual-SunshineVirt` output; a virtual pick is translated to that output name before the
   * capture path resolves it. `SUNSHINE_VDISPLAY_NAME` overrides the name so a second instance —
   * the side-by-side test lane, or a probe — owns an output of its own instead of fighting over
   * the one the running service already created (which the compositor would not enumerate twice).
   */
  inline constexpr auto VIRTUAL_DISPLAY_NAME = "SunshineVirt";

  /// VNC port a virtual monitor helper listens on by default; only used locally by the compositor.
  inline constexpr int VIRTUAL_DISPLAY_PORT = 5910;

  /// Environment variable overriding @ref VIRTUAL_DISPLAY_NAME.
  inline constexpr auto VIRTUAL_DISPLAY_NAME_ENV = "SUNSHINE_VDISPLAY_NAME";

  /// Environment variable overriding @ref VIRTUAL_DISPLAY_PORT.
  inline constexpr auto VIRTUAL_DISPLAY_PORT_ENV = "SUNSHINE_VDISPLAY_PORT";

  /**
   * @brief What to start for one virtual monitor, and what to look for afterwards.
   */
  struct virtual_display_identity_t {
    std::string name;          ///< Name handed to the helper, e.g. `SunshineVirt`.
    std::string output_name;   ///< Output the compositor exposes, e.g. `Virtual-SunshineVirt`.
    int port;                  ///< VNC port the helper listens on.
    bool name_override_ignored;  ///< A non-empty `SUNSHINE_VDISPLAY_NAME` could not be used.
    bool port_override_ignored;  ///< A non-empty `SUNSHINE_VDISPLAY_PORT` could not be used.
  };

  /**
   * @brief Resolve the helper identity, honouring the test-instance overrides.
   *
   * The `*_ignored` flags are set when a non-empty override had to be dropped (a blank name, a
   * non-numeric or out-of-range port) so the caller can report the fallback instead of leaving the
   * operator wondering why their override did nothing.
   *
   * @param name_override Value of `SUNSHINE_VDISPLAY_NAME` (empty when unset).
   * @param port_override Value of `SUNSHINE_VDISPLAY_PORT` (empty when unset).
   * @return Name, output name and port to use for this instance's helper.
   */
  virtual_display_identity_t resolve_virtual_display_identity(std::string_view name_override, std::string_view port_override);

  /**
   * @brief Output name the compositor exposes for a helper name.
   * @param name Name handed to the helper.
   * @return `Virtual-` followed by the name.
   */
  std::string virtual_display_output_name(std::string_view name);

  /// Value exported as SUNSHINE_CLIENT_VIRTUAL_DISPLAY for a virtual-KWin pick.
  inline constexpr auto VIRTUAL_DISPLAY_HOOK_KWIN = "kwin";

  /// Value exported as SUNSHINE_CLIENT_VIRTUAL_DISPLAY for a virtual-KMS pick.
  inline constexpr auto VIRTUAL_DISPLAY_HOOK_KMS = "kms";

  /**
   * @brief What a client's display pick resolves to.
   */
  struct display_pick_t {
    std::string name;             ///< Output the capture path resolves, empty when the client picked nothing.
    std::string virtual_display;  ///< Hook switch (`kwin`/`kms`), empty for a physical pick.
  };

  /**
   * @brief Translate the client's display pick into the capture target and the hook switch.
   *
   * Moonlight sends the picked display in the `display_name` launch argument. A virtual id has to
   * become a concrete output name (the one krfb creates) plus the switch that tells a host hook which
   * backend the client asked for; anything else is a physical connector name and is used as-is. The
   * host-config entry is the client's "默认" pick, which mirrors `output_name` — when that names the
   * virtual monitor the switch has to be injected as well, otherwise the hook no-ops and the capture
   * waits for a monitor nobody creates (measured 2026-09-25).
   *
   * @param requested Display the client picked; empty when it did not pick one.
   * @param configured_output_name Sunshine's own `output_name` value.
   * @param identity Helper identity a virtual pick is translated to.
   * @return Capture target and hook switch.
   */
  display_pick_t resolve_display_pick(const std::string &requested, const std::string &configured_output_name,
                                      const virtual_display_identity_t &identity);

  /**
   * @brief Translate the client's display pick using this process' helper identity.
   *
   * @param requested Display the client picked; empty when it did not pick one.
   * @param configured_output_name Sunshine's own `output_name` value.
   * @return Capture target and hook switch.
   */
  display_pick_t resolve_display_pick(const std::string &requested, const std::string &configured_output_name);

  /// Executable that creates the virtual monitor on demand (KDE's krfb virtual-monitor helper).
  inline constexpr auto VIRTUAL_DISPLAY_HELPER = "krfb-virtualmonitor";

  /// Executable that makes a created output live and applies display combinations (KScreen).
  inline constexpr auto KSCREEN_HELPER = "kscreen-doctor";

  /**
   * @brief Resolve one external helper executable without trusting the process' `PATH` alone.
   *
   * A service (systemd user unit, launchd, a bare container entry point) starts with a minimal
   * `PATH`, while the helper may live in `/usr/bin`, only in the distribution's global profile, or
   * next to a portable installation. Candidates are tried in this order:
   *
   *  1. @p configured — an absolute path from `sunshine.conf` (`virtual_display_helper`,
   *     `kscreen_helper`), so a host can point at its own copy;
   *  2. @p path_env — the caller's `$PATH`, which is how this always worked;
   *  3. @ref helper_fallback_dirs — the locations a service' `PATH` usually misses.
   *
   * Never logs: the caller decides whether a missing helper is worth a message, because this runs
   * on every client display-list request.
   *
   * @param tool Executable name to look for (`krfb-virtualmonitor`, `kscreen-doctor`, ...).
   * @param configured Absolute path configured by the user; empty to search only.
   * @param path_env Colon-separated directory list standing in for `$PATH`.
   * @return Absolute path of the first usable candidate, empty when the helper is unavailable.
   */
  std::string find_helper(const std::string &tool, const std::string &configured, const std::string &path_env);

  /**
   * @brief Directories searched after the configured path and `$PATH`.
   *
   * @return Candidate directories in search order, including entries that do not exist.
   */
  std::vector<std::string> helper_fallback_dirs();

  /**
   * @brief One output as the KScreen-backed paths see it.
   */
  struct kscreen_output_t {
    std::string name;  ///< Connector name, for example `DP-1`.
    std::string uuid;  ///< KScreen uuid, the handle `kscreen-doctor` uses.
    bool enabled {};  ///< Whether the output is currently on.
    int priority {};  ///< Priority as reported, `0` when unset.
    std::string geometry;  ///< Geometry, empty when the source does not report one.
  };

  /**
   * @brief Parse the output list out of KWin's own output configuration.
   *
   * Split off from the file access so the document shape can be tested with samples: the flat
   * assumption this started with produced zero outputs on the target host without any error, and a
   * wrong shape is invisible until a session fails to apply the client's display combination.
   *
   * @param text Contents of `kwinoutputconfig.json`.
   * @return Outputs in document order, empty when the text cannot be used.
   */
  std::vector<kscreen_output_t> kscreen_outputs_from_json_text(const std::string &text);

  /**
   * @brief Which outputs a settled topology pass has to switch back on.
   *
   * KWin runs its own output bookkeeping when a new output appears, and the pass Sunshine issues
   * right after applying a mode can beat it by a frame; this decides what still needs an enable once
   * the compositor has had a moment.
   *
   * @param snapshot Outputs as they were before the session started.
   * @param current Outputs as the compositor reports them now.
   * @param target_uuid The session's own output, never touched here.
   * @return uuids to enable, in snapshot order.
   */
  std::vector<std::string> outputs_to_reenable(const std::vector<kscreen_output_t> &snapshot,
                                              const std::vector<kscreen_output_t> &current,
                                              const std::string &target_uuid);

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
   * @brief Create the session's virtual monitor through the built-in path.
   *
   * Used for a session whose client picked a virtual display while the host has no
   * `global_prep_cmd` do-hook: the hook stays the preferred path for hosts that already carry one,
   * so this is a pure addition for installs that never configure it (AppImage, .deb).
   *
   * @param width Requested width in pixels.
   * @param height Requested height in pixels.
   * @param fps Requested refresh rate.
   * @return True when the monitor was created and is enumerated by the compositor.
   */
  bool session_virtual_display_start(int width, int height, int fps);

  /**
   * @brief Remove the session's virtual monitor (no-op when nothing was created).
   */
  void session_virtual_display_stop();

  /**
   * @brief Point the virtual touch/pen devices at the session's output.
   *
   * KWin drops absolute-input events from a device whose `outputName` is empty, and the
   * libvirtualhid devices appear only once the client connects — so this polls (bounded) in the
   * background and sets the property as soon as the touchscreen exists. It replaces the
   * `sunshine-touchbind.sh` waiter for hosts that have no global_prep_cmd hook.
   *
   * @param output_name KWin output the session streams from.
   * @return True when the poller was started.
   */
  bool session_bind_touch(const std::string &output_name);

  /**
   * @brief Apply the client's display-combination mode (`dd_configuration_option`), in-process.
   *
   * Phase 2 of the Linux topology work: the same five modes the host hook implements with kscreen,
   * but done from the process, so hook-less installs (deb/AppImage) get them too. Callers must skip
   * this when a global do-hook is configured — the hook owns the topology on those hosts.
   *
   * @param target_name Output the session was launched against.
   */
  void session_apply_topology(const std::string &target_name);

  /**
   * @brief Restore the topology captured before the first apply, once the last session ends.
   */
  void session_revert_topology();

  /**
   * @brief Whether the display list served to clients may advertise the virtual ids.
   *
   * Flipped on 2026-09-25 after the launch path was verified end-to-end on the target host: the
   * host-config (默认) pick ran the global do-hook before the encoder probe and streamed through
   * KWin ScreenCast with the virtual monitor created by the hook. Both virtual ids resolve to the
   * same plumbing (id -> `Virtual-<helper name>` + the hook switch), so they share that path.
   * Turn it back off if a client picking `虚拟-KWin`/`虚拟-KMS` cannot start a session: an id that
   * launch cannot honour yields a hard 503 instead of a working stream.
   */
  inline constexpr bool OFFER_VIRTUAL_DISPLAY_IDS = true;
}  // namespace platf
