# Deploy the site

`site/` is a static directory: no build step, no framework, no external
assets. Cloudflare Pages (or any static host) serves it as-is.

## Before uploading

From the repository root:

```sh
python3 site/check_links.py          # expects: 2 page(s), 0 broken
```

Open `site/index.html` and `site/relay.html` at 1440 and 390 widths and check
there is no horizontal scroll. The page is dark-only by design.

Check the version strip says the release being shipped (`CROWSNEST 0.14.3` at
the time of writing), the hero plays the live strip capture (`shots/hud-live.webm` / `.mp4`,
poster `hud.png`, which shows PIN, CONSOLE, CLICK-THRU and HIDE), the console
plate plays `shots/console-live.webm` / `.mp4`, and `shots/views.webp` is the
current 34-view contact sheet.

## Upload

Cloudflare dashboard: **Workers & Pages -> Create -> Pages -> Upload assets**,
project `crowsnest`, directory `site/`.

Or with Wrangler (the project is yours to name):

```sh
npx wrangler pages deploy site --project-name crowsnest
```

The site is not deployed by GitHub Actions: the client CI ignores `site/**`,
and Cloudflare Pages is a manual step.

## After uploading

- Open the live URL and hard-reload; confirm the top strip says `0.14.3`.
- Click **Releases**, **Relay**, the install tabs and the copy buttons; confirm
  `releases/latest/download/install.sh` and `SHA256SUMS` resolve.
- Confirm the hero video plays (the strip updates about once a second) and the
  poster shows PIN/CONSOLE; `shots/console-live` plays the Overview capture;
  `shots/views.webp` opens to the 34-view sheet.
- If an old image or video is still served, purge the Cloudflare cache for
  `/hud.png`, `/shots/hud-live.*`, `/shots/console-live.*`,
  `/shots/console-live-poster.webp`, `/shots/processes-live.webp`,
  `/shots/views.webp` and `/shots/relay-checks.webp` (or append a cache-busting
  query to those references in `index.html`).
- The relay captures (`shots/relay-fleet.webp` and `shots/relay-checks.webp`)
  must show the watchroom design, not the old console; the checks plate shows
  HTTP, TLS and push probes with 24 hour bars and certificate days.

## What must not ship

- A version strip older than the latest release.
- A `views.webp` with fewer than 34 tiles.
- A dead link to a screenshot, the release page, the relay page or the install
  command.
