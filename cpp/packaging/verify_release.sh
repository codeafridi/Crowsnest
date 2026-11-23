#!/bin/sh
# verify_release.sh - verify a Crowsnest release's Sigstore provenance.
#
# The release workflow signs the unified SHA256SUMS file with Sigstore keyless
# signing. Verifying that signature proves the manifest came from
# .github/workflows/release.yml at that tag; `sha256sum -c` then proves every
# asset is the byte the manifest names. There is no key to fetch or trust.
#
# Usage:
#   bash cpp/packaging/verify_release.sh [--all] [tag|latest]
#   bash cpp/packaging/verify_release.sh --key cosign.pub --local ./dist
#   bash cpp/packaging/verify_release.sh --all --tag v0.13.5
#
# Options:
#   --all            also download every asset and check it against the manifest
#   --key <path>     verify with a public key instead of keyless identity
#                    (offline/maintainer testing; no OIDC issuer check)
#   --local <dir>    use <dir>/SHA256SUMS + <dir>/SHA256SUMS.sigstore.json
#   --tag <tag>      the tag whose workflow identity signed the manifest
#   --cosign <path>  cosign binary (default: cosign on PATH)
#
# Exit: 0 verified, 1 signature/checksum failure, 2 usage/cosign missing.
set -eu

REPO="codeafridi/Crowsnest"
WORKFLOW=".github/workflows/release.yml"
ISSUER="https://token.actions.githubusercontent.com"
ALL=0
KEY=""
LOCAL=""
TAG=""
COSIGN="${COSIGN:-cosign}"

usage() {
    sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --all) ALL=1 ;;
        --key) [ $# -ge 2 ] || usage; KEY=$2; shift ;;
        --local) [ $# -ge 2 ] || usage; LOCAL=$2; shift ;;
        --tag) [ $# -ge 2 ] || usage; TAG=$2; shift ;;
        --cosign) [ $# -ge 2 ] || usage; COSIGN=$2; shift ;;
        -h|--help) usage ;;
        -*) echo "unknown option: $1" >&2; usage ;;
        *) TAG=$1 ;;
    esac
    shift
done

command -v "$COSIGN" >/dev/null 2>&1 || {
    cat >&2 <<'EOF'
cosign is required to verify the signed manifest. Install it first:
  go install github.com/sigstore/cosign/v3/cmd/cosign@latest
  brew install cosign                 # macOS / Linuxbrew
  winget install sigstore.cosign      # Windows
Then re-run this script. The checksum alone is not a publisher proof.
EOF
    exit 2
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

fetch() { # <url> <path>
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --max-time 180 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 180 -O "$2" "$1"
    else
        echo "error: need curl or wget" >&2
        exit 2
    fi
}

latest_tag() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSI -o /dev/null -w '%{redirect_url}' \
            "https://github.com/${REPO}/releases/latest" |
            sed 's#.*/tag/##'
    else
        wget -q --max-redirect=0 -S -O /dev/null \
            "https://github.com/${REPO}/releases/latest" 2>&1 |
            grep -i '^ *Location:' | tail -n 1 | awk '{print $2}' |
            sed 's#.*/tag/##'
    fi
}

if [ -n "$LOCAL" ]; then
    LOCAL="$(cd "$LOCAL" && pwd)"
    sums="$LOCAL/SHA256SUMS"
    bundle="$LOCAL/SHA256SUMS.sigstore.json"
    [ -f "$sums" ] || { echo "missing $sums" >&2; exit 2; }
    [ -f "$bundle" ] || { echo "missing $bundle" >&2; exit 2; }
else
    [ -n "$TAG" ] || TAG="latest"
    if [ "$TAG" = "latest" ]; then
        TAG="$(latest_tag)"
    fi
    [ -n "$TAG" ] || { echo "could not resolve the latest tag" >&2; exit 1; }
    case "$TAG" in
        v*) ;;
        *) TAG="v$TAG" ;;
    esac
    sums="$work/SHA256SUMS"
    bundle="$work/SHA256SUMS.sigstore.json"
    base="https://github.com/${REPO}/releases/download/${TAG}"
    echo ">> downloading ${TAG} manifest"
    fetch "$base/SHA256SUMS" "$sums"
    fetch "$base/SHA256SUMS.sigstore.json" "$bundle"
fi

if [ -z "$TAG" ] && [ -z "$KEY" ]; then
    echo "error: keyless verification needs the tag whose workflow signed the" >&2
    echo "manifest; pass --tag <vX.Y.Z> (a --key verification needs no tag)." >&2
    exit 2
fi

echo ">> verifying the SHA256SUMS signature"
if [ -n "$KEY" ]; then
    "$COSIGN" verify-blob --key "$KEY" --bundle "$bundle" "$sums"
else
    identity="https://github.com/${REPO}/${WORKFLOW}@refs/tags/${TAG}"
    "$COSIGN" verify-blob --bundle "$bundle" \
        --certificate-identity "$identity" \
        --certificate-oidc-issuer "$ISSUER" "$sums"
fi

count="$(awk '/^[0-9a-f]{64}  /{n++} END{print n+0}' "$sums")"
[ "$count" -gt 0 ] || { echo "error: the manifest lists no files" >&2; exit 1; }
echo ">> signature OK: ${count} file(s) in the manifest"

if [ "$ALL" -eq 1 ]; then
    if [ -n "$LOCAL" ]; then
        dir="$LOCAL"
    else
        dir="$work/assets"
        mkdir -p "$dir"
        base="https://github.com/${REPO}/releases/download/${TAG}"
        awk '/^[0-9a-f]{64}  /{print $2}' "$sums" | while read -r name; do
            case "$name" in
                */*|..*) echo "refusing a path in the manifest: $name" >&2; exit 1 ;;
            esac
            echo ">> $name"
            fetch "$base/$name" "$dir/$name"
        done
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        ( cd "$dir" && sha256sum -c "$sums" )
    else
        ( cd "$dir" && shasum -a 256 -c "$sums" )
    fi
    echo ">> checksums OK: ${count} file(s)"
fi

echo ">> verified ${TAG:-local manifest}"
