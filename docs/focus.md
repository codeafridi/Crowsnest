# Focus tracking

The strip and every per-app card follow the window you are using. This page is
how that works, how to set it up, and what the status line means when it does
not.

## How it is detected

One provider is selected for the session; there is no polling of every window.

| Session | Provider | What it reads |
|---|---|---|
| GNOME on Wayland | `gnome-pidfile` | the extension writes `/run/user/<uid>/crowsnest-active-pid` on `notify::focus-window` |
| KDE Plasma on Wayland | `kde-dbus` | the KWin script calls `org.crowsnest.Focus.setFocusedPid` over the session bus |
| Hyprland | `hyprctl` | `hyprctl activewindow` |
| Sway | `swaymsg` | the focused container's pid over the Sway socket |
| niri | `niri` | the compositor's focused window event |
| X11 (any desktop) | `x11-native` | EWMH `_NET_ACTIVE_WINDOW` + `_NET_WM_PID`; no xdotool needed |
| macOS | `mac-nsworkspace` | `NSWorkspace.frontmostApplication` (pid-only fallback) |
| Windows | `windows-getforegroundwindow` | `GetForegroundWindow` + the process id |

GNOME and KDE on X11 use the native X11 provider. GNOME and KDE on Wayland
need the bridge below. A compositor with no supported surface reports no focus
and the host cards still fill (the M58 no-starvation rule).

## Setup

The one-liner installer detects GNOME/KDE and installs the bridge when it can.
The app can install or repair it at any time, from the same bundled payload:

```bash
crowsnest focus-bridge
```

or **Settings -> Collection -> FOCUS -> SET UP FOCUS BRIDGE**.

- **GNOME**: copies `metadata.json` + `extension.js` to
  `~/.local/share/gnome-shell/extensions/crowsnest-focus@codeafridi`, appends
  the UUID to `org.gnome.shell enabled-extensions`, and runs
  `gnome-extensions enable` when available. GNOME loads new extensions only at
  login: **log out and back in** once.
- **KDE**: copies the KWin script to
  `~/.local/share/kwin/scripts/crowsnest-focus`, installs it with
  `kpackagetool6` (or 5), sets `Plugins/crowsnest-focusEnabled=true` in
  `kwinrc`, and asks KWin to reconfigure. No relogin is normally needed.
- **X11**: nothing is installed; the native provider is used.

The command is idempotent and never needs root. It prints what it did. A
`.deb` install carries the payload at `/usr/share/crowsnest/bridge`; a portable
bundle or AppImage carries it beside the binary.

## The PIN button

**PIN** in the strip holds the current app as the subject while focus moves.
Useful for a full-screen game, a VM window, a compile, or a second monitor you
are not touching. It becomes **UNPIN**.

- The pin is a `(pid, start time)` pair, so a recycled pid cannot inherit it.
- The pinned process exiting (or the identity changing) drops the pin and live
  focus resumes; the next tick shows the live app.
- The pin is session-only, not written to `config.ini`.
- `[remote]` reports the pinned app exactly as it reports a focused one.

## The status line

Settings -> Collection -> FOCUS and `crowsnest --dump` both carry the same
sentence:

| Text | Meaning |
|---|---|
| `focus  live via <provider> - <app> (pid N)` | working; the provider names the source |
| `focus  pinned to <app> (pid N) - UNPIN to follow focus again` | a pin is active |
| `focus  off - GNOME bridge not installed (run: crowsnest focus-bridge)` | copy + enable the bridge |
| `focus  off - GNOME bridge installed but not loaded (...)` | log out and back in |
| `focus  off - KDE bridge not installed (...)` | copy + enable the KWin script |
| `focus  off - KDE bridge installed but no window reported (...)` | check the KWin script is enabled; then re-run the command |
| `focus  off - no focused X11 window` | the X11 provider is working, nothing is focused |
| `focus  off - no focus provider for this session` | an unsupported compositor; host cards still fill |

`crowsnest --dump` also prints `provider`, `available`, `bridgeInstalled`,
`desktop`, `sessionType` and four deterministic self-tests
(`listParseOk`, `listTextOk`, `statusGateOk`), so a bug report can tell the
provider apart from the desktop.

## Privacy

The bridge sends one integer: the focused window's pid. The GNOME extension
writes it to a file in your own runtime directory; the KWin script calls a
D-Bus method on the running app. No window titles, paths, command lines or
keystrokes are read, and nothing leaves the machine.

## Troubleshooting

- **`--` in the App column on GNOME Wayland**: run `crowsnest --dump`; if
  `bridgeInstalled` is false, run `crowsnest focus-bridge` and log out/back in.
  If it is true, the shell has the old extension loaded: log out and back in.
- **After a GNOME upgrade**: extensions can be disabled. Re-run
  `crowsnest focus-bridge`; it appends the UUID back to enabled-extensions.
- **KDE shows `no window reported`**: open System Settings -> Window Management
  -> KWin Scripts and confirm "Crowsnest Focus Bridge" is checked, or re-run
  `crowsnest focus-bridge`.
- **X11**: `xdotool` is not required; if no window is focused, the provider
  correctly reports none. The host cards keep updating.
- **The HUD itself is clicked**: the overlay never becomes the subject; the
  sampler keeps the last real app instead.
