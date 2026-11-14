#!/bin/sh
# Crowsnest installer (Linux x86-64, macOS, Windows).
#
# One-line install — the SAME command on every OS:
#   curl -fsSL https://raw.githubusercontent.com/codeafridi/Crowsnest/main/install.sh | sh
#   (or, when curl is absent:  wget -qO- ...   — see README)
# Everything the installer downloads after bootstrap goes through a
# fallback ladder: native curl -> wget -> snap curl -> busybox wget ->
# distro package-manager rescue -> manual instructions.
#
# Installs the app and the `crowsnest` command. Nothing starts automatically:
# run `crowsnest start` after installing, or pass --autostart to have the
# installer start the HUD now and at login. Downloads show live byte/percent
# progress on the terminal.
#
# Linux x86-64 (glibc >= 2.34): downloads a self-contained build (Qt 6 bundled
# — nothing else to install) into ~/.local/opt/crowsnest, and installs the
# desktop focus bridge (GNOME extension / KWin script) when one is needed.
#
# macOS: downloads the latest DMG, mounts it, copies Crowsnest.app into
# /Applications (falls back to ~/Applications without admin rights),
# installs the `crowsnest` command in /usr/local/bin, and clears the
# quarantine attribute so the unsigned app starts like any other.
#
# Windows (run inside Git Bash / MSYS2): downloads the portable zip into
# %LOCALAPPDATA%\Crowsnest and adds a `crowsnest` command through the
# WindowsApps PATH directory.
#
# Options:
#   --autostart      also start the HUD now and at login (default: off)
#   --no-autostart   accepted for compatibility; same as the default
#   --no-bridge      skip installing the Linux desktop focus bridge
#   --uninstall      remove the app, command, service, autostart and bridge
#   -h, --help       show this help
#
# Environment overrides:
#   CROWSNEST_BUNDLE_URL  override the Linux portable-bundle URL
#   CROWSNEST_DMG_URL     override the macOS DMG URL
#   CROWSNEST_WIN_URL     override the Windows portable-zip URL
#   CROWSNEST_TEST_TAG    pin the resolved release tag (packaging CI only)
#
set -eu

REPO="codeafridi/Crowsnest"
RAW="https://raw.githubusercontent.com/${REPO}/main"
BUNDLE_URL="${CROWSNEST_BUNDLE_URL:-https://github.com/${REPO}/releases/latest/download/crowsnest-portable.tar.gz}"

HOME_DIR="${HOME:-/root}"
APP_DIR="${CROWSNEST_DIR:-$HOME_DIR/.local/opt/crowsnest}"
BIN_DIR="${XDG_BINDIR:-$HOME_DIR/.local/bin}"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME_DIR/.config}"
UNIT_DIR="$CONFIG_HOME/systemd/user"
UNIT_FILE="$UNIT_DIR/crowsnest.service"
AUTOSTART_DIR="$CONFIG_HOME/autostart"
AUTOSTART_FILE="$AUTOSTART_DIR/crowsnest.desktop"
WRAPPER="$BIN_DIR/crowsnest"

GNOME_UUID="crowsnest-focus@codeafridi"
KDE_ID="crowsnest-focus"

MAC_LABEL="dev.crowsnest.hud"
MAC_PLIST="${HOME_DIR}/Library/LaunchAgents/${MAC_LABEL}.plist"
MAC_APP_DEFAULT="/Applications/Crowsnest.app"
MAC_CLI="/usr/local/bin/crowsnest"

autostart=0
bridge=1
mode=install

usage() {
  cat >&2 <<'USAGE'
Crowsnest installer (Linux x86-64, macOS, Windows).

Installs the app and the `crowsnest` command. Nothing starts automatically:
run `crowsnest start` after installing, or pass --autostart to have the
installer start the HUD now and at login. Downloads show live byte/percent
progress on the terminal.

  --autostart      also start the HUD now and at login (default: off)
  --no-autostart   accepted for compatibility; same as the default
  --no-bridge      skip installing the Linux desktop focus bridge
  --uninstall      remove the app, command, service, autostart and bridge
  -h, --help       show this help

Environment overrides:
  CROWSNEST_BUNDLE_URL  override the Linux portable-bundle URL
  CROWSNEST_DMG_URL     override the macOS DMG URL
  CROWSNEST_WIN_URL     override the Windows portable-zip URL
  CROWSNEST_TEST_TAG    pin the resolved release tag (packaging CI only)
USAGE
}

for arg in "$@"; do
  case "$arg" in
    --uninstall)    mode=uninstall ;;
    --autostart)    autostart=1 ;;
    --no-autostart) autostart=0 ;;
    --no-bridge)    bridge=0 ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "unknown option: $arg" >&2; usage; exit 2 ;;
  esac
done

log() { printf '%s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

under_snap() { # <tool> — true when the tool resolves under /snap (sandboxed)
  case "$(readlink -f "$(command -v "$1" 2>/dev/null)" 2>/dev/null)" in
    /snap/*) return 0 ;;
    *)       return 1 ;;
  esac
}

pkg_install_curl() { # best-effort distro rescue; 0 iff curl became usable
  if command -v apt-get >/dev/null 2>&1; then
    run_priv apt-get update -y >/dev/null 2>&1 || true
    run_priv apt-get install -y curl >/dev/null 2>&1
  elif command -v dnf >/dev/null 2>&1; then
    run_priv dnf install -y curl >/dev/null 2>&1
  elif command -v yum >/dev/null 2>&1; then
    run_priv yum install -y curl >/dev/null 2>&1
  elif command -v zypper >/dev/null 2>&1; then
    run_priv zypper --non-interactive install curl >/dev/null 2>&1
  elif command -v pacman >/dev/null 2>&1; then
    run_priv pacman -S --noconfirm curl >/dev/null 2>&1
  elif command -v apk >/dev/null 2>&1; then
    run_priv apk add curl >/dev/null 2>&1
  else
    return 1
  fi
  command -v curl >/dev/null 2>&1 && command curl -fsSL \
    "https://github.com/${REPO}" -o /dev/null >/dev/null 2>&1
}

# Where downloads land. This must be REAL DISK: /tmp is a tmpfs on most
# desktops, and a 40 MB bundle written into a tmpfs that is nearly full dies
# with ENOSPC halfway through - which is exactly the failure a user reported as
# "could not download". A snap-confined curl additionally gets a private /tmp
# the main mount does not share.
work_dir() { # -> prints a writable directory with room for the bundle
  need_kb=131072   # the bundle plus the extracted tree, with slack
  for base in "${CROWSNEST_TMPDIR:-}" "${XDG_CACHE_HOME:-$HOME/.cache}" "$HOME" "${TMPDIR:-}" /tmp; do
    [ -n "$base" ] || continue
    dir="$base/crowsnest-install"
    mkdir -p "$dir" 2>/dev/null || continue
    [ -w "$dir" ] || continue
    avail="$(df -Pk "$dir" 2>/dev/null | awk 'NR==2 { print $4 }')"
    case "$avail" in
      ''|*[!0-9]*) printf '%s\n' "$dir"; return 0 ;;
    esac
    if [ "$avail" -ge "$need_kb" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
  done
  # Nothing had the room we asked for; use the first writable candidate and let
  # the download report the truth instead of guessing here.
  for base in "${CROWSNEST_TMPDIR:-}" "${XDG_CACHE_HOME:-$HOME/.cache}" "$HOME" "${TMPDIR:-}" /tmp; do
    [ -n "$base" ] || continue
    dir="$base/crowsnest-install"
    mkdir -p "$dir" 2>/dev/null && [ -w "$dir" ] && { printf '%s\n' "$dir"; return 0; }
  done
  return 1
}

# A write failure is not a missing downloader. Classify it so the installer
# stops recommending "apt-get install curl" to someone whose disk is full (and
# stops asking for sudo to re-attempt a download that cannot succeed).
write_failure() { # <stderr-file> <dest-dir> — 0 when the cause was the write
  [ -f "$1" ] || return 1
  if grep -qiE 'no space left|disk quota|short write|failure writing|cannot write to|write error' "$1"; then
    dir="${2:-.}"
    avail="$(df -h "$dir" 2>/dev/null | awk 'NR==2 { print $4 " free of " $2 }')"
    die "the download could not be WRITTEN to $dir (${avail:-no space reported})
This is a disk-space problem, not a downloader problem: a Crowsnest bundle needs
about 90 MB free while it unpacks. Free some space, or point the installer at a
larger filesystem:
  export XDG_CACHE_HOME=/path/with/space     # or CROWSNEST_TMPDIR=/that/path
  then re-run this installer."
  fi
  return 1
}

# Live download progress: a background watcher prints bytes/percent once per
# second on a tty; the downloader itself stays silent so its stderr keeps
# recording real failures for write_failure.
watcher=""

stop_watcher() {
  if [ -n "$watcher" ]; then
    kill "$watcher" 2>/dev/null || true
    wait "$watcher" 2>/dev/null || true
    watcher=""
  fi
}

content_length() { # <url> — echoes the server's Content-Length in bytes, else nothing
  len=""
  if command -v curl >/dev/null 2>&1; then
    len="$(curl -fsIL --connect-timeout 10 --max-time 15 "$1" 2>/dev/null \
      | tr -d '\r' | grep -i '^content-length:' | tail -n 1 | awk '{print $2}')"
  elif command -v wget >/dev/null 2>&1; then
    len="$(wget -q --spider -S -T 20 "$1" 2>&1 \
      | grep -i '^ *content-length:' | tail -n 1 | awk '{print $2}')"
  fi
  case "$len" in
    ''|*[!0-9]*) return 0 ;;
    *)           printf '%s\n' "$len" ;;
  esac
}

fmt_bytes() { # <bytes> — human-readable size for the progress line
  awk -v b="${1:-0}" 'BEGIN {
    if (b !~ /^[0-9]+$/) { print "--"; exit }
    split("B KiB MiB GiB TiB", u)
    i = 1
    while (b >= 1024 && i < 5) { b /= 1024; i++ }
    if (i == 1) printf "%d %s\n", b, u[i]
    else        printf "%.1f %s\n", b, u[i]
  }'
}

progress_loop() { # <dest> <want> — background watcher; exits by itself when the
                  # size has been stable (done/failed) or never moves
  f="$1" want="$2" prev="" stable=0 zeros=0 missing=0 seen=0
  while :; do
    if [ ! -f "$f" ]; then
      # The downloader has not created the destination yet (DNS, connect,
      # server response). Give it 30 s before giving up on progress; a file
      # that disappears AFTER it existed means the attempt is over.
      if [ "$seen" -eq 1 ]; then exit 0; fi
      missing=$((missing + 1))
      [ "$missing" -ge 30 ] && exit 0
      sleep 1
      continue
    fi
    seen=1
    sz="$(wc -c <"$f" 2>/dev/null | tr -d '[:space:]')"
    case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
    if [ "$sz" -gt 0 ]; then
      zeros=0
      if [ "$sz" = "$prev" ]; then
        stable=$((stable + 1))
        [ "$stable" -ge 3 ] && exit 0
      else
        stable=0
      fi
    else
      zeros=$((zeros + 1))
      [ "$zeros" -ge 15 ] && exit 0
    fi
    prev="$sz"
    if [ -t 2 ]; then
      if [ -n "$want" ] && [ "$want" -gt 0 ] && [ "$sz" -le "$want" ]; then
        printf '\r\033[Kdownloading %s / %s (%s%%)' \
          "$(fmt_bytes "$sz")" "$(fmt_bytes "$want")" "$(( sz * 100 / want ))" >&2
      else
        # No (or a wrong) advertised length: bytes only, never a fake percent.
        printf '\r\033[Kdownloading %s' "$(fmt_bytes "$sz")" >&2
      fi
    fi
    sleep 1
  done
}

finish() { # <dest> — stop the watcher, clear the line, report the size
  stop_watcher
  [ -t 2 ] && printf '\r\033[K' >&2
  sz="$(wc -c <"$1" 2>/dev/null | tr -d '[:space:]')"
  case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
  log "downloaded: $(fmt_bytes "$sz") $1"
}

dl_curl() { # <url> <dest> — one silent curl attempt; write_failure dies on disk errors
  curl -fsSL --connect-timeout 10 -o "$2" "$1" 2>"$errf" && return 0
  write_failure "$errf" "$(dirname "$2")" || true
  return 1
}

dl_wget() { # <url> <dest> — one silent wget attempt
  wget -q -T 20 -O "$2" "$1" 2>"$errf" && return 0
  write_failure "$errf" "$(dirname "$2")" || true
  return 1
}

dl_busybox() { # <url> <dest> — one silent busybox wget attempt
  busybox wget -q -T 20 -O "$2" "$1" 2>"$errf" && return 0
  write_failure "$errf" "$(dirname "$2")" || true
  return 1
}

fetch() { # <url> <dest> — downloader ladder (M58a), works without native curl
  # Every attempt records its stderr: the ladder must be able to tell "no
  # downloader" from "the disk is full", because the remedy is different and
  # one of them asks for the user's sudo password for nothing.
  errf="$(dirname "$2")/.crowsnest-dl.stderr"
  { : > "$errf"; } 2>/dev/null || errf=/dev/null
  attempted=""

  if [ -t 2 ] && [ -z "$watcher" ]; then
    want="$(content_length "$1" || true)"
    case "$want" in ''|0|*[!0-9]*) want="" ;; esac
    progress_loop "$2" "$want" &
    watcher="$!"
  fi

  if command -v curl >/dev/null 2>&1 && ! under_snap curl; then
    attempted="$attempted curl"
    if dl_curl "$1" "$2"; then finish "$2"; return 0; fi
  fi
  if command -v wget >/dev/null 2>&1 && ! under_snap wget; then
    attempted="$attempted wget"
    if dl_wget "$1" "$2"; then finish "$2"; return 0; fi
  fi
  if command -v curl >/dev/null 2>&1; then
    log "note: the curl on this machine is a snap (sandbox may block writes)"
    attempted="$attempted snap-curl"
    if dl_curl "$1" "$2"; then finish "$2"; return 0; fi
    log "snap curl failed - continuing down the ladder"
  fi
  if command -v busybox >/dev/null 2>&1; then
    attempted="$attempted busybox"
    if dl_busybox "$1" "$2"; then finish "$2"; return 0; fi
  fi

  # Only ask for sudo when there is no downloader at all - not when one ran and
  # failed to write.
  case "$attempted" in
    "") log "no downloader found; installing one" ;;
    *)  log "every downloader present failed; not retrying through the package manager
   (see the last error below)" ;;
  esac
  if [ -z "$attempted" ] || ! command -v curl >/dev/null 2>&1; then
    if pkg_install_curl; then
      if dl_curl "$1" "$2"; then finish "$2"; return 0; fi
      if command -v wget >/dev/null 2>&1; then
        if dl_wget "$1" "$2"; then finish "$2"; return 0; fi
      fi
    fi
  fi

  stop_watcher
  last="$(head -n 1 "$errf" 2>/dev/null | cut -c1-160)"
  [ -n "$last" ] && last="
last error: $last"
  die "could not download $(basename "$1")${last}
tried:$attempted
A downloader is present (or could not be installed). If the message above is a
network error, check the URL and your connection; if it mentions writes, see
the space advice above. You can also fetch the bundle yourself from
https://github.com/${REPO}/releases"
}

run_priv() { # <cmd...> — passwordless sudo when possible, else interactive
  if sudo -n "$@" 2>/dev/null; then
    return 0
  fi
  sudo "$@"
}

have_systemd() {
  command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1
}

desktop_id() {
  printf '%s' "${XDG_CURRENT_DESKTOP:-}${XDG_SESSION_DESKTOP:-}" | tr '[:upper:]' '[:lower:]'
}

latest_version_tag() { # echoes v0.8.0 (releases/latest redirects to /releases/tag/<tag>)
  local loc tag
  if command -v curl >/dev/null 2>&1; then
    loc="$(curl -fsS -o /dev/null -w '%{redirect_url}' "https://github.com/${REPO}/releases/latest" || true)"
  elif command -v wget >/dev/null 2>&1; then
    loc="$(wget -q --max-redirect=0 -O /dev/null -S "https://github.com/${REPO}/releases/latest" 2>&1 | grep -i '^ *Location:' | tr -d '\r' | tail -n 1 | awk '{print $2}')"
  fi
  tag="${loc##*/tag/}"
  [ -n "$tag" ] \
    || die "could not resolve the latest release; open https://github.com/${REPO}/releases"
  log "latest release: $tag"
  printf '%s' "$tag"
}

# ---------------------------------------------------------------- macOS ----

mac_install() {
  command -v hdiutil >/dev/null 2>&1 || die "hdiutil is required (macOS only)"
  if [ -n "${CROWSNEST_DMG_URL:-}" ]; then
    dmg_url="$CROWSNEST_DMG_URL"
  elif [ -n "${CROWSNEST_TEST_TAG:-}" ]; then
    dmg_url="https://github.com/${REPO}/releases/download/${CROWSNEST_TEST_TAG}/Crowsnest-$(printf '%s' "$CROWSNEST_TEST_TAG" | tr -d v)-macos.dmg"
  else
    tag="$(latest_version_tag)"
    dmg_url="https://github.com/${REPO}/releases/download/${tag}/Crowsnest-$(printf '%s' "$tag" | tr -d v)-macos.dmg"
  fi

  tmp="$(work_dir)" || die "no writable directory for the download"
  mkdir -p "$tmp" 2>/dev/null || true
  trap 'rm -rf "$tmp"' EXIT INT TERM

  log "Downloading Crowsnest for macOS..."
  fetch "$dmg_url" "$tmp/crowsnest.dmg" \
    || die "download failed ($dmg_url). Get the DMG from https://github.com/${REPO}/releases"

  hdiutil attach -nobrowse -readonly -quiet "$tmp/crowsnest.dmg" \
    || die "could not mount the DMG"
  mount="/Volumes/Crowsnest"
  if [ ! -d "$mount/Crowsnest.app" ]; then
    hdiutil detach "$mount" -force -quiet >/dev/null 2>&1 || true
    die "no Crowsnest.app inside the DMG"
  fi

  mac_app="$MAC_APP_DEFAULT"
  res_old="$mac_app/Contents/Resources/crowsnest"
  res_old_home="${HOME_DIR}/Applications/Crowsnest.app/Contents/Resources/crowsnest"
  if [ -x "$res_old" ]; then
    "$res_old" disable >/dev/null 2>&1 || true
  elif [ -x "$res_old_home" ]; then
    "$res_old_home" disable >/dev/null 2>&1 || true
  else
    launchctl bootout "gui/$(id -u)/${MAC_LABEL}" >/dev/null 2>&1 || true
  fi

  if [ -w /Applications ]; then
    rm -rf "$MAC_APP_DEFAULT"
    cp -R "$mount/Crowsnest.app" "$MAC_APP_DEFAULT"
  elif run_priv rm -rf "$MAC_APP_DEFAULT" \
    && run_priv cp -R "$mount/Crowsnest.app" /Applications/; then
    :
  else
    mac_app="${HOME_DIR}/Applications/Crowsnest.app"
    mkdir -p "${HOME_DIR}/Applications"
    rm -rf "$mac_app"
    cp -R "$mount/Crowsnest.app" "$mac_app"
    log "note: no admin rights for /Applications - installed to ~/Applications"
  fi
  hdiutil detach "$mount" -quiet >/dev/null 2>&1 \
    || hdiutil detach "$mount" -force -quiet >/dev/null 2>&1 || true

  # curl never sets the quarantine attribute; clear it defensively anyway
  # so a re-downloaded DMG places a runnable app.
  xattr -dr com.apple.quarantine "$mac_app" >/dev/null 2>&1 || true
  log "Installed bundle: $mac_app"

  res="$mac_app/Contents/Resources/crowsnest"
  mkdir -p "$(dirname "$res")"
  fetch "$RAW/cpp/packaging/install_macos.sh" "$res" \
    || die "could not fetch the macOS verb manager (install_macos.sh)"
  chmod 0755 "$res"

  cat > "$tmp/crowsnest-cli" <<EOF
#!/bin/sh
exec "\${CROWSNEST_APP:-$mac_app}/Contents/Resources/crowsnest" "\$@"
EOF
  chmod 0755 "$tmp/crowsnest-cli"
  mac_cli_dir="$(dirname "$MAC_CLI")"
  mkdir -p "$mac_cli_dir" 2>/dev/null || true
  if [ -w "$mac_cli_dir" ]; then
    cp -f "$tmp/crowsnest-cli" "$MAC_CLI"
  elif run_priv /bin/mkdir -p "$mac_cli_dir" \
    && run_priv /usr/bin/install -m 0755 "$tmp/crowsnest-cli" "$MAC_CLI"; then
    :
  else
    log "note: could not write $MAC_CLI - the verb manager works directly:"
    log "      \"$res\" {start|stop|restart|status|logs|enable|disable|update|uninstall|path}"
  fi

  if [ "$autostart" -eq 1 ]; then
    "$res" enable
    log "Started now and at login."
  else
    log "Installed. Nothing is running yet."
  fi

  cat <<'EOF' >&2
Next steps:
  crowsnest start       run the HUD now
  crowsnest stop        stop the HUD
  crowsnest restart     stop + start
  crowsnest status      is it running?
  crowsnest logs        tail the HUD output
  crowsnest enable      also run at every login
  crowsnest disable     stop and don't start at login
  crowsnest update      upgrade to the latest version (re-runs this installer)
  crowsnest uninstall   remove everything
EOF
}

mac_uninstall_standalone() {
  for res_try in \
    "$MAC_APP_DEFAULT/Contents/Resources/crowsnest" \
    "${HOME_DIR}/Applications/Crowsnest.app/Contents/Resources/crowsnest"; do
    if [ -x "$res_try" ]; then
      "$res_try" disable >/dev/null 2>&1 || true
      break
    fi
  done
  launchctl bootout "gui/$(id -u)/${MAC_LABEL}" >/dev/null 2>&1 || true
  rm -f "$MAC_PLIST" 2>/dev/null || true
  if [ -w /Applications ]; then
    rm -rf "$MAC_APP_DEFAULT"
  elif run_priv rm -rf "$MAC_APP_DEFAULT"; then
    :
  else
    log "note: could not remove $MAC_APP_DEFAULT without admin rights - remove it in Finder."
  fi
  if [ -w "$(dirname "$MAC_CLI")" ]; then
    rm -f "$MAC_CLI"
  else
    run_priv rm -f "$MAC_CLI" 2>/dev/null || true
  fi
  log "Crowsnest uninstalled (macOS)."
}

# ------------------------------------------------------------- Windows ----

win_install() {
  command -v powershell.exe >/dev/null 2>&1 \
    || die "this Windows flow needs Git Bash/MSYS2 (powershell.exe on PATH)"
  command -v cygpath >/dev/null 2>&1 \
    || die "cygpath is required (Git Bash)"
  appdirWin="${LOCALAPPDATA:-${USERPROFILE}\\AppData\\Local}\\Crowsnest"
  appdir="$(cygpath -u "$appdirWin" 2>/dev/null \
    || printf '%s' "$appdirWin" | sed 's|\\|/|g')"

  if [ -n "${CROWSNEST_WIN_URL:-}" ]; then
    zip_url="$CROWSNEST_WIN_URL"
  elif [ -n "${CROWSNEST_TEST_TAG:-}" ]; then
    zip_url="https://github.com/${REPO}/releases/download/${CROWSNEST_TEST_TAG}/Crowsnest-$(printf '%s' "$CROWSNEST_TEST_TAG" | tr -d v)-windows-portable.zip"
  else
    tag="$(latest_version_tag)"
    zip_url="https://github.com/${REPO}/releases/download/${tag}/Crowsnest-$(printf '%s' "$tag" | tr -d v)-windows-portable.zip"
  fi
  ver=""
  if [ -n "${CROWSNEST_TEST_TAG:-}" ]; then
    ver="${CROWSNEST_TEST_TAG#v}"
  elif [ -n "${tag:-}" ]; then
    ver="${tag#v}"
  fi

  tmp="$(work_dir)" || die "no writable directory for the download"
  mkdir -p "$tmp" 2>/dev/null || true
  trap 'rm -rf "$tmp"' EXIT INT TERM

  log "Downloading Crowsnest for Windows..."
  fetch "$zip_url" "$tmp/crowsnest.zip" \
    || die "download failed ($zip_url). Get the installer from https://github.com/${REPO}/releases"

  # A running exe keeps its image locked. The exe's own stop verb waits for
  # the kernel to retire each process; taskkill is the fallback. Removal is
  # retried because Windows releases the handle a moment after the exit.
  if [ -f "$appdir/crowsnest.exe" ]; then
    "$appdir/crowsnest.exe" stop >/dev/null 2>&1 || true
  fi
  taskkill //F //IM crowsnest.exe >/dev/null 2>&1 || true
  i=0
  while [ -d "$appdir" ] && [ "$i" -lt 50 ]; do
    rm -rf "$appdir" 2>/dev/null && break
    i=$((i + 1))
    sleep 0.1
  done
  [ -d "$appdir" ] \
    && die "could not replace $(cygpath -w "$appdir") - close any file open in it and retry"
  mkdir -p "$appdir"
  ZIP_WIN="$(cygpath -w "$tmp/crowsnest.zip")" \
  DEST_WIN="$(cygpath -w "$appdir")" \
  MSYS_NO_PATHCONV=1 powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -Command 'try { Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::ExtractToDirectory($env:ZIP_WIN, $env:DEST_WIN) } catch { Expand-Archive -Path $env:ZIP_WIN -DestinationPath $env:DEST_WIN -Force }' \
    || die "unzip failed"
  [ -f "$appdir/crowsnest.exe" ] || die "portable install incomplete (crowsnest.exe missing)"
  if [ -n "$ver" ]; then
    printf '%s\n' "$ver" > "$appdir/VERSION"
  fi

  shimDirUnix="$(dirname "$appdir")/Microsoft/WindowsApps"
  shimDirWin="$(cygpath -w "$shimDirUnix")"
  mkdir -p "$shimDirUnix" 2>/dev/null || true
  # uninstall is handled by the shims: the running exe cannot delete the
  # folder it is executing from, and a batch file can remove itself.
  printf '@echo off\r\nif /i "%%~1"=="uninstall" (\r\n  if exist "%%LOCALAPPDATA%%\\Crowsnest\\crowsnest.exe" "%%LOCALAPPDATA%%\\Crowsnest\\crowsnest.exe" disable >nul 2>&1\r\n  rmdir /s /q "%%LOCALAPPDATA%%\\Crowsnest" 2>nul\r\n  del "%%LOCALAPPDATA%%\\Microsoft\\WindowsApps\\crowsnest" 2>nul\r\n  echo Crowsnest uninstalled (Windows).\r\n  (goto) 2>nul & del "%%~f0"\r\n  exit /b 0\r\n)\r\nif "%%~1"=="" (\r\n  start "" /b "%%LOCALAPPDATA%%\\Crowsnest\\crowsnest.exe"\r\n  exit /b 0\r\n)\r\n"%%LOCALAPPDATA%%\\Crowsnest\\crowsnest.exe" %%*\r\n' \
    > "$shimDirUnix/crowsnest.cmd" 2>/dev/null || true
  printf '#!/bin/sh\nif [ "$1" = "uninstall" ]; then\n  if [ -x "$LOCALAPPDATA/Crowsnest/crowsnest.exe" ]; then\n    "$LOCALAPPDATA/Crowsnest/crowsnest.exe" disable >/dev/null 2>&1 || true\n  fi\n  MSYS_NO_PATHCONV=1 cmd.exe //c rmdir /s /q "%%LOCALAPPDATA%%\\Crowsnest" >/dev/null 2>&1 || true\n  rm -f "$LOCALAPPDATA/Microsoft/WindowsApps/crowsnest.cmd"\n  rm -f "$LOCALAPPDATA/Microsoft/WindowsApps/crowsnest"\n  echo "Crowsnest uninstalled (Windows)."\n  exit 0\nfi\nif [ "$#" -eq 0 ]; then\n  cmd.exe //c start "" /b "$LOCALAPPDATA\\Crowsnest\\crowsnest.exe" >/dev/null 2>&1\n  exit 0\nfi\nexec "$LOCALAPPDATA/Crowsnest/crowsnest.exe" "$@"\n' \
    > "$shimDirUnix/crowsnest" 2>/dev/null || true
  chmod 0755 "$shimDirUnix/crowsnest" 2>/dev/null || true
  if [ ! -f "$shimDirUnix/crowsnest.cmd" ] && [ ! -f "$shimDirUnix/crowsnest" ]; then
    log "note: could not write the 'crowsnest' command to $shimDirWin"
    log "      run the HUD directly: $(cygpath -w "$appdir/crowsnest.exe")"
  fi

  if [ "$autostart" -eq 1 ]; then
    "$appdir/crowsnest.exe" --autostart=on
    EXE_WIN="$(cygpath -w "$appdir/crowsnest.exe")" \
    MSYS_NO_PATHCONV=1 powershell.exe -NoProfile -ExecutionPolicy Bypass \
      -Command 'Start-Process -FilePath $env:EXE_WIN' >/dev/null 2>&1 || true
    log "Started now and at login."
  else
    log "Nothing is running yet - run: crowsnest"
  fi
  if [ -n "$ver" ]; then
    log "Installed $ver to $appdirWin"
  else
    log "Installed to $appdirWin"
  fi

  cat <<'EOF' >&2
Next steps:
  crowsnest              run the HUD now (open a NEW shell first so the
                         command is on PATH)
  crowsnest stop         stop it
  crowsnest status       version, autostart state, running instances
  crowsnest restart      restart the HUD
  crowsnest enable       start now and at every login
  crowsnest disable      stop and do not start at login
  crowsnest update       download and install the latest release
  crowsnest uninstall    remove the app, the command and autostart
  crowsnest path         show where it is installed
  crowsnest vers         show the installed version
  crowsnest --dump       JSON snapshot of the machine (headless check)
EOF
  log "note: unsigned build - SmartScreen 'More info / Run anyway' on first GUI launch."
}

win_uninstall_standalone() {
  appdirWin="${LOCALAPPDATA:-${USERPROFILE}\\AppData\\Local}\\Crowsnest"
  appdir="$(cygpath -u "$appdirWin" 2>/dev/null \
    || printf '%s' "$appdirWin" | sed 's|\\|/|g')"
  "$appdir/crowsnest.exe" --autostart=off 2>/dev/null || true
  taskkill //F //IM crowsnest.exe >/dev/null 2>&1 || true
  rm -rf "$appdir"
  shimDirUnix="$(dirname "$appdir")/Microsoft/WindowsApps"
  rm -f "$shimDirUnix/crowsnest.cmd" "$shimDirUnix/crowsnest" 2>/dev/null || true
  log "Crowsnest uninstalled (Windows)."
}

# --------------------------------------------------------------- Linux ----

write_wrapper() {
  mkdir -p "$BIN_DIR"
  cat > "$WRAPPER" <<EOF
#!/bin/sh
set -eu
APP="\${CROWSNEST_DIR:-\$HOME/.local/opt/crowsnest}"
UNIT="crowsnest.service"
XDG_ENTRY="\${XDG_CONFIG_HOME:-\$HOME/.config}/autostart/crowsnest.desktop"
have_sd() { command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; }
launch_now() { ( setsid "\$APP/run.sh" >/dev/null 2>&1 & ) || true; }
add_xdg_entry() {
  mkdir -p "\$(dirname "\$XDG_ENTRY")"
  printf '[Desktop Entry]\nType=Application\nName=Crowsnest\nExec=%s\nTerminal=false\nX-GNOME-Autostart-enabled=true\n' "\$APP/run.sh" > "\$XDG_ENTRY"
}
rm_xdg_entry() { rm -f "\$XDG_ENTRY" 2>/dev/null || true; }
running() {
  command -v pgrep >/dev/null 2>&1 && pgrep -f "\$APP/crowsnest" >/dev/null 2>&1
}
stop_app() {
  if command -v pkill >/dev/null 2>&1; then
    pkill -f "\$APP/crowsnest" 2>/dev/null || true
  else
    echo "note: pkill not available on this system; kill the HUD manually with: pkill -f crowsnest" >&2
  fi
}
tail_log() {
  if have_sd && command -v journalctl >/dev/null 2>&1; then
    exec journalctl --user -u "\$UNIT" -f
  fi
  echo "journal not available; follow the app log with:" >&2
  echo "  tail -f \$HOME/.cache/dev.crowsnest/crowsnest/hud.log" >&2
  exit 1
}
cmd="\${1:-run}"
[ "\$#" -gt 0 ] && shift
case "\$cmd" in
  run|gui)   exec "\$APP/run.sh" "\$@" ;;
  start)     if have_sd && systemctl --user start "\$UNIT" 2>/dev/null; then :; else launch_now; fi ;;
  stop)      if have_sd; then systemctl --user stop "\$UNIT" 2>/dev/null || true; fi; stop_app ;;
  restart)   if have_sd && systemctl --user restart "\$UNIT" 2>/dev/null; then :; else stop_app; sleep 1; launch_now; fi ;;
  status)    if running; then pgrep -af "\$APP/crowsnest" 2>/dev/null || echo "running"; else echo "not running"; fi; [ -f "\$APP/VERSION" ] && printf 'version: ' && cat "\$APP/VERSION" || true ;;
  vers)      [ -f "\$APP/VERSION" ] && cat "\$APP/VERSION" || echo "unknown" ;;
  logs)      tail_log ;;
  enable)    if have_sd && systemctl --user enable --now "\$UNIT" 2>/dev/null; then :; else add_xdg_entry; launch_now; fi ;;
  disable)   if have_sd; then systemctl --user disable --now "\$UNIT" 2>/dev/null || true; fi; rm_xdg_entry; stop_app ;;
  update)    if have_sd; then systemctl --user stop "\$UNIT" 2>/dev/null || true; fi
             stop_app
             t="\$(mktemp)"
             if wget -q -T 20 -O "\$t" ${RAW}/install.sh 2>/dev/null \
                || curl -fsSL --connect-timeout 10 -o "\$t" ${RAW}/install.sh \
                || busybox wget -q -T 20 -O "\$t" ${RAW}/install.sh; then
               sh "\$t"
             else
               echo "crowsnest update: no working downloader (wget/curl/busybox)" >&2
             fi
             rm -f "\$t" ;;
  uninstall) stop_app
             t="\$(mktemp)"
             if wget -q -T 20 -O "\$t" ${RAW}/install.sh 2>/dev/null \
                || curl -fsSL --connect-timeout 10 -o "\$t" ${RAW}/install.sh \
                || busybox wget -q -T 20 -O "\$t" ${RAW}/install.sh; then
               sh "\$t" --uninstall
             else
               echo "crowsnest uninstall: no working downloader (wget/curl/busybox)" >&2
             fi
             rm -f "\$t" ;;
  path)      echo "\$APP" ;;
  -*)        exec "\$APP/run.sh" "\$cmd" "\$@" ;;
  *)         echo "crowsnest: unknown command: \$cmd" >&2
             echo "  run start stop restart status vers logs enable disable update uninstall path" >&2
             exit 2 ;;
esac
EOF
  chmod 0755 "$WRAPPER"
}

# The systemd user unit is written and loaded but never enabled or started
# here; enabling belongs to --autostart only.
write_service() {
  if ! have_systemd; then
    return 0
  fi
  mkdir -p "$UNIT_DIR"
  cat > "$UNIT_FILE" <<EOF
[Unit]
Description=Crowsnest HUD
[Service]
ExecStart=$APP_DIR/run.sh
Restart=on-failure
[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  log "Installed systemd user service: $UNIT_FILE (disabled)"
}

enable_login_start() { # --autostart only: start now and enable at every login
  if have_systemd; then
    systemctl --user import-environment DISPLAY WAYLAND_DISPLAY \
      XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_RUNTIME_DIR 2>/dev/null || true
    systemctl --user enable --now crowsnest.service \
      || log "warning: the service did not start; try: crowsnest start"
  else
    mkdir -p "$AUTOSTART_DIR"
    cat > "$AUTOSTART_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=Crowsnest
Exec=$APP_DIR/run.sh
Terminal=false
X-GNOME-Autostart-enabled=true
EOF
    ( setsid "$APP_DIR/run.sh" >/dev/null 2>&1 & ) || true
    log "note: no systemd user session; stop with: pkill -f crowsnest"
  fi
}

enable_gnome_extension() { # <uuid>
  command -v gsettings >/dev/null 2>&1 || return 0
  cur="$(gsettings get org.gnome.shell enabled-extensions 2>/dev/null || echo '[]')"
  case "$cur" in
    *"'$1'"*) return 0 ;;
  esac
  case "$cur" in
    ''|'[]'|'@as []') nv="['$1']" ;;
    *) nv="${cur%]}, '$1']" ;;
  esac
  gsettings set org.gnome.shell enabled-extensions "$nv" 2>/dev/null || true
}

disable_gnome_extension() { # <uuid>
  command -v gsettings >/dev/null 2>&1 || return 0
  cur="$(gsettings get org.gnome.shell enabled-extensions 2>/dev/null || echo '[]')"
  nv="$(printf '%s' "$cur" | sed "s/, *'$1'//; s/'$1', *//; s/'$1'//")"
  case "$nv" in
    *'['*']'*) gsettings set org.gnome.shell enabled-extensions "$nv" 2>/dev/null || true ;;
  esac
}

bridge_gnome() {
  src="$APP_DIR/bridge/gnome/$GNOME_UUID"
  dst="$HOME_DIR/.local/share/gnome-shell/extensions/$GNOME_UUID"
  if [ ! -f "$src/metadata.json" ]; then
    log "note: GNOME focus bridge not bundled; focus will show '--' until installed."
    return 0
  fi
  mkdir -p "$dst"
  cp -f "$src/metadata.json" "$src/extension.js" "$dst/" 2>/dev/null || true
  enable_gnome_extension "$GNOME_UUID"
  log "Installed GNOME focus bridge -> $dst"
  log "  GNOME loads new extensions only at login: log out and back in to activate it."
}

bridge_kde() {
  src="$APP_DIR/bridge/kwin/$KDE_ID"
  dst="$HOME_DIR/.local/share/kwin/scripts/$KDE_ID"
  if [ ! -f "$src/metadata.json" ]; then
    log "note: KWin focus bridge not bundled."
    return 0
  fi
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -r "$src/." "$dst/" 2>/dev/null || true
  if command -v kpackagetool6 >/dev/null 2>&1; then
    kpackagetool6 --type=KWin/Script -i "$dst" >/dev/null 2>&1 || true
  elif command -v kpackagetool5 >/dev/null 2>&1; then
    kpackagetool5 --type=KWin/Script -i "$dst" >/dev/null 2>&1 || true
  fi
  if command -v kwriteconfig6 >/dev/null 2>&1; then
    kwriteconfig6 --file kwinrc --group Plugins --key "${KDE_ID}Enabled" true
  elif command -v kwriteconfig5 >/dev/null 2>&1; then
    kwriteconfig5 --file kwinrc --group Plugins --key "${KDE_ID}Enabled" true
  fi
  if command -v qdbus6 >/dev/null 2>&1; then
    qdbus6 org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
  elif command -v qdbus >/dev/null 2>&1; then
    qdbus org.kde.KWin /KWin reconfigure >/dev/null 2>&1 || true
  fi
  log "Installed KDE focus bridge."
}

install_focus_bridge() {
  [ "$bridge" -eq 1 ] || return 0
  case "$(desktop_id)" in
    *gnome*) bridge_gnome ;;
    *kde*|*plasma*) bridge_kde ;;
    *sway*)   log "note: focus detection needs 'swaymsg'; install it if you don't have it." ;;
    *niri*)   log "note: focus detection needs 'niri msg'; install it if you don't have it." ;;
    *hyprland*) log "note: focus detection needs 'hyprctl'; install it if you don't have it." ;;
    *xfce*|*x11*|*mate*|*cinnamon*) log "note: on X11, focus detection needs 'xdotool'." ;;
    *) log "note: focus detection on X11 needs 'xdotool'; GNOME/KDE need the desktop bridge." ;;
  esac
}

remove_focus_bridge() {
  if [ -d "$HOME_DIR/.local/share/gnome-shell/extensions/$GNOME_UUID" ]; then
    disable_gnome_extension "$GNOME_UUID"
    rm -rf "$HOME_DIR/.local/share/gnome-shell/extensions/$GNOME_UUID"
  fi
  rm -rf "$HOME_DIR/.local/share/kwin/scripts/$KDE_ID"
}

do_uninstall() {
  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user disable --now crowsnest.service 2>/dev/null || true
  fi
  remove_focus_bridge
  rm -f "$UNIT_FILE" "$AUTOSTART_FILE" "$WRAPPER"
  [ -n "${APP_DIR:-}" ] && [ -d "$APP_DIR" ] && rm -rf "$APP_DIR"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload 2>/dev/null || true
  fi
  log "Crowsnest uninstalled."
}

linux_install() {
  command -v tar >/dev/null 2>&1 || die "tar is required"

  arch="$(uname -m 2>/dev/null || echo unknown)"
  [ "$arch" = "x86_64" ] || log "warning: this bundle is built for x86_64; you are on $arch"

  # Real disk, not a tmpfs: see work_dir().
  tmp="$(work_dir)" || die "no writable directory for the download"
  mkdir -p "$tmp" 2>/dev/null || true
  trap 'rm -rf "$tmp"' EXIT INT TERM

  log "Downloading Crowsnest..."
  fetch "$BUNDLE_URL" "$tmp/bundle.tar.gz" \
    || die "download failed ($BUNDLE_URL). If there is no release yet, build from source (see README)."

  rm -rf "$APP_DIR"
  mkdir -p "$APP_DIR"
  tar xzf "$tmp/bundle.tar.gz" -C "$APP_DIR"
  chmod +x "$APP_DIR/run.sh" "$APP_DIR/crowsnest" 2>/dev/null || true
  if [ -f "$APP_DIR/VERSION" ]; then
    log "Version: $(cat "$APP_DIR/VERSION")"
  else
    log "Version: unknown (bundle predates M58 VERSION stamping; fine)"
  fi

  write_wrapper
  write_service
  if [ "$autostart" -eq 1 ]; then
    enable_login_start
    log "Started now and at login."
  else
    log "Installed. Nothing is running yet - run: crowsnest start"
  fi
  install_focus_bridge

  log ""
  log "Installed to $APP_DIR"
  cat <<'EOF' >&2
Next steps:
  crowsnest run         run in the foreground (Ctrl+C to stop)
  crowsnest start       run in the background
  crowsnest stop        stop it now
  crowsnest status      is it running?
  crowsnest logs        tail the HUD output
  crowsnest vers        show the installed version
  crowsnest path        show where it is installed
  crowsnest enable      also run at every login
  crowsnest disable     stop and don't start at login
  crowsnest update      upgrade to the latest version
  crowsnest uninstall   remove everything
EOF

  case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) log ""; log "note: add $BIN_DIR to your PATH to use the 'crowsnest' command." ;;
  esac
}

# Library mode (M58a tests): source install.sh with CROWSNEST_LIB=1 to reuse
# fetch()/run_priv() without executing the main dispatch.
if [ "${CROWSNEST_LIB:-}" = "1" ]; then
  return 0 2>/dev/null || exit 0
fi

kernel_sys="$(uname -s)"
case "$kernel_sys" in
  Linux)                os_flow="linux" ;;
  Darwin)               os_flow="mac" ;;
  MINGW*|MSYS*|CYGWIN*) os_flow="win" ;;
  *) die "unsupported OS: $kernel_sys (Linux / macOS / Windows only)" ;;
esac

if [ "$mode" = "uninstall" ]; then
  case "$os_flow" in
    mac) mac_uninstall_standalone ;;
    win) win_uninstall_standalone ;;
    *)   do_uninstall ;;
  esac
  exit 0
fi

case "$os_flow" in
  mac) mac_install ;;
  win) win_install ;;
  *)   linux_install ;;
esac
