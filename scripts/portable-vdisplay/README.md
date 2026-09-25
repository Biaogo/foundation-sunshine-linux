# Portable virtual-display hooks for Sunshine on Linux (Plasma / Wayland)

Sunshine on **Windows** ships a virtual display driver: pick the virtual monitor
in the client and the host creates the output for you.

On **Linux this build has no such implementation** — `src/platform/linux/`
contains only the *names* it advertises to the client (`OFFER_VIRTUAL_DISPLAY_IDS`
is a compile-time `true`), so **every** client sees “虚拟-KWin / 虚拟-KMS”
whether or not the host can actually create anything. If nothing on the host
creates the output, the session hangs: Sunshine waits up to 20 s for a display
that never appears while the client gives up after ~10 s
(`Warning: Requested display [Virtual-SunshineVirt] did not appear within 20000 ms`).

These four scripts are that “something on the host”. They are self-contained
bash — no Nix, no build step — and they also fix three problems that bite even
without the virtual display:

| Script | Role |
|---|---|
| `sunshine-vdisplay-do.sh` | creates/enables the virtual output (or makes a requested real monitor live) and binds touch to it |
| `sunshine-vdisplay-undo.sh` | restores the topology, kills the virtual output |
| `sunshine-topology.sh` | applies the session’s display-preparation mode with `kscreen-doctor`, `ensure`/`revert`/`list` |
| `sunshine-touchbind.sh` | points the touchscreen/pen device at the right output |

## Requirements

* Plasma **Wayland** session (`kwin_wayland`), running for the *same user* as Sunshine
* `kscreen-doctor` — package `kscreen`
* `krfb-virtualmonitor` — package `krfb` (only needed for the virtual options); check with
  `dpkg -L krfb | grep virtualmonitor`
* `busctl` (systemd, for the touch binding) and `pkill` (`procps`)

## Install

```bash
mkdir -p ~/.local/bin
cp sunshine-vdisplay-do.sh sunshine-vdisplay-undo.sh \
   sunshine-topology.sh sunshine-touchbind.sh ~/.local/bin/
chmod +x ~/.local/bin/sunshine-vdisplay-*.sh ~/.local/bin/sunshine-topology.sh ~/.local/bin/sunshine-touchbind.sh
```

Then add this to `~/.config/sunshine/sunshine.conf` (one line!):

```ini
global_prep_cmd = [{"do":"/home/YOUR_USER/.local/bin/sunshine-vdisplay-do.sh","undo":"/home/YOUR_USER/.local/bin/sunshine-vdisplay-undo.sh","elevated":"false"}]
```

> `"elevated":"false"` matters: an elevated hook runs outside the Wayland/DBus
> session and cannot talk to KWin at all.

Recommended companions (replace `eDP-1` with your own monitor, see
`kscreen-doctor -o` or `sunshine-topology.sh list`):

```ini
capture = kwin          # the XDG-portal capture path hangs on teardown on some systems
output_name = eDP-1     # what “默认” means; leave it empty to use the first output
max_bitrate = 50000     # optional cap in kbit/s — only needed on a CPU-only build
```

Restart Sunshine afterwards.

## Test without a client

```bash
# 1. what does the compositor offer right now?
~/.local/bin/sunshine-topology.sh list

# 2. pretend to be a session — prints every action it would take, changes nothing
SUNSHINE_VDISPLAY_DRYRUN=1 SUNSHINE_CLIENT_VIRTUAL_DISPLAY=kwin \
  SUNSHINE_CLIENT_WIDTH=1920 SUNSHINE_CLIENT_HEIGHT=1080 SUNSHINE_CLIENT_FPS=60 \
  ~/.local/bin/sunshine-vdisplay-do.sh

# 3. same for a real monitor
SUNSHINE_VDISPLAY_DRYRUN=1 SUNSHINE_CLIENT_DISPLAY_NAME=eDP-1 \
  ~/.local/bin/sunshine-vdisplay-do.sh
```

Log of the real thing: `${XDG_RUNTIME_DIR}/sunshine-vdisplay.log`
(override with `SUNSHINE_VDISPLAY_LOG`).

## Which client option to pick

* **虚拟-KWin** — works, this is the one to use.
* **虚拟-KMS** — the output is created, but capturing it through KMS needs
  `CAP_SYS_ADMIN`, which an unprivileged AppImage can never have
  (`Error: Failed to gain CAP_SYS_ADMIN`). Use 虚拟-KWin.
* **A real monitor / 默认** — no virtual output is created; the hook only makes
  the requested monitor live and binds touch to it.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Requested display [Virtual-SunshineVirt] did not appear within 20000 ms` | `krfb` not installed, not a Plasma Wayland session, or the client picked a virtual option while the hook is not configured |
| Touch lands in a corner / at the wrong spot | the touch binding points at a dead output; check for `touchbind: … -> <output>` in the log, and that the hook ran (see `global_prep_cmd`) |
| Connected, but no video, and the app dies right after disconnecting | the capture path, not the hook: set `capture = kwin`; the “Hang detected … (still waiting for the video thread)”/core-dump guard is Sunshine terminating itself |
| `Failed to gain CAP_SYS_ADMIN` | you picked 虚拟-KMS; KMS capture needs ambient capabilities — use 虚拟-KWin |

## Uninstall

```bash
rm ~/.local/bin/sunshine-vdisplay-*.sh ~/.local/bin/sunshine-topology.sh ~/.local/bin/sunshine-touchbind.sh
# and drop the global_prep_cmd line from ~/.config/sunshine/sunshine.conf
```
