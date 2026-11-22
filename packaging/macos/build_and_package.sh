#!/usr/bin/env bash
# packaging/macos/build_and_package.sh - CE-PLT-5
#
# Universal (arm64 + x86_64) build and package of the Khensu desktop app and
# the Copilot plug-in for macOS:
#
#   1. configure + build (Xcode generator, CMAKE_OSX_ARCHITECTURES="arm64;x86_64")
#   2. verify every binary really is universal (lipo)
#   3. bundle: Khensu Mixing.app (with local-services in Contents/Resources),
#      Khensu Copilot.vst3 and Khensu Copilot.component (AU, when built)
#   4. optionally stage a relocatable Python runtime into the app
#   5. codesign (hardened runtime)   - always: a Developer ID when KHENSU_CODESIGN_IDENTITY is set, else ad hoc
#   6. notarize + staple             - only when notary credentials are set
#   7. dist/: the installer khensu-setup-<v>-macos.pkg (the app to
#      Applications, the VST3 to Library/Audio/Plug-Ins/VST3, the AU to
#      .../Components; each part can be left out, and "Install for me only"
#      puts them under ~/ instead of the system folders), a drag-and-drop .dmg
#      (app + plug-ins, to place anywhere) and a .zip of the plug-ins, with
#      SHA-256 sums. The .pkg is what the app's auto-update installs.
#
# UNVERIFIED: written and reviewed on Windows; it has not yet been run on a
# Mac, with a Developer ID or against the notary service (see TODO / CFG-01).
#
# Environment (every signing / notarizing input is optional; the step is
# skipped with a message when its inputs are unset):
#   KHENSU_BUILD_DIR            build dir                  (default: build-macos)
#   KHENSU_DIST_DIR             output dir                 (default: dist/macos)
#   KHENSU_CONFIG               CMake config               (default: Release)
#   KHENSU_ARCHS                architectures              (default: "arm64;x86_64")
#   KHENSU_DEPLOYMENT_TARGET    minimum macOS              (default: 11.0)
#   KHENSU_JOBS                 build parallelism          (default: 4)
#   KHENSU_PYTHON_RUNTIME_DIR   a relocatable universal2 Python install (e.g.
#                               python-build-standalone) to stage as
#                               local-services/runtime. Without it the app
#                               needs a system Python 3.11+ and creates its
#                               venv OUTSIDE the bundle on first run.
#   KHENSU_CODESIGN_IDENTITY    "Developer ID Application: Name (TEAMID)"
#   KHENSU_INSTALLER_IDENTITY   "Developer ID Installer: Name (TEAMID)" (signs the .pkg)
#   KHENSU_NOTARY_PROFILE       a `xcrun notarytool store-credentials` profile, or
#   KHENSU_APPLE_ID + KHENSU_TEAM_ID + KHENSU_APP_PASSWORD
#   KHENSU_SKIP_BUILD=1         package an existing build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${KHENSU_BUILD_DIR:-$ROOT/build-macos}"
DIST_DIR="${KHENSU_DIST_DIR:-$ROOT/dist/macos}"
CONFIG="${KHENSU_CONFIG:-Release}"
ARCHS="${KHENSU_ARCHS:-arm64;x86_64}"
DEPLOY="${KHENSU_DEPLOYMENT_TARGET:-11.0}"
JOBS="${KHENSU_JOBS:-4}"
ENTITLEMENTS="$ROOT/packaging/macos/entitlements.plist"
VERSION="$(sed -n 's/^#define KHENSU_VERSION_STRING "\(.*\)"/\1/p' "$ROOT/shared-core/include/khensu/common/Version.h")"

log()  { printf '\n==> %s\n' "$*"; }
skip() { printf '    (skipped: %s)\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "this script runs on macOS only"
command -v cmake >/dev/null || die "cmake not found"
command -v xcodebuild >/dev/null || die "Xcode command-line tools not found"
command -v pkgbuild >/dev/null || die "pkgbuild not found (Xcode command-line tools)"
PYTHON="${KHENSU_PYTHON:-python3}"

APP_SRC="$BUILD_DIR/desktop-app/KhensuDesktop_artefacts/$CONFIG/Khensu Mixing.app"
VST3_SRC="$BUILD_DIR/plugin-copilot/KhensuCopilot_artefacts/$CONFIG/VST3/Khensu Copilot.vst3"
AU_SRC="$BUILD_DIR/plugin-copilot/KhensuCopilot_artefacts/$CONFIG/AU/Khensu Copilot.component"

# --------------------------------------------------------------------------- #
# 1. Configure + build
# --------------------------------------------------------------------------- #
if [[ "${KHENSU_SKIP_BUILD:-0}" != "1" ]]; then
    log "Configure ($ARCHS, macOS >= $DEPLOY)"
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Xcode \
        -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOY" \
        -DKHENSU_BUILD_TESTS=OFF \
        -DFETCHCONTENT_BASE_DIR="${FETCHCONTENT_BASE_DIR:-$ROOT/.fetchcontent}"

    log "Build app + plug-ins"
    cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS" \
        --target KhensuDesktop --target KhensuCopilot_VST3
    # The AU format is added on Apple builds (plugin-copilot/CMakeLists.txt);
    # its failure is reported but does not block the app + VST3 package.
    cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS" --target KhensuCopilot_AU \
        || printf '    warning: the AU target did not build - packaging without it\n'
fi

[[ -d "$APP_SRC" ]]  || die "app bundle not found: $APP_SRC"
[[ -d "$VST3_SRC" ]] || die "VST3 bundle not found: $VST3_SRC"

# --------------------------------------------------------------------------- #
# 2. Stage + verify universal binaries
# --------------------------------------------------------------------------- #
log "Stage bundles into $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/plugins"
APP="$DIST_DIR/Khensu Mixing.app"
ditto "$APP_SRC" "$APP"
ditto "$VST3_SRC" "$DIST_DIR/plugins/Khensu Copilot.vst3"
HAVE_AU=0
if [[ -d "$AU_SRC" ]]; then
    ditto "$AU_SRC" "$DIST_DIR/plugins/Khensu Copilot.component"
    HAVE_AU=1
else
    skip "no AU bundle was built"
fi

check_universal() {
    local bin="$1" archs
    archs="$(lipo -archs "$bin" 2>/dev/null || true)"
    for want in ${ARCHS//;/ }; do
        [[ " $archs " == *" $want "* ]] || die "$bin is missing $want (has: ${archs:-none})"
    done
    printf '    %s: %s\n' "$(basename "$bin")" "$archs"
}
log "Verify architectures"
check_universal "$APP/Contents/MacOS/Khensu Mixing"
check_universal "$DIST_DIR/plugins/Khensu Copilot.vst3/Contents/MacOS/Khensu Copilot"
[[ $HAVE_AU == 1 ]] && check_universal "$DIST_DIR/plugins/Khensu Copilot.component/Contents/MacOS/Khensu Copilot"

# --------------------------------------------------------------------------- #
# 3. The analysis service inside the app
# --------------------------------------------------------------------------- #
log "Bundle local-services into the app"
# The same allow-listed stage the Windows installer packages (tracked files
# only, never a .venv, test scratch, settings or keys), not the working tree.
"$PYTHON" "$ROOT/scripts/stage_local_services.py" --check
SVC="$APP/Contents/Resources/local-services"
mkdir -p "$SVC"
rsync -a --delete "$ROOT/build/packaging/local-services/" "$SVC/"
mkdir -p "$APP/Contents/Resources/resources"
rsync -a --exclude 'desktop.ini' "$ROOT/resources/" "$APP/Contents/Resources/resources/"

if [[ -n "${KHENSU_PYTHON_RUNTIME_DIR:-}" ]]; then
    log "Stage Python runtime from $KHENSU_PYTHON_RUNTIME_DIR"
    [[ -x "$KHENSU_PYTHON_RUNTIME_DIR/bin/python3" ]] || die "no bin/python3 in KHENSU_PYTHON_RUNTIME_DIR"
    ditto "$KHENSU_PYTHON_RUNTIME_DIR" "$SVC/runtime"
    # The hash-pinned runtime set (the same lock the Windows installer uses).
    "$SVC/runtime/bin/python3" -m pip install --no-deps --require-hashes \
        -r "$ROOT/local-services/requirements-runtime-hashed.txt"
    "$SVC/runtime/bin/python3" -m pip install --no-deps "$SVC"
    "$SVC/runtime/bin/python3" "$SVC/launch_service.py" --smoke
else
    skip "KHENSU_PYTHON_RUNTIME_DIR unset - the app will use a system Python 3.11+"
fi

log "Secret scan of what ships"
"$PYTHON" "$ROOT/scripts/scan_release_secrets.py" "$APP" "$DIST_DIR/plugins"

# --------------------------------------------------------------------------- #
# 4. Codesign (hardened runtime)
# --------------------------------------------------------------------------- #
# MAC-1: every build is signed. Without a Developer ID it is signed ad hoc
# ("-", this Mac only): Apple Silicon will not run unsigned code, and copying
# the service and data files in after the build breaks the linker's seal.
KHENSU_CODESIGN_IDENTITY="${KHENSU_CODESIGN_IDENTITY:--}"
sign() {
    if [[ "$KHENSU_CODESIGN_IDENTITY" == "-" ]]; then
        codesign --force --options runtime --sign - "$@"   # ad hoc: no timestamp
    else
        codesign --force --timestamp --options runtime --sign "$KHENSU_CODESIGN_IDENTITY" "$@"
    fi
}
if [[ -n "${KHENSU_CODESIGN_IDENTITY:-}" ]]; then
    log "Codesign with: $KHENSU_CODESIGN_IDENTITY"
    # Inside-out: every nested Mach-O first (the staged Python runtime holds
    # hundreds of .so / .dylib files), then the bundles.
    while IFS= read -r -d '' f; do
        if file -b "$f" | grep -q "Mach-O"; then sign "$f"; fi
    done < <(find "$APP/Contents/Resources" -type f \( -name '*.so' -o -name '*.dylib' -o -perm -u+x \) -print0)
    sign --entitlements "$ENTITLEMENTS" "$APP"
    sign "$DIST_DIR/plugins/Khensu Copilot.vst3"
    [[ $HAVE_AU == 1 ]] && sign "$DIST_DIR/plugins/Khensu Copilot.component"
    codesign --verify --deep --strict --verbose=2 "$APP"
else
    skip "KHENSU_CODESIGN_IDENTITY unset - bundles are unsigned (Gatekeeper will block them)"
fi

# --------------------------------------------------------------------------- #
# 5. Package
# --------------------------------------------------------------------------- #
log "Package"
PKG="$DIST_DIR/khensu-setup-$VERSION-macos.pkg"
DMG="$DIST_DIR/Khensu-Mixing-$VERSION-macos-universal.dmg"
PLUGINS_ZIP="$DIST_DIR/Khensu-Copilot-$VERSION-macos-universal.zip"

# Component packages: each installs one bundle to its standard folder. With
# the distribution's currentUserHome domain ("Install for me only") the same
# paths land under ~/ (~/Applications, ~/Library/Audio/Plug-Ins/...).
PKGWORK="$(mktemp -d)"
component() { # <id> <bundle> <install-location>
    local id="$1" bundle="$2" location="$3" root="$PKGWORK/root-$1"
    mkdir -p "$root"
    ditto "$bundle" "$root/$(basename "$bundle")"
    pkgbuild --analyze --root "$root" "$PKGWORK/$id.plist" >/dev/null
    # Install where the user chose, not over a copy they moved elsewhere.
    plutil -replace 0.BundleIsRelocatable -bool NO "$PKGWORK/$id.plist"
    pkgbuild --root "$root" --component-plist "$PKGWORK/$id.plist" \
        --identifier "com.khensu.mixing.$id" --version "$VERSION" \
        --install-location "$location" "$PKGWORK/$id.pkg"
}
component app "$APP" /Applications
component vst3 "$DIST_DIR/plugins/Khensu Copilot.vst3" /Library/Audio/Plug-Ins/VST3
AU_CHOICE=""
AU_LINE=""
if [[ $HAVE_AU == 1 ]]; then
    component au "$DIST_DIR/plugins/Khensu Copilot.component" /Library/Audio/Plug-Ins/Components
    AU_CHOICE='<choice id="au" title="Khensu Copilot (Audio Unit)" description="For Logic Pro, GarageBand and other AU hosts."><pkg-ref id="com.khensu.mixing.au"/></choice>'
    AU_LINE='<line choice="au"/>'
fi
cat > "$PKGWORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Khensu Mixing $VERSION</title>
    <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_currentUserHome="true" enable_localSystem="true"/>
    <os-version min="$DEPLOY"/>
    <choices-outline>
        <line choice="app"/>
        <line choice="vst3"/>
        $AU_LINE
    </choices-outline>
    <choice id="app" title="Khensu Mixing (app)" description="The standalone app, in Applications."><pkg-ref id="com.khensu.mixing.app"/></choice>
    <choice id="vst3" title="Khensu Copilot (VST3)" description="The plug-in, in Library/Audio/Plug-Ins/VST3."><pkg-ref id="com.khensu.mixing.vst3"/></choice>
    $AU_CHOICE
    <pkg-ref id="com.khensu.mixing.app" version="$VERSION">app.pkg</pkg-ref>
    <pkg-ref id="com.khensu.mixing.vst3" version="$VERSION">vst3.pkg</pkg-ref>
    $( [[ $HAVE_AU == 1 ]] && echo "<pkg-ref id=\"com.khensu.mixing.au\" version=\"$VERSION\">au.pkg</pkg-ref>" )
</installer-gui-script>
XML
pkg_sign=()
[[ -n "${KHENSU_INSTALLER_IDENTITY:-}" ]] && pkg_sign=(--sign "$KHENSU_INSTALLER_IDENTITY" --timestamp)
productbuild --distribution "$PKGWORK/distribution.xml" --package-path "$PKGWORK" "${pkg_sign[@]}" "$PKG"
rm -rf "$PKGWORK"

# Drag-and-drop alternative: the app and the plug-ins, with shortcuts to the
# standard folders, for anyone who wants them somewhere else.
STAGE="$(mktemp -d)"
ditto "$APP" "$STAGE/Khensu Mixing.app"
ditto "$DIST_DIR/plugins" "$STAGE/Plug-ins"
ln -s /Applications "$STAGE/Applications"
ln -s /Library/Audio/Plug-Ins/VST3 "$STAGE/VST3 Plug-Ins folder"
hdiutil create -volname "Khensu Mixing $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"
(cd "$DIST_DIR/plugins" && ditto -c -k --keepParent . "$PLUGINS_ZIP")
[[ "${KHENSU_CODESIGN_IDENTITY:--}" != "-" ]] && codesign --force --timestamp --sign "$KHENSU_CODESIGN_IDENTITY" "$DMG"

# --------------------------------------------------------------------------- #
# 6. Notarize + staple
# --------------------------------------------------------------------------- #
notary_args=()
if [[ -n "${KHENSU_NOTARY_PROFILE:-}" ]]; then
    notary_args=(--keychain-profile "$KHENSU_NOTARY_PROFILE")
elif [[ -n "${KHENSU_APPLE_ID:-}" && -n "${KHENSU_TEAM_ID:-}" && -n "${KHENSU_APP_PASSWORD:-}" ]]; then
    notary_args=(--apple-id "$KHENSU_APPLE_ID" --team-id "$KHENSU_TEAM_ID" --password "$KHENSU_APP_PASSWORD")
fi
if [[ ${#notary_args[@]} -gt 0 && "${KHENSU_CODESIGN_IDENTITY:--}" != "-" ]]; then  # ad hoc cannot be notarized
    log "Notarize"
    xcrun notarytool submit "$PKG" "${notary_args[@]}" --wait
    xcrun notarytool submit "$DMG" "${notary_args[@]}" --wait
    xcrun notarytool submit "$PLUGINS_ZIP" "${notary_args[@]}" --wait
    xcrun stapler staple "$PKG"
    xcrun stapler staple "$DMG"
    xcrun stapler staple "$DIST_DIR/plugins/Khensu Copilot.vst3"
    [[ $HAVE_AU == 1 ]] && xcrun stapler staple "$DIST_DIR/plugins/Khensu Copilot.component"
    # Re-zip so the archive carries the stapled tickets.
    rm -f "$PLUGINS_ZIP"
    (cd "$DIST_DIR/plugins" && ditto -c -k --keepParent . "$PLUGINS_ZIP")
else
    skip "notary credentials or signing identity unset - not notarized"
fi

log "Checksums"
(cd "$DIST_DIR" && shasum -a 256 "$(basename "$PKG")" "$(basename "$DMG")" "$(basename "$PLUGINS_ZIP")" | tee SHA256SUMS.txt)
log "Done: $DIST_DIR"
