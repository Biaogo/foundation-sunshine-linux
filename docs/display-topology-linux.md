# Display topology modes on Linux — design

Goal: make the `dd_*` display-preparation modes (the "屏幕组合功能" list) actually work on this
Linux host, where upstream only implements the Windows backend and our base's
`display_device::configure_display()` is a stub.

Anchors: fork `src/display_device/session.cpp` (its device-prep state machine), upstream
`src/display_device.h` (`device_prep_e`), this host's krfb virtual-monitor hook
(`~/.local/bin/sunshine-vdisplay-*.sh`), and the config keys already present in this tree
(`dd_configuration_option`, `dd_resolution_option`, `dd_hdr_option`, `dd_config_revert_delay`,
`dd_config_revert_on_disconnect`, `dd_configuration_option`, `output_name`).

## 1. The modes

| UI wording (fork) | upstream `device_prep_e` | what it must do on Linux |
|---|---|---|
| 跟随主机 / no override | `verify_only` (or disabled) | change nothing; current behaviour (the hook still creates/enables the virtual output) |
| 无操作模式 | `no_operation` | keep the present layout; do not enable/disable/move anything |
| 激活模式 | `ensure_active` | enable the target display if it is disabled; leave the rest as-is |
| 主屏串流模式 | `ensure_primary` | enable the target and make it the primary output (in Plasma: `priority.1`) |
| 副屏串流模式 | `ensure_secondary` | the physical output stays primary; the target becomes an extra output to its right (`position` + `priority.2`) |
| 仅启用指定显示器模式 | `ensure_only_display` | only the target output enabled; every other output disabled |

The "target display" is whatever the session selected: the hook-managed virtual output
(`Virtual-SunshineVirt`) for the default/虚拟-KWin/虚拟-KMS picks, or a physical output for an
explicit pick. Physical picks must stay untouched by the topology layer unless the selected mode
says otherwise (that is what the current hook contract already guarantees).

## 2. How it maps onto this host

Plasma exposes everything needed through kscreen (`kscreen-doctor` is a CLI over the same API):

| Need | primitive |
|---|---|
| enable / disable an output | `output.<id>.enable` / `output.<id>.disable` |
| set the mode | `output.<id>.mode.<W>x<H>@<fps>` |
| set the primary | `output.<id>.priority.1` (higher number = lower priority) |
| position (secondary) | `output.<id>.position.<X>,<Y>` |
| scale | `output.<id>.scale.<F>` |
| snapshot for revert | `kscreen-doctor -j` (JSON) captured before the first change |

Lifecycle: **the topology change must happen after the virtual output exists**, i.e. inside the
existing do-hook, after `krfb-virtualmonitor` started and `kscreen-doctor output.<uuid>.enable`
succeeded — and it must be re-applied if KWin re-arranges outputs when a monitor is (dis)connected
(the fork has `session_listener.*` for exactly that; on this host a single re-apply after the
output appears is enough, see §4).

## 3. Phased implementation

**Phase 1 — hook-driven (smallest, covers the user-visible feature).**
Extend the existing do/undo hooks:
- the do-hook reads the `dd_configuration_option` value (exported to the hook environment like the
  other `SUNSHINE_*` variables) and applies the matching kscreen actions after enabling the virtual
  output;
- the undo-hook restores the saved snapshot (`kscreen-doctor -j` taken at the start), honouring
  `dd_config_revert_delay` / `dd_config_revert_on_disconnect`.
- `SUNSHINE_CLIENT_*` env plumbing already exists (`process.cpp` exports the session env), so this
  needs one more variable plus hook logic.

Pros: no C++ surface, easy to iterate, matches the deployed architecture. Cons: topology logic lives
in a shell script, and it only applies to sessions that run the hook.

**Phase 2 — C++ `display_device` for Linux.** Implement `display_device::configure_display()` (Linux
branch) to perform the same actions through the kscreen DBus interface, with the snapshot/revert
state machine owned by the process (upstream's `display_device/session.cpp` shape). Benefits:
applies on every launch path, reports failures into the log/HTTP response, survives hook changes.

Phase 1 is what I would ship first; Phase 2 only if the script becomes the bottleneck.

## 4. Pitfalls that this design must respect (all observed on this host)

1. **The virtual output does not exist until the hook creates it** — a topology change applied
   before `krfb-virtualmonitor` + `kscreen-doctor enable` will silently target nothing.
2. **KWin re-lays-out outputs on hotplug**; `krfb` appearing/disappearing triggers that. Applying the
   mode once, right after the enable, is enough today; a re-apply guard (compare `kscreen-doctor -j`
   against the desired state, retry a couple of times) makes it robust.
3. **eDP-1 is the login head** (forced-on dead panel with a fixed EDID). `ensure_only_display` on the
   virtual output means eDP-1 goes dark during the session — that is the intended semantics, but the
   session must not be *started* from a state where eDP-1's disable would take the compositor's only
   scanout away before the virtual output is enabled (order: enable target first, then disable
   others).
4. **Restore must be unconditional.** A dropped client, a crashed hook or a Sunshine restart must all
   leave the host in the pre-session layout; therefore the snapshot must be written to disk
   (`$XDG_RUNTIME_DIR`) and the undo-hook must be idempotent.
5. **The virtual output's scale is 1.5** (logical 1584×720 vs physical 2376×1080). Topology code that
   computes positions/sizes must use logical geometry (that is also the open coordinate bug for
   absolute input, tracked separately).
6. **`dd_config_revert_delay`** semantics: upstream waits before reverting so the client can
   reconnect into the same layout; the hook must honour it (a `sleep` in the undo path is fine).

## 5. Open questions for the operator

1. For 副屏串流模式, which physical output is the "primary" side — eDP-1 (the dead panel with the
   fake EDID) or nothing at all (i.e. the mode is only meaningful with a real monitor attached)?
2. For 主屏串流模式, should the virtual output also be *positioned* at the origin (it currently is),
   or does "primary" only mean priority?
3. Should the modes apply to physical picks too (e.g. selecting eDP-1 with `ensure_only_display`), or
   only to the hook-managed virtual output?
4. Is a hook-only (Phase 1) implementation acceptable, or is the C++ path required for 正式使用?
