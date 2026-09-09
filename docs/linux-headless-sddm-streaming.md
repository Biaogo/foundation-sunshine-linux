# Headless & SDDM streaming on Linux — pre-login displays, virtual monitors, and what is (not) possible

This guide is for anyone streaming a Linux host with Sunshine + Moonlight who
wants the display to appear/disappear with the client — including the
question that comes up every time: *"can the virtual display be created
dynamically while sitting at the login manager?"*

All of this applies to the `linux-support` branch, which ships a KWin
ScreenCast capture backend (direct `zkde_screencast_unstable_v1`) in addition
to the classic KMS/wlr/X11 backends.

## TL;DR

| Login state | Dynamic virtual display? | Resolution/fps follows the client? | Capture path |
|---|---|---|---|
| Desktop session (Wayland compositor running) | ✅ created on connect, destroyed on disconnect | ✅ | KWin ScreenCast |
| Login manager (SDDM/GDM greeter) | ❌ | ❌ fixed at boot (EDID) | KMS / kmsgrab |

The short version: **a compositor-level virtual monitor can only live inside
a running compositor of the streaming user. A greeter runs before that
compositor exists — pre-login you can only stream a *connector*, and
connectors are fixed by firmware/EDID.** The standard architecture is a small
static "login head" for the greeter + a dynamic virtual monitor after login.
Both are described below.

## Why the login manager cannot have a dynamic virtual display

A Wayland virtual output (KDE's `krfb-virtualmonitor`, wlroots' heads, …) is
an object **inside** a running compositor: the compositor owns the
`wl_output`, exposes it to capture clients, and drives its modes.

At the greeter there is no user compositor yet:

- **SDDM/GDM Wayland greeters do run their own compositor**, but as the
  `sddm`/`gdm` system user, with its Wayland socket inside that user's
  runtime directory (`0700`). Wayland's security model does not let another
  user's process attach to it — so your user-level Sunshine cannot create or
  capture greeter compositor outputs.
- Running Sunshine *as* the greeter user to get around this is a security
  anti-pattern (it holds pairing credentials and runs a Web UI) and drags
  compositor environment setup into the login manager. Don't.

What *does* exist at greeter time is the DRM layer underneath: KMS
connectors with their EDID-declared modes. That is what the KMS (kmsgrab)
capture backend reads — hence the numeric display names ("0", "1") in the
client list pre-login, and hence why any pre-login display is **fixed**.

## The standard architecture: static login head + dynamic virtual monitor

### 1. After login: dynamic virtual monitor (recommended)

Once the desktop session is up, the KWin capture backend can stream a
virtual output that exists only for the duration of the stream:

1. On client connect, Sunshine runs `global_prep_cmd`; the do-hook starts
   `krfb-virtualmonitor` at the **client's requested resolution and refresh
   rate** (`SUNSHINE_CLIENT_WIDTH/HEIGHT/FPS` are exported to the hook), e.g.:
   ```bash
   krfb-virtualmonitor --resolution "${W}x${H}" --name SunshineVirt \
     --port 5910 --password "$PW" &
   ```
   Note `--resolution` is a **single** `WIDTHxHEIGHT` token — a space inside
   the quotes makes krfb silently create no output.
2. The new output appears in `kscreen-doctor -o` **disabled**; enable it
   (that is what makes the real `wl_output` global appear):
   ```bash
   uuid=$(kscreen-doctor -o | grep -oE "Virtual-SunshineVirt [0-9a-f-]+" | awk '{print $2}')
   kscreen-doctor "output.$uuid.enable"
   ```
3. Optionally add a custom mode at the client fps (on NVIDIA the mode switch
   may be rejected by the driver — Sunshine scales instead; treat failure as
   non-fatal).
4. The undo-hook kills `krfb-virtualmonitor`; the output disappears with the
   process.

Client picks map cleanly: the two virtual entries and the host's configured
`output_name` all resolve to this dynamic monitor, while physical outputs
stay directly selectable for mirror-style streaming.

### 2. Pre-login: a static "login head" (required on truly headless hosts)

If the machine has no usable monitor — dead laptop panel, headless desktop —
the greeter needs one display to render on. Two kernel-level options:

**Option A — force-enable a connector with a custom EDID** (works everywhere):

Kernel cmdline (NixOS: `hardware.display.outputs`, or `boot.kernelParams`):

```
video=HDMI-A-1:e drm.edid_firmware=HDMI-A-1:edid/headless-1080p.bin
```

- `:e` force-enables the connector even with nothing plugged in
- the EDID binary declares the modes the greeter will offer; generate one at
  a chosen resolution/refresh with `edid-decode` round-trips or tools like
  `edid-generator`
- the screen stays physically dark but SDDM renders, kmsgrab captures, and
  Moonlight's keyboard/mouse reach the greeter (active VT)

Dead-panel laptops can use this to keep the internal panel "present" without
lighting it.

**Option B — a virtual DRM device: vkms (Virtual KMS)** (cleanest for
desktops):

```
modprobe vkms enable_cursor=1
```

The kernel creates a purely virtual DRM card — no real connector is
consumed, no fake EDID for a real panel. Recent kernels support runtime
creation through configfs (new devices, custom EDIDs, multiple connectors),
so a privileged helper could in principle rebuild the login head per client
resolution. In practice the greeter only needs *a* display to type a password
into — 1080p@60 is fine — and the greeter would need to re-enumerate outputs
after any change, so a fixed mode is the pragmatic choice. Enable it
persistently at boot if you prefer it over a forced connector.

**Option C — skip the greeter entirely: autologin + desktop lock.** If your
threat model only requires protecting the session (not the boot), enable
autologin and immediately lock the desktop on login. Sunshine then streams
the *dynamic* virtual monitor for the entire session and the greeter's fixed
resolution never matters. Security is equivalent to a locked greeter (both
only stop physical passers-by); this gives the best streaming experience.

## Putting it together

1. **Greeter**: one static head — forced connector + EDID (A) or vkms (B).
   Fixed resolution is fine for typing a password; pick the EDID's preferred
   mode to taste.
2. **Session**: dynamic krfb-virtualmonitor driven by `global_prep_cmd`
   hooks; resolution/refresh follow the Moonlight client exactly.
3. **Capture routing**: with both KMS and KWin sources alive (a linger
   service boots at SDDM, KWin verifies after login), route display picks by
   name shape — alphanumeric names belong to KWin, numeric ids to KMS.
4. **Capabilities caveat**: kmsgrab needs `CAP_SYS_ADMIN` (setcap wrapper on
   NixOS). A file-capped process is non-dumpable, which breaks KWin ≥ 6.6's
   per-client `zkde_screencast` permission gate (it readlinks
   `/proc/<pid>/exe`); the documented workaround is
   `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` in both the session and service
   environments, and clearing the ambient cap in-process before spawning
   hooks (a cap-carrying krfb-virtualmonitor hangs before EGL init).
5. **uinput under a linger service**: `/dev/uinput` uaccess ACLs are applied
   by logind only while the user owns an active session — a linger service
   that starts at SDDM opens uinput once *before* that. Add a static rule
   (`KERNEL=="uinput", GROUP="input", MODE="0660"`) and put the user in
   `input`, or virtual mouse/keyboard will be dead for the service lifetime.

## FAQ

**Can Sunshine itself create the virtual display at SDDM if it runs as the
greeter user?**
Technically the KWin backend would reach the greeter compositor, but you are
now running a credential-holding stream host inside the login manager. Not
recommended for any machine you care about.

**Why does my client list show "0"/"1" before login and real names after?**
Numeric names are KMS connector indices; real names come from the compositor
enumeration once the KWin source is verified. The client list follows the
active capture source — with the routing rules above the list upgrades
itself after the first post-login re-verification.

**The virtual monitor exists but Moonlight shows nothing.**
Check it is *enabled* (`kscreen-doctor -o` — krfb-virtualmonitor creates it
disabled), that the do-hook actually ran (a skipped hook means the encoder
probe finds no display and the stream 503s), and that any cap-shedding for
the KWin path happened before the hook spawned krfb.
