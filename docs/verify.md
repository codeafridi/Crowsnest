# Verify a release

Every Crowsnest release has two layers of integrity:

1. `SHA256SUMS` lists the SHA-256 of every published asset.
2. `SHA256SUMS.sigstore.json` is a Sigstore bundle proving that manifest was
   signed by the release workflow at that tag. There is **no release key** to
   fetch, rotate or trust: the signer certificate is short-lived and bound to
   GitHub's OIDC identity for
   `.github/workflows/release.yml@refs/tags/<tag>`.

The DMG/MSI installers themselves are still not Apple/Microsoft-signed; see
[the end of this page](#what-is-not-signed).

## The short version

Install [cosign](https://docs.sigstore.dev/cosign/installation/), then:

```sh
git clone https://github.com/codeafridi/Crowsnest.git
cd Crowsnest
bash cpp/packaging/verify_release.sh --all
```

`--all` resolves the latest tag, downloads `SHA256SUMS` and its Sigstore
bundle, verifies the signature against the exact workflow identity, then
downloads every asset the manifest names and checks each digest. Pass a tag
instead of `latest` to pin one:

```sh
bash cpp/packaging/verify_release.sh --all v0.13.5
```

Exit codes: `0` verified, `1` signature/checksum failure, `2` usage or cosign
missing. A failed signature or checksum means **do not install**.

## Without the script

```sh
tag=v0.13.5   # the release you are checking
base="https://github.com/codeafridi/Crowsnest/releases/download/$tag"
curl -fsSLO "$base/SHA256SUMS"
curl -fsSLO "$base/SHA256SUMS.sigstore.json"

cosign verify-blob --bundle SHA256SUMS.sigstore.json \
  --certificate-identity \
    "https://github.com/codeafridi/Crowsnest/.github/workflows/release.yml@refs/tags/$tag" \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com \
  SHA256SUMS
```

The identity and issuer are the point. Do not replace them with a wildcard:
the signature only means something when it names this repository's release
workflow at that tag.

Then check the bytes:

```sh
sha256sum -c SHA256SUMS        # macOS: shasum -a 256 -c SHA256SUMS
```

For a single artifact you can also use the installer's `--verify` /
`-Verify`, but that checks the checksum only; the `cosign` command above is
what checks the publisher.

## What the signature proves

- The manifest was produced by the workflow file
  `.github/workflows/release.yml` in `codeafridi/Crowsnest`, at the tag you
  named, in a GitHub-hosted runner with an OIDC identity.
- The manifest names the bytes of every asset; a modified asset fails
  `sha256sum -c`.
- The signing event is recorded in the public Sigstore transparency log
  (Rekor), so the signature cannot be quietly minted later for a different
  workflow.

It does **not** prove that the source code is bug-free, that the repository or
its maintainers were not compromised, or that a file you downloaded outside
`SHA256SUMS` is genuine. A signature is provenance, not a code audit.

Verification normally contacts the Sigstore trust root (TUF); the bundle
carries the certificate and transparency-log proof. For an air-gapped check,
supply a cached Sigstore trusted root with cosign's `--trusted-root`.

## What is not signed

| Artifact | Signature |
|---|---|
| `SHA256SUMS` + every asset's bytes | Sigstore keyless (this page) |
| `Crowsnest-*-macos.dmg` | **not Apple-notarized**; no Developer ID certificate yet |
| `Crowsnest-*-windows.msi` / portable zip | **not Authenticode-signed**; no certificate yet |
| `crowsnest_*.deb` | not GPG-signed (deb822 repo signing is a separate future step) |

The macOS and Windows packages trigger Gatekeeper/SmartScreen once. The build
scripts are ready for certificates:

- macOS: `CROWSNEST_CODESIGN_IDENTITY` and `CROWSNEST_NOTARY_PROFILE` in
  `cpp/packaging/build_macos.sh` sign, notarize and staple the DMG. Without
  them the build is visibly unsigned.
- Windows: `CROWSNEST_SIGN_CERT` and `CROWSNEST_SIGN_PASSWORD` in
  `cpp/packaging/windows/build_msi.ps1` sign the exe and MSI with
  `signtool` and a timestamp. Without them it prints that the build is
  unsigned.

A requested signature that cannot be applied fails the build; it never
silently degrades to unsigned.

This page covers client releases. The relay's release assets are checksum-only
until its release process is moved into CI and given the same signing step; the
managed relay is not open yet.
