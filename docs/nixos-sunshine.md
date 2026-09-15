# Sunshine on NixOS — KDE Wayland deployment notes

> **Scope: KDE Plasma (Wayland) only.** The dynamic virtual display and the
> KWin capture path described here depend on KWin and KDE tooling
> (`krfb-virtualmonitor`, `kscreen-doctor`, the `zkde_screencast` protocol).
> GNOME/wlroots-based compositors have different mechanisms (Mutter virtual
> monitors, `wlr-export-dmabuf`) and are **not covered**.
>
> A private-config version of this page lives in a private NixOS repo; this
> document is the distribution-neutral copy. Snippets referencing module
> files are illustrative — adapt paths to your own config layout.

How to run [Foundation Sunshine](https://github.com/Biaogo/foundation-sunshine-linux)
(the qiin2333 fork of LizardByte/Sunshine, `linux-support` branch) as a
systemd **user service** on NixOS with KDE Plasma, including a dynamic
virtual display for headless / phone-as-second-screen streaming.

## Building the package

Use the flake:

```nix
# flake.nix
{
  inputs.foundation-sunshine-linux-nix.url = "github:Biaogo/foundation-sunshine-linux.nix";

  outputs = { self, nixpkgs, foundation-sunshine-linux-nix, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [ ./configuration.nix ];
    };
  };
}
```

```nix
# configuration.nix
{ pkgs, ... }: {
  environment.systemPackages = [
    foundation-sunshine-linux-nix.packages.x86_64-linux.default
  ];
}
```

(Or import `foundation-sunshine-linux-nix.overlays.default` and use
`pkgs.foundation-sunshine` anywhere.)

If you pin the source yourself instead, keep the rev **on the fork's
`v<version>-linux` tag** so your hash stays comparable with the flake and
the release tarballs. Bumping: set `hash = lib.fakeHash;`, run the build,
copy the `got:` hash back.

## System-level NixOS module pieces

```nix
# configuration.nix (system side)
{ pkgs, lib, ... }: {
  # kmsgrab needs CAP_SYS_ADMIN: setcap wrapper, service ExecStart points here
  security.wrappers.sunshine = {
    owner = "root";
    group = "root";
    capabilities = "cap_sys_admin+p";
    source = lib.getExe pkgs.foundation-sunshine;
  };

  # uinput ownership independent of session timing (see below)
  services.udev.extraRules = ''
    KERNEL=="uinput", SUBSYSTEM=="misc", GROUP="input", MODE="0660"
  '';

  # Sunshine default ports + mDNS
  networking.firewall.allowedTCPPorts = [ 47984 47989 47990 48010 ];
  networking.firewall.allowedUDPPorts = [ 47998 47999 48000 48002 48010 5353 ];
  services.avahi.enable = true;
  services.avahi.openFirewall = true;
}
```

## Home-manager user service (linger-capable)

```nix
# home module
{ pkgs, lib, ... }: {
  systemd.user.services.sunshine = {
    Unit = {
      Description = "Foundation Sunshine game stream host";
      # default.target + linger: the service boots at SDDM, BEFORE any
      # graphical-session target exists (graphical-session.target is not
      # active pre-login)
      After = "network.target";
    };
    Service = {
      ExecStart = "/run/wrappers/bin/sunshine"; # the setcap wrapper
      Environment = [
        "WAYLAND_DISPLAY=wayland-0"
        "QT_QPA_PLATFORM=wayland"
        # KWin permission gate workaround — see below. KWin reads its own
        # session environment; ALSO set it as a session variable, e.g.
        # environment.sessionVariables.KWIN_WAYLAND_NO_PERMISSION_CHECKS = "1";
        "KWIN_WAYLAND_NO_PERMISSION_CHECKS=1"
      ];
      Restart = "on-failure";
      RestartSec = 5;
    };
    Install.WantedBy = [ "default.target" ];
  };

  # krfb-virtualmonitor + kscreen-doctor for the virtual display hooks
  home.packages = with pkgs.kdePackages; [ krfb libkscreen ];
  xdg.dataFile."applications/org.kde.krfb.virtualmonitor.desktop".source =
    "${pkgs.kdePackages.krfb}/share/applications/org.kde.krfb.virtualmonitor.desktop";
}
```

Enable linger so the service runs pre-login:

```bash
loginctl enable-linger $USER
```

## Capture backends and how sources decide

- **KWin ScreenCast** (`SUNSHINE_ENABLE_KWIN`, on by default in this build):
  in-session. Connects to the compositor directly; can capture
  krfb-virtualmonitor virtual outputs by name.
- **KMS / kmsgrab**: pre-login (SDDM) via the setcap wrapper. Understands
  NUMERIC ids only ("0", "1") — it enumerates DRM connectors.
- Source selection runs once at service start. With linger the start happens
  at SDDM where only KMS verifies; a re-verify pass enables KWin as soon as a
  Wayland session appears (`Wayland session detected` in the log).
  Display-name routing keys on the name shape: alpha-numeric names → KWin,
  purely numeric ids → KMS.

## The KWin permission gate (important)

KWin ≥ 6.6 gates `zkde_screencast_unstable_v1` per client by readlinking
`/proc/<pid>/exe` and matching it against `Exec=` of desktop files carrying
`X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1`. A setcap-wrapped
sunshine is **non-dumpable** (its `/proc/<pid>/exe` is unreadable even to its
own uid), so KWin can never identify it → `zkde_screencast_unstable_v1 not
found in registry` → every virtual pick 503s. `PR_SET_DUMPABLE` at runtime
does **not** restore readability for the wrapper case.

Fix: `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` set BOTH as a session variable
(what KWin itself reads — e.g. `environment.sessionVariables` on NixOS) and
in the service `Environment=` (Sunshine then skips its own
permission-desktop-file dance). This is upstream Sunshine's documented
workaround. Takes effect only after **re-login** (session vars).

## Dynamic virtual display (krfb-virtualmonitor)

The client picks a virtual display; hooks create it on connect and destroy
it on disconnect. Wire it through Sunshine's `global_prep_cmd`
(sunshine.conf):

```json
"global_prep_cmd" = [{ "do": "~/.local/bin/sunshine-vdisplay-do.sh",
                       "undo": "~/.local/bin/sunshine-vdisplay-undo.sh",
                       "elevated": "false" }]
"output_name" = "Virtual-SunshineVirt"
```

The do-hook, in essence (full lifecycle: start krfb at the client's
resolution/fps → wait → enable → custom mode):

```bash
#!/usr/bin/env bash
# Sunshine exports the client's request to prep-cmd hooks:
W="${SUNSHINE_CLIENT_WIDTH:-1920}"; H="${SUNSHINE_CLIENT_HEIGHT:-1080}"
FPS="${SUNSHINE_CLIENT_FPS:-60}"

# Skip pre-login: no compositor of ours is running, krfb would SIGABRT
# against no Wayland display and drkonqi turns every crash into a popup.
pgrep -u "$(id -u)" -f "/kwin_wayland" >/dev/null || exit 0

pkill -f "krfb-virtualmonitor.*5910"; sleep 0.3
krfb-virtualmonitor --resolution "${W}x${H}" --name SunshineVirt \
  --port 5910 --password sunshine-vm &   # NOTE: single WxH token, NO space

# krfb creates the output DISABLED; enabling is what makes the wl_output
# global appear — poll for it, then enable:
for i in $(seq 1 60); do
  U=$(kscreen-doctor -o 2>/dev/null | grep -oE "Virtual-SunshineVirt [0-9a-f-]+" | awk '{print $2}')
  [ -n "$U" ] && break; sleep 0.5
done
[ -n "$U" ] && kscreen-doctor "output.$U.enable"
# Client-fps custom mode (NVIDIA may reject the switch; Sunshine scales instead)
kscreen-doctor "output.$U.addCustomMode.$W.$H.${FPS}000.full" 2>/dev/null
kscreen-doctor "output.$U.mode.${W}x${H}@$FPS" 2>/dev/null
```

```bash
#!/usr/bin/env bash
# undo-hook: kill krfb; the output disappears with the process
pkill -f "krfb-virtualmonitor.*5910"
```

Hook gotchas learned the hard way:

- `--resolution` is a **single** `WIDTHxHEIGHT` token — `"$W x$H"` passes two
  arguments and krfb silently creates nothing (every virtual pick 503s after
  Sunshine's 20 s wait).
- Pin the hook's PATH (a systemd user service's default PATH has no
  coreutils on NixOS) or use absolute store paths.
- The hook env: Sunshine exports `SUNSHINE_CLIENT_WIDTH/HEIGHT/FPS` to prep
  commands.
- `/dev/uinput` static udev rule (see the system module above) — uaccess
  ACLs are session-timed and miss a linger service that opens uinput once at
  SDDM, which kills virtual mouse/keyboard for the service's lifetime.

## Pre-login (SDDM) streaming

The greeter cannot have a dynamic virtual display (compositor ownership +
Wayland socket isolation; running Sunshine as the greeter user is a security
anti-pattern). Pre-login the KMS backend can only stream a *connector* with
boot-time-fixed modes — force-enable one with a custom EDID, or use a `vkms`
virtual DRM device, or skip the greeter with autologin + desktop lock. Full
walkthrough: [linux-headless-sddm-streaming.md](linux-headless-sddm-streaming.md).

## Deploy checklist

1. Build/bump the package (fakeHash round-trip if pinning the source).
2. `sudo nixos-rebuild switch`.
3. `systemctl --user restart sunshine` (or re-login if the plasma session
   variables changed — KWin only reads them at session start).
4. Verify: no `not found in registry`, `Screencasting with KWin ScreenCast`
   in-session, do-hook ran (its own debug log if you added one),
   the virtual output appears in `kscreen-doctor -o` only while streaming
   and is gone after disconnect.
