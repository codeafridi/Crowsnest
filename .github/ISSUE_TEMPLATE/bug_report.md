---
name: Bug report
about: Create a report to help us improve
title: ''
labels: ''
assignees: ''

---

---
name: Bug report
about: Something in Crowsnest is wrong
title: ''
labels: ['bug']
assignees: ''
---
## What happened?
A short description. If it printed an error, paste the exact text 

## How to reproduce
Steps, numbered. Include the exact command or the exact clicks. If it happened once and won't happen again, say that — one-shot reports are hard on both of us.

## What did you expect instead?
Environment
- Version: crowsnest --version (the exact string)
- OS + desktop environment (e.g. "Fedora 43, GNOME on Wayland" / "Lubuntu 25.10, X11")
- How you installed it: deb / DMG / MSI / portable / .AppImage / source. For the AppImage or portable, whether it was fetched from a release page or from latest/download
- How it runs: manually, systemd unit, LaunchAgent, autostart entry

## Output
Paste what you actually have, in this order of usefulness:
1. crowsnest --dump output (~60 lines, headless, safe to paste whole)
2. crowsnest logs (the last ~40 lines of hud.log)
3. crowsnest --probe=8 output if the problem is a missing/corrupt sample rather than a UI complaint
4. A screenshot if it's a rendering problem

## Config
The [section] blocks relevant to the bug. Redact token= values and any RELAY_KEY. If the bug disappears when you empty the config, that alone is useful so say so instead of pasting the full file.

## Anything else
Context, when it started, whether an older version worked and which one.
