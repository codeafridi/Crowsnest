#!/bin/sh
# Build the Crowsnest .app bundle and a drag-install DMG on macOS.
#
# Usage: CROWSNEST_BUILD_DIR=cpp/build bash cpp/packaging/build_macos.sh
#   CROWSNEST_BUILD_DIR  (default cpp/build)  cmake build dir to (re)configure
#   CROWSNEST_ARCHS      (default "arm64;x86_64") CMAKE_OSX_ARCHITECTURES
#   CROWSNEST_DMG_DIR    (default $CROWSNEST_BUILD_DIR/packaging)
#   CROWSNEST_SKIP_DEPLOY=1 skips macdeployqt (portable dev-tree run only)
#   CROWSNEST_CODESIGN_IDENTITY  Developer ID Application identity; when set
#                                the app and DMG are signed and verified
#   CROWSNEST_NOTARY_PROFILE     notarytool keychain profile; when set the DMG
#                                is notarized and the ticket stapled (requires
#                                CROWSNEST_CODESIGN_IDENTITY)
#
# Without the two signing variables the build is unsigned and the DMG says so.
# A requested signature that cannot be applied is an error, never a silent
# unsigned build.
#
# Bundle contract (cpp/packaging/Info.plist.in): APPL, dev.crowsnest.hud,
# LSUIElement=1, LSMinimumSystemVersion 12.0, executable name "crowsnest".

set -eu
cd "$(dirname "$0")/../.."

BUILD_DIR="${CROWSNEST_BUILD_DIR:-cpp/build}"
ARCHS="${CROWSNEST_ARCHS:-arm64;x86_64}"
OUT_DIR="${CROWSNEST_DMG_DIR:-${BUILD_DIR}/packaging}"

VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\)[[:space:]]*$/\1/p' cpp/CMakeLists.txt | head -1)"
[ -n "$VERSION" ] || { echo "cannot parse project version from cpp/CMakeLists.txt" >&2; exit 1; }

APP="${BUILD_DIR}/crowsnest.app"

cmake -S cpp -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS"
cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

[ -d "$APP" ] || { echo "missing bundle: $APP" >&2; exit 1; }
plutil -lint "${APP}/Contents/Info.plist"

if [ "${CROWSNEST_SKIP_DEPLOY:-0}" != "1" ]; then
    MDQT="$(command -v macdeployqt || true)"
    if [ -z "$MDQT" ] && [ -n "${QT_ROOT_DIR:-}" ]; then
        MDQT="$(find "$QT_ROOT_DIR" -name macdeployqt -type f 2>/dev/null | head -1 || true)"
    fi
    [ -n "$MDQT" ] || { echo "macdeployqt not found (PATH/QT_ROOT_DIR)" >&2; exit 1; }
    "$MDQT" "$APP" -always-overwrite
fi

SIGN_ID="${CROWSNEST_CODESIGN_IDENTITY:-}"
NOTARY_PROFILE="${CROWSNEST_NOTARY_PROFILE:-}"
if [ -n "$NOTARY_PROFILE" ] && [ -z "$SIGN_ID" ]; then
    echo "CROWSNEST_NOTARY_PROFILE needs CROWSNEST_CODESIGN_IDENTITY" >&2
    exit 1
fi
if [ -n "$SIGN_ID" ]; then
    echo ">> codesigning the app as $SIGN_ID" >&2
    codesign --force --deep --options runtime --timestamp \
        --sign "$SIGN_ID" "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"
fi

STAGE="$(mktemp -d "${RUNNER_TEMP:-/tmp}/crowsnest-dmg.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/Crowsnest.app"
ln -s /Applications "$STAGE/Applications"

OUT_DIR="${OUT_DIR%/}"
mkdir -p "$OUT_DIR"
DMG="$OUT_DIR/Crowsnest-${VERSION}-macos.dmg"
rm -f "$DMG" "${DMG}.sha256"

# Two-phase create + retry: hdiutil on CI runners can transiently fail
# with "Resource busy" while Spotlight (mds) scans the staging volume.
RW_DMG="$(mktemp "${TMPDIR:-/tmp}/crowsnest-rw.XXXXXX.dmg")"
trap 'rm -rf "$STAGE" "$RW_DMG"' EXIT
attempt=0
while :; do
    attempt=$((attempt + 1))
    if hdiutil create -volname Crowsnest -srcfolder "$STAGE" -ov \
        -format UDRW "$RW_DMG"; then
        break
    fi
    [ "$attempt" -lt 3 ] || { echo "hdiutil create failed" >&2; exit 1; }
    echo "hdiutil create busy, retry $attempt/3..." >&2
    sleep 5
done
attempt=0
while :; do
    attempt=$((attempt + 1))
    if hdiutil convert "$RW_DMG" -format UDZO -o "$DMG"; then
        break
    fi
    [ "$attempt" -lt 3 ] || { echo "hdiutil convert failed" >&2; exit 1; }
    echo "hdiutil convert busy, retry $attempt/3..." >&2
    sleep 5
done
rm -f "$RW_DMG"
if [ -n "$SIGN_ID" ]; then
    echo ">> codesigning the DMG" >&2
    codesign --force --timestamp --sign "$SIGN_ID" "$DMG"
fi
if [ -n "$NOTARY_PROFILE" ]; then
    echo ">> notarizing the DMG (this usually takes a few minutes)" >&2
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
    spctl -a -t open --context context:primary-signature -v "$DMG"
fi
# The sidecar must validate from the download directory, so hash the file by
# its own name rather than recording the build-tree path. Hash AFTER signing
# and stapling: those change the bytes.
( cd "$(dirname "$DMG")" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256" )
echo "built: $DMG"
