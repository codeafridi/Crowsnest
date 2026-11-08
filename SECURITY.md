# Security policy

## Reporting

Use GitHub's private vulnerability reporting: Security tab on this repo → "Report a vulnerability". Do not open a public issue for anything you think is a security problem.If it's in a public issue I treat it as public knowledge and close it.

Right now there is no security email address(you could email it to code.afridi@gmail.com) though GitHub's private reporting is the main channel.

## What to include

- Version: `crowsnest --version`
- OS and how you installed it (deb / DMG / MSI / portable / AppImage / source)
- The config you ran, **with tokens redacted** — `[metrics] token=`, `[fleet] token=`, any `RELAY_KEY`. If you paste a real token I have to assume it's burned and I can't help debug with it anyway.
- Steps to reproduce. "Someone could theoretically" without steps gets a slow reply.

The relay (the server side) is closed-source. Reports about it are welcome, but I can't walk through reproduction from source the way I can for the client, so expect some back and forth.

## Supported versions

Only the latest release gets fixes. If you're on anything older the answer is "update first". Check https://github.com/codeafridi/Crowsnest/releases.

## In scope

- The C++ client and its collection surfaces: `/proc` parsing, `/metrics` endpoint (bind + token enforcement), the fleet snapshot route and peer handling, the OTLP (4318) and DogStatsD (8125) receivers, TLS checks, the Docker/kubectl integrations.
- `install.sh` and `crowsnest update` — this downloads a release over HTTPS and executes it, which is the most obvious attack surface the project has. Shortcut any part of that chain and it's a finding.
- Config parsing, especially anything that ends up in a shell command or a network request.
- The dashboard in the relay: auth, session handling, CSRF boundaries, token handling.

## Not vulnerabilities (please read before reporting)

- **Crowsnest is designed to read your machine.** It shows process names, open ports, command lines and memory usage because that is the product. "The HUD shows another process's data" is not a finding.
- A same-uid process can read other same-uid processes' data. That's how Unix works; the docs say so and the product depends on it.
- `/metrics` bound to loopback with no token is the documented default and intentional. If you bound it to `0.0.0.0` without a token, endpoint resolution downgrades to loopback on purpose but if you find a way to make it *not* downgrade, that's in scope.
- The binaries are unsigned (no Apple developer account, no Authenticode cert)(would do it in future though). Known distribution limitation, documented in the install docs.
- The link-local cloud metadata reads (169.254.169.254 etc.) happen by design, bounded and only the answers the user's own instance returns.
- Anything requiring an attacker to already have your user's privileges or your machine.

Anything needing local access to your box to exploit is only interesting if it crosses a privilege boundary the product promises to keep.

## What you get

- Acknowledgement within ~48h.
- A verdict — fix,I maintain this alone.
- Fixes land in the next release. I run coordinated disclosure and I'll tell you which release, you pick the publication window reasonable to you.
- Credit in release notes with the name or handle you want or no credit, your call.
