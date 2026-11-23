# What is free, and what is paid

Crowsnest has one paid product: the **managed relay**. Everything else is free
and MIT.

## The desktop app

Free, MIT, no account, no telemetry, no licence check and no feature gate. Every
console view, every collector and the local alerting ship in the same build.
The app never phones home.

## The relay

The relay is the shared state: fleet history, incidents, delivery, routing,
rotas and the browser dashboard. It is also MIT. You can run it yourself:

- one static binary and a Postgres 16 database;
- every feature is available, with no signed licence, no trial timer, no
  expiry and no phone-home;
- retention and cadence are your configuration, and the plan limits
  (100 machines, 10 seats, 25 share links, a 15 s cadence floor) are the same
  ones the hosted service applies;
- self-hosting is community-supported while the hosted product is being built.

## The managed relay

The paid product is the same relay, operated by Crowsnest: hosting, TLS,
upgrades, backups, the mail transport and support. $9 per month or $90 per year
($7.50/month), 14-day trial, no card. Managed hosting is not open yet.

A managed trial that ends without a subscription becomes read-only and keeps
its data. Only a managed deployment arms that clock (`billing.managed = true`);
a relay you run yourself never expires.

## Why not a signed licence?

A licence key would have to be enforced by the same MIT binary a self-hoster
runs, so it could be patched out by anyone who wanted to - and it would punish
the honest operator who just wants their own fleet's state on their own
Postgres. We would rather sell the part that actually costs money to provide:
running the service. The relay stays free, the client stays free, and the
managed relay is the convenience you can buy.

## What the price is not

- Not a per-machine or per-seat meter on local telemetry.
- Not a gate on the desktop app; `[remote]` is in the free build, off by
  default.
- Not a data hostage: export, delete and the deletion receipt work on both the
  self-hosted and managed relay.
