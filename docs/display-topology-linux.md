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
- the do-hook reads the display-preparation options from its environment and applies the matching
  kscreen actions after enabling the virtual output;
- the undo-hook restores the saved snapshot (`kscreen-doctor -j` taken at the start), honouring
  `dd_config_revert_delay` / `dd_config_revert_on_disconnect`.

**Done (2026-09-25, fork `refork/linux`):** the env plumbing. `proc_t::update_session_env()` now
exports, per session, alongside the existing display switch:

| variable | values |
|---|---|
| `SUNSHINE_CLIENT_DD_CONFIG` | `disabled` \| `verify_only` \| `ensure_active` \| `ensure_primary` \| `ensure_only_display` |
| `SUNSHINE_CLIENT_DD_RESOLUTION` | `disabled` \| `automatic` \| `manual` |
| `SUNSHINE_CLIENT_DD_REFRESH_RATE` | `disabled` \| `automatic` \| `manual` |
| `SUNSHINE_CLIENT_DD_HDR` | `disabled` \| `automatic` |

They are erased before being re-set (same anti-leak discipline as `SUNSHINE_CLIENT_DISPLAY_NAME`),
and the C++ enum → string mapping is one switch per enum in `process.cpp`. Remaining work: the hook
actions themselves. Note the UI's six wordings collapse onto five enum values plus the host's
`output_name` default, and that 副屏串流 has no upstream enum of its own — it is `ensure_active` with
a position/priority adjustment, so the hook must distinguish it from the wording, not the enum.

Pros: no C++ surface beyond the export, easy to iterate, matches the deployed architecture. Cons:
topology logic lives in a shell script, and it only applies to sessions that run the hook.

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

## 5. Decisions (operator, 2026-09-25) — the questions this design asked

1. 副屏串流模式's "primary" side: **eDP-1 as the physical primary** (it is the host's only other
   output; it is an invisible forced-on head, so the mode is "virtual output extends to the right"),
   with the caveat that with no real monitor attached the mode is mostly a topology rehearsal.
2. 主屏串流模式: **priority only** — the virtual output already sits at the origin, no repositioning.
3. The modes **apply to the hook-managed virtual output only**; a physical pick never has its
   topology rewritten by this layer.
4. **Phase 1 (hook) first**; Phase 2 (C++) only if the script becomes the bottleneck.

These were answered as "默认" (accept the proposed defaults), so implementation may proceed on them —
re-confirm only if a mode's behaviour disagrees with what the operator actually wants.

## 6. Interaction with the absolute-input (touch/pen) binding

Every topology change re-creates or re-parents KWin's input devices, and KWin leaves `outputName`
**empty** on libvirtualhid's absolute-input devices (see `sunshine-touchbind.sh`): unbound devices get
their events dropped, which shows up as "touch does nothing / sticks to a corner". Therefore:

- the topology layer must **re-run the touch binding after it changes the layout** (same
  `busctl --user set-property … outputName s <output>` call, re-checking that the devices are still
  bound), and
- `ensure_only_display`/`ensure_primary` — the modes that change which output is active — are exactly
  the cases where KWin may drop the binding, so the binding must be re-verified *after* them, not
  before. Binding once at session start (today's hook) is sufficient only while the topology stays
  untouched.

