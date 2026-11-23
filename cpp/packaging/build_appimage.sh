#!/usr/bin/env bash
#
# build_appimage.sh - build a single-file AppImage from the portable bundle.
#
# Reuses the self-contained, bundled-Qt output of build_portable.sh, so the
# resulting AppImage runs on any glibc >= 2.34 x86_64 or aarch64 distro with
# no Qt or compiler installed. appimagetool is downloaded on first use; Docker
# is only needed if the portable bundle has not been built yet.
#
# Usage:
#   bash cpp/packaging/build_appimage.sh [output_dir]
#
# Environment:
#   APPIMAGETOOL_URL  override the appimagetool download URL
#   APPIMAGE_RUNTIME_URL  override the type2 runtime download URL
#   APPIMAGE_RUNTIME_SHA256  refuse a runtime whose sha256 does not match
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
out_dir="${1:-$repo_root/dist}"
mkdir -p "$out_dir"

version="${VERSION:-$(git -C "$repo_root" describe --tags --abbrev=0 2>/dev/null || echo 0.0.0)}"
version="${version#v}"
arch="${TARGET_ARCH:-$(uname -m)}"
case "$arch" in
  x86_64|amd64) arch=x86_64; bundle_name=crowsnest-portable.tar.gz ;;
  aarch64|arm64) arch=aarch64; bundle_name=crowsnest-portable-aarch64.tar.gz ;;
  *) echo "error: unsupported AppImage architecture: $arch (need x86_64 or aarch64)" >&2; exit 1 ;;
esac

bundle="$out_dir/$bundle_name"
appdir="$out_dir/Crowsnest.AppDir"
appimage="$out_dir/Crowsnest-${version}-${arch}.AppImage"
tool_dir="$out_dir/.tools"
appimagetool="$tool_dir/appimagetool-${arch}.AppImage"
appimagetool_url="${APPIMAGETOOL_URL:-https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${arch}.AppImage}"

log() { printf '>> %s\n' "$*" >&2; }

# Force extraction-based execution of appimagetool so this works without FUSE
# (containers, CI, WSL are all common cases).
export APPIMAGE_EXTRACT_AND_RUN=1

# 1. Portable bundle (bundled Qt). Build it if missing.
if [ ! -f "$bundle" ]; then
  log "portable bundle not found; building it (requires Docker)"
  TARGET_ARCH="$arch" bash "$here/build_portable.sh" "$out_dir"
fi

# 2. Assemble the AppDir.
log "assembling AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr" "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/scalable/apps"

tar xzf "$bundle" -C "$appdir/usr"

# Bundle layout is flat (crowsnest, lib/, plugins/, run.sh, bridge/, README.md).
# Move the binary under usr/bin; lib/ and plugins/ already sit at usr/.
mkdir -p "$appdir/usr/bin"
mv "$appdir/usr/crowsnest" "$appdir/usr/bin/crowsnest"
chmod +x "$appdir/usr/bin/crowsnest"
rm -f "$appdir/usr/run.sh"

cp "$here/icons/crowsnest.svg" "$appdir/usr/share/icons/hicolor/scalable/apps/crowsnest.svg"
cp "$here/icons/crowsnest.svg" "$appdir/crowsnest.svg"

cat > "$appdir/crowsnest.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Crowsnest
Comment=Compact on-screen focused-process monitor
Exec=crowsnest
Icon=crowsnest
Terminal=false
Categories=Utility;System;Monitor;
X-AppImage-Version=${version}
X-AppImage-BuildId=$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo dev)
EOF
cp "$appdir/crowsnest.desktop" "$appdir/usr/share/applications/crowsnest.desktop"

cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
export LD_LIBRARY_PATH="$HERE/usr/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
exec "$HERE/usr/bin/crowsnest" "$@"
EOF
chmod +x "$appdir/AppRun"

# 3. appimagetool + the type2 runtime.
#
#    The runtime is fetched here, not left to appimagetool: its internal
#    downloader has no visible connect/size bound, and a stalled socket once
#    held a local build for minutes. Bounding and retrying the fetch keeps a
#    release from hanging on it, and APPIMAGE_RUNTIME_SHA256 lets an operator
#    pin the bytes they trust.
tool_dir="$out_dir/.tools"
runtime="$tool_dir/runtime-${arch}"
if [ ! -s "$runtime" ]; then
  mkdir -p "$tool_dir"
  runtime_url="${APPIMAGE_RUNTIME_URL:-https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${arch}}"
  log "downloading runtime"
  ok=0
  for _ in 1 2 3; do
    rm -f "${runtime}.part"
    if command -v curl >/dev/null 2>&1; then
      curl -fsSL --connect-timeout 15 --max-time 120 \
        -o "${runtime}.part" "$runtime_url" && ok=1
    elif command -v wget >/dev/null 2>&1; then
      wget -q --timeout=120 -O "${runtime}.part" "$runtime_url" && ok=1
    else
      echo "error: need curl or wget to download the AppImage runtime" >&2
      exit 1
    fi
    [ "$ok" -eq 1 ] && break
  done
  if [ "$ok" -ne 1 ]; then
    echo "error: could not download the AppImage runtime from $runtime_url" >&2
    exit 1
  fi
  mv "${runtime}.part" "$runtime"
fi
if [ -n "${APPIMAGE_RUNTIME_SHA256:-}" ]; then
  actual="$(sha256sum "$runtime" | awk '{print $1}')"
  [ "$actual" = "$APPIMAGE_RUNTIME_SHA256" ] || {
    echo "error: runtime sha256 $actual does not match APPIMAGE_RUNTIME_SHA256" >&2
    exit 1
  }
fi

if [ ! -x "$appimagetool" ]; then
  mkdir -p "$tool_dir"
  log "downloading appimagetool"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$appimagetool_url" -o "$appimagetool"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$appimagetool" "$appimagetool_url"
  else
    echo "error: need curl or wget to download appimagetool" >&2
    exit 1
  fi
  chmod +x "$appimagetool"
fi

# 4. Emit the AppImage (with zsync update channel so AppImageUpdate and
#    `crowsnest update`-style in-place updates work).
#    UPDATE_INFORMATION format:
#    gh-releases-zsync|<owner>|<repo>|latest|<pattern>
#    NOTE: appimagetool needs -u passed explicitly (env alone is not picked
#    up); when zsyncmake is on PATH it also writes the .zsync file, so we
#    expose the copy bundled inside appimagetool if the host has none.
log "building AppImage"
export UPDATE_INFORMATION="${UPDATE_INFORMATION:-gh-releases-zsync|codeafridi|Crowsnest|latest|Crowsnest-*${arch}.AppImage.zsync}"
if ! command -v zsyncmake >/dev/null 2>&1; then
  # appimagetool ships zsyncmake inside itself, but `--appimage-extract`
  # ignores an output directory and unpacks into ./squashfs-root, so the
  # extraction has to run in its own directory. Getting this wrong is silent:
  # appimagetool still emits the AppImage and simply skips the .zsync, which
  # leaves the update channel the binary advertises pointing at a 404.
  tool_tmp="$(mktemp -d)"
  if ( cd "$tool_tmp" \
       && "$appimagetool" --appimage-extract "usr/bin/zsyncmake" >/dev/null 2>&1 ) \
     && [ -x "$tool_tmp/squashfs-root/usr/bin/zsyncmake" ]; then
    export PATH="$tool_tmp/squashfs-root/usr/bin:$PATH"
  else
    echo "warning: no zsyncmake found; install it (apt install zsync) for" >&2
    echo "         AppImageUpdate delta support" >&2
  fi
fi
if ! ARCH="$arch" "$appimagetool" --runtime-file "$runtime" -u "$UPDATE_INFORMATION" --no-appstream "$appdir" "$appimage" >/dev/null 2>&1; then
  ARCH="$arch" "$appimagetool" --runtime-file "$runtime" -u "$UPDATE_INFORMATION" "$appdir" "$appimage"
fi
chmod +x "$appimage" 2>/dev/null || true

# appimagetool writes the .zsync into the CURRENT directory, not next to the
# image, so every release since 0.5.0 left it in the repo root and shipped an
# AppImage advertising an update file nobody uploaded. Move it where the image
# is before checking.
if [ ! -f "${appimage}.zsync" ]; then
  stray="$(basename "$appimage").zsync"
  if [ -f "$stray" ]; then
    mv "$stray" "${appimage}.zsync"
  fi
fi

# The embedded UPDATE_INFORMATION names <image>.zsync. Shipping the image
# without it advertises an update channel that 404s, so refuse to finish.
if [ ! -f "${appimage}.zsync" ]; then
  echo "error: ${appimage}.zsync was not produced - AppImageUpdate would" >&2
  echo "       point at a missing file (install zsync and rerun)" >&2
  exit 1
fi

log "done: $appimage"
log "run:  $appimage"
log "update info: $UPDATE_INFORMATION (delta updates via AppImageUpdate)"
