# Cloud page: identity, cost and footprint

The **Cloud** page (rail 29) answers three questions about the machine it is
running on:

1. *Which instance is this, and how old is it?*
2. *What does this app cost me?*
3. *What does Crowsnest itself send out to answer 1 and 2?*

The third one is on the page too, in the **CLOUD FOOTPRINT** card, because a
monitoring tool that phones home quietly is exactly what people install a local
monitor to avoid.

## Identity (link-local metadata)

Crowsnest reads the provider's own instance metadata document. The read goes to a
link-local address, which is never routed off the host, and only when
`[cloud] enabled` is true (the default).

| Provider | Endpoint | Header / auth |
|---|---|---|
| GCP | `169.254.169.254/computeMetadata/v1/instance/{id,name,machine-type,zone}` | `Metadata-Flavor: Google` |
| OCI | `http://169.254.0.23/opc/v2/instance/{id,display-name,shape,canonical-region-name,…}` | `Authorization: Bearer Oracle` |
| Azure | `169.254.169.254/metadata/instance/compute?api-version=2021-12-13&format=json` | `Metadata: true` |
| AWS | `169.254.169.254/latest/api/token` (IMDSv2 `PUT`), then `/latest/dynamic/instance-identity/document` | `X-aws-ec2-metadata-token` |

The probe order is GCP → OCI → Azure → AWS because each service answers only to
its own header: a provider that is not present refuses the connection (or
answers 4xx), so the sweep costs a refused connect, not a false positive. AWS
falls back to the unauthenticated document when IMDSv2 tokens are refused.

The whole sweep shares **one** budget, `[cloud] timeout_ms` (default 450 ms,
clamped 120–5000). It is shared, not per-request, because a host that
*blackholes* `169.254.169.254` instead of refusing it would otherwise pay the
budget five times inside a single tick. The sweep also starts on the **second**
tick, so the first sample is never network-bound; the card says so for that one
tick.

Cadence: every **5 minutes** once an instance has been identified, every
**15 minutes** while no provider answers. A gated host therefore pays one
refused sweep per quarter hour.

Cloud identity is nothing without a price, and a price has to come from
somewhere. Crowsnest does not invent or bundle one — a stale list price shown as
current is worse than no number at all.

### Instance age and the account inventory

Only the provider knows when this instance launched and what else is in the
account, so Crowsnest asks the provider's own CLI and reuses **your** login:

```sh
aws    ec2 describe-instances --region <imds region> --output json \
       --query "Reservations[].Instances[]"
az     vm list -d --subscription <imds account>
gcloud compute instances list --project <imds project> --format=json
oci    compute instance list --compartment-id <imds compartment> \
       --region <imds region> --all --output json
```

This is a *listing* call: it reads what exists, it never changes anything.
Polled every 10 minutes by a background runner with its own budget
(`[cloud] inventory_timeout_ms`, default 25000, clamped 2000–120000), so no
tick ever waits on it — the sampler publishes the last completed listing and
says `listing in progress` / `stale` while a refresh is in flight.

`[cloud] inventory=false` disables it entirely: no CLI is spawned at all.

#### AWS: the whole account, not one region

`ec2 describe-instances` is region-scoped, so a single call sees only the
region the metadata service named. Starting an instance in another region would
never show up. For AWS the listing is therefore a small plan:

```sh
aws sts get-caller-identity --output json          # which account
aws configure get region                           # the CLI's home region
aws ec2 describe-regions --output json \
    --region <home>                                # every enabled region
aws ec2 describe-instances --region <each> --output json \
    --query "Reservations[].Instances[]"           # one call per region
```

- `[cloud] identity=auto` runs the first two steps and takes the account and
  region from **your CLI login**. That is what makes the page work on a laptop
  — or on a host whose link-local address is blocked — where the metadata
  service answers nothing. On a real instance the metadata answer always wins;
  `identity=auto` only fills in what it left empty.
- The region list defaults to the **enabled** regions (the CLI's own default;
  `--all-regions` is deliberately not passed).
- If IAM allows `DescribeInstances` but denies `DescribeRegions`, the listing
  falls back to the home region and the card says exactly why it lists one.
- One region failing never hides the rest: the summary names the regions that
  did not answer.
- `[cloud] region_concurrency` (default 4, clamp 1–8) is how many
  `describe-instances` calls run at once — each `aws` start is 1–2 s of Python
  and an account has ~30 regions.
- `[cloud] account_timeout_ms` (default 90000, clamp 10000–300000) is the
  whole-plan budget, separate from one call's `inventory_timeout_ms`.
- **CHECK NOW** on the FLEET INVENTORY banner runs the plan immediately, and
  the line above the button states exactly which commands it will run and that
  no Cost Explorer request (which is billable) is part of it. Opening the Cloud
  page also refreshes the listing once if it is older than a minute; while the
  page stays open the full sweep runs every 5 minutes instead of 10.
- **A state change does not wait for the next sweep.** While the Cloud page is
  open, every 10 s Crowsnest re-lists **only the regions that already have a
  row** (normally one `aws ec2 describe-instances` call) and replaces those
  rows, so a stop, start or terminate done in the AWS console shows up within
  seconds. A region whose call fails keeps its last known rows rather than
  vanishing, and a complete account sweep still runs on the 5/10-minute cadence
  to discover instances in regions you have never used.
- **Selecting an instance** shows everything the provider reported about it:
  id, name, type with its vCPU/memory, region/zone, state, launch time and age;
  the AMI and **the distribution** (the image's own description — "Amazon Linux
  2023 AMI …", "Ubuntu 22.04 LTS …"), architecture, platform and
  virtualization; key pair, the IAM role **name**, security groups; VPC,
  subnet, private and public address and both DNS names; the root disk with its
  size and type; tenancy, monitoring, **IMDSv2** state (`HttpTokens`), CPU
  credit mode and tags. Everything except the image name, the type spec and the
  root volume size comes from the same `describe-instances` answer the table
  already uses. Those three are one extra read-only call each per listing
  (`ec2:DescribeImages` per region with rows, `ec2:DescribeInstanceTypes` once,
  `ec2:DescribeVolumes` per region with rows) and are cached, so a second
  listing costs none. If IAM denies one, only that field shows `--`.
- **A refresh never blanks the table**: the previous answer stays on screen
  while the new one is being read, with the note saying `listing N region(s) -
  M found so far`. Collapsing and reopening the console therefore keeps the
  rows instead of showing an empty card until the next poll.

The CLI itself is resolved like a login shell would: `PATH` first, then the
directories the providers install into (`/opt/homebrew/bin`,
`/usr/local/aws-cli/v2/current/bin`, `/snap/bin`,
`~/google-cloud-sdk/bin`, `%LOCALAPPDATA%\Programs\Amazon\AWSCLIV2`, …). A
LaunchAgent or a systemd user unit inherits a narrow `PATH`, so without this an
`aws` that works in your terminal was reported as missing.
`CROWSNEST_CLI_DIRS` (a `PATH`-style list) prepends explicit directories.


## How the page is laid out

One column, read top to bottom, in the order the questions are asked:

| section | what is in it |
|---|---|
| the four tiles | provider, the age of the instance the page is describing, its accrued cost and its monthly rate |
| **ACCOUNT** | the CHECK NOW line, the inventory summary beside the running-instances chart, the instances table, and the selected instance's detail in two columns underneath |
| **THIS HOST** | the metadata summary and the instance detail, side by side |
| **COST MODEL** | the rate/accrual/projection card beside the focused app's share, then the app's cost chart and its top processes |
| **BILLED SPEND** | month to date beside the daily chart, then the query detail and the per-service split |
| **CLOUD FOOTPRINT** | every outbound action, its cadence and the two guarantees |
| **IAC DRIFT** | only when `[cloud] terraform_state` is set — otherwise the section is not rendered at all |

## Billed spend (what the provider actually charged)

The estimate above is a rate × time projection. **Billed spend** is the other
number: what the provider says it charged the account this month.

| Provider | Query | Metric |
|---|---|---|
| AWS | `aws ce get-cost-and-usage` (daily, grouped by `SERVICE`) then `aws ce get-cost-forecast` (month to end) | `UnblendedCost` |
| Azure | `az consumption usage list --top 1000 --filter "properties/usageStart ge '<month start>'"` | `pretaxCost` |
| GCP | — | gate: Google bills through a BigQuery export, so there is no CLI cost API |
| OCI | — | gate: the OCI usage API is not wired yet |

The card shows month to date, the month-end projection (billed + forecast for
the remaining days), a daily chart and the per-service split with each service's
share. Where the provider flags a period as *estimated*, the card says so; where
the query hits Azure's 1000-row page cap, the total is labelled a floor.

Three properties are deliberate:

- **The window is UTC.** Cost Explorer's `TimePeriod` is a UTC date and Azure's
  `usageStart` is UTC, so the month is computed in UTC — a machine in IST and a
  machine in UTC ask for the same window.
- **The query is billable.** AWS charges for Cost Explorer requests, so it runs
  every 6 hours, is separately switchable (`[cloud] spend=false`), separately
  budgeted (`spend_timeout_ms`, 5000–180000), and is named in the CLOUD
  FOOTPRINT card next to the metadata read.
- **It is polled, never awaited.** The sampler publishes the last completed
  answer; no tick waits on a CLI.

## Several AWS accounts (`[cloud] profiles`)

An account is what `sts get-caller-identity` names, and a laptop is usually
logged into one of them. A developer with prod/staging/sandbox accounts lists
them:

```ini
[cloud]
profiles = prod:work, staging:work-staging, sandbox:default/eu-west-1
#           ^label  ^aws --profile    (optional region pin)
```

Each profile runs the same sequence with `--profile <name>`: `aws sts
get-caller-identity`, `aws configure get region`, `aws ec2 describe-regions`,
then one `describe-instances` per region. Profiles run **one after another** -
three accounts at once is 3 x 18 `aws` spawns on one laptop - and the listing
budget is per account, so a slow account cannot kill the others.

- The fleet table gains an `ACCOUNT` column (the label you wrote) and the
  summary carries one `profile <label> (<account id>) - N row(s)` line each.
- **Self** is matched by instance id first; name and private address are
  compared only inside the account that owns them, because `web-01` and
  `10.0.0.5` exist in prod, staging and sandbox at the same time.
- A logged-out profile (`Unable to locate credentials`, an expired `aws login`
  or SSO session) reports its own line; the other accounts still list.
- A pinned region (`sandbox:default/eu-west-1`) lists that account in that
  region only, and the scope says so.
- Cross-account access is the CLI's job: a profile with `role_arn` +
  `source_profile` works because the CLI assumes it. Crowsnest passes the name,
  never calls `assume-role` itself, and still only lists.
- `[cloud] identity=` is the single-account fallback; with `profiles` set the
  profiles win and the summary states that the override was ignored.

## Desired capacity and tag hygiene (`[cloud] asg`, `required_tags`)

The listing answers "what exists"; the orchestrator answers "what should". One
extra read-only call per listed region asks `aws autoscaling
describe-auto-scaling-groups`, and the `CAPACITY (AUTO SCALING)` table pairs the
two:

```
GROUP        REGION      DESIRED  SEEN  RUNNING
web-prod     eu-west-1         3     2        2
batch-nightly ap-south-1       0     0        0
```

`SEEN` is how many of the group's members this listing actually saw, joined by
the `aws:autoscaling:groupName` tag every member carries. A group at 2/3 is a
**launch or AZ problem, not an application problem**, and the cell is red so it
reads that way. `DESIRED 0` is a reading (scaled to zero), never an outage.

```ini
[cloud]
asg           = true          ; one describe-auto-scaling-groups per region
required_tags = team, env     ; at most 8, keys are case-sensitive
```

`TAG COVERAGE` audits the instances the listing already parsed against those
keys: a missing key, a **key with a different case** (`Team` is not `team`; AWS
treats tag keys case-sensitively) and a tag with an **empty value** all count as
missing. A stopped instance still counts, because its disks are still billed.
The card names the count per key and the offender instances, capped at eight
names.

The audit deliberately does **not** use the Resource Groups Tagging API:
`aws resourcegroupstaggingapi get-resources help` states "GetResources does not
return untagged resources", so an audit built on it could only see the
instances that already pass. The ground truth is `describe-instances`, which
lists everything and carries the tags.

## Spend per team (`[cloud] cost_group`)

The month-to-date total answers "how much"; the next question is "on what". One
more Cost Explorer call splits the same window by a tag:

```ini
[cloud]
cost_group = team     ; any tag key the account uses; empty = no grouped call
```

The `BY TAG` table shows each value with its share of the window and, when
`[cloud] profiles` lists several accounts, the account it belongs to (the
second group is `LINKED_ACCOUNT`, so `payments` in prod and `payments` in
staging stay two numbers). Cost Explorer allows at most two groups per call.

- Untagged spend arrives under **the provider's own label** (for example
  `No team`) and is rendered verbatim, never renamed. It is the money version
  of the tag audit: the two agree because both read the same tag.
- The per-instance `$/MO` stays an **estimate** (rate x uptime); this table is
  **billed**. The two are never blended.
- Cost Explorer data lags up to a day, and the grouped query is **billable**
  like the other two - which is why the split is opt in rather than on by
  default. A failed grouped call costs the split, never the bill: the card
  names the reason and the month-to-date above it is unaffected.

## Cost

- **Rate file.** `~/.config/crowsnest/cloud-rates.txt` (override with
  `[cloud] rates=`):

  ```ini
  # imported 2026-10-28
  aws/c7g.2xlarge       = 0.2920
  azure/Standard_D2s_v3 = 0.0960
  ```

  A bare `c7g.2xlarge` matches when exactly one provider lists it. The
  `# imported` stamp is load-bearing: past 30 days the page and the Settings
  row append `(N days old)`.

- **Importing real prices.** `crowsnest --cloud-prices[=<type>] [--region=]
  [--rates=]` shells out to the `aws` CLI you already use
  (`aws pricing get-products --service-code AmazonEC2`, filtered to the shared
  / used-capacity / Linux / no-preinstalled-software on-demand SKU) and
  rewrites the rate file with a fresh stamp. It is opt-in, runs **on demand
  only** — never from a tick — and Crowsnest never reads or stores credentials:
  the CLI uses the profile you already configured. With no account, it prints
  the CLI's own refusal (`NoCredentials (exit 253)`) instead of guessing a
  price.

- **Accrued cost** is `rate × uptime since this boot`. The page says *this
  boot* and never *instance age*, because a reboot resets host uptime. `AGE`
  and *since launch (upper bound)* come from the provider's launch time and are
  only shown when the inventory answered.

- **Monthly projection** is the standard 730-hour month.

- **App share** is `rate × app cpu% / vCPU`. The CPU number is per-core on
  purpose: a fully busy 8 vCPU box spends the whole rate. `TOP PROCESSES IN
  THE FOCUSED APP` ranks the focused app's own process tree.

## What leaves the host (CLOUD FOOTPRINT)

The Cloud page renders this card from the live configuration, so it can never
drift from what the sampler is actually doing:

```
metadata   169.254.169.254 (aws/azure/gcp) + 169.254.0.23 (oci), link-local only, read only
cadence    every 5 min while an instance is identified, every 15 min otherwise
budget     450 ms for the whole sweep, last 62 ms / 4 request(s)
inventory  aws (/opt/homebrew/bin/aws) every 10 min, 25000 ms budget - 30 aws
           regions in one listing - the CLI's own login, no credential is read
prices     on demand only: `crowsnest --cloud-prices` asks the AWS Price List API
           with your aws(1) credentials, never from a tick
rates      /home/you/.config/crowsnest/cloud-rates.txt
writes     none - no credential is read or stored, no instance is ever
           started, stopped or terminated
```

| Action | Destination | When | Can it be turned off |
|---|---|---|---|
| IMDS identity read | `169.254.169.254`, `169.254.0.23` | every 5/15 min | `[cloud] enabled=false` |
| Account listing | the provider CLI, to the provider API | every 10 min (5 min while the Cloud page is open, and once when it opens) | `[cloud] inventory=false` |
| Price import | AWS Price List API via `aws` | only when you run `--cloud-prices` | it never runs otherwise |

There is no other outbound path in the cloud feature — no telemetry, no
licence check, no update ping.

## Gates and honesty

- Off a cloud instance, or where IMDS is firewalled or IMDSv2 tokens are
  refused, every card prints the reason instead of an empty box.
- A credential-less or logged-out CLI prints the exact refusal from the CLI and
  the fix (`aws configure`, `az login`, `gcloud auth login`,
  `oci setup config`).
- A row the rate file cannot price is counted as `unpriced`, never `$0`.
- A stopped or deallocated instance never claims a `$/MO`.
- Azure's listing carries no creation time, so Azure rows show `--` age instead
  of a fabricated one.
- Cloud systems are **read-only**: Crowsnest identifies, prices and lists. It
  never starts, stops, terminates or resizes anything.

## Verification

CI runs both paths on every push:

- the ubuntu and windows runners are cloud VMs, so the **Azure** identity path
  is exercised against a **real** metadata service (`cloud ok: azure
  Standard_D4ads_v5 westus`), and the CLI gate path runs with no credentials
  (`az login`);
- the macOS runner has no IMDS at all, so it proves the gated path and the five
  deterministic self-tests in `--dump` (`rateLookupOk`, `costMathOk`,
  `shareOk`, `topOk`, `staleOk`, plus the fleet equivalents);
  the maths is asserted even where the network is not.

A real EC2/GCE/OCI/Azure box with a logged-in CLI is the remaining manual
check: compare `IDENTITY` with `aws ec2 describe-instances`, run
`crowsnest --cloud-prices`, and confirm the accrued figure tracks uptime.
