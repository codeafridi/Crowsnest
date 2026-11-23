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

Check the version strip says the release being shipped (`CROWSNEST 0.13.8` at
the time of writing), the `hud.png` capture shows the current strip (PIN,
MORE, CLICK, HIDE), and `shots/views.webp` is the current 34-view contact
sheet.

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

- Open the live URL and hard-reload; confirm the top strip says `0.13.8`.
- Click **Releases**, **Relay**, the install tabs and the copy buttons; confirm
  `releases/latest/download/install.sh` and `SHA256SUMS` resolve.
- Confirm the hero `hud.png` has the PIN button and `shots/views.webp` opens to
  the 34-view sheet.
- If an old image is still served, purge the Cloudflare cache for
  `/hud.png`, `/shots/views.webp`, `/shots/overview.webp` and
  `/shots/processes.webp` (or append a cache-busting query to the four
  references in `index.html`).
- The relay capture (`shots/relay-fleet.webp`) must show the watchroom design,
  not the old console.

## What must not ship

- A version strip older than the latest release.
- A `views.webp` with fewer than 34 tiles.
- A dead link to a screenshot, the release page, the relay page or the install
  command.
