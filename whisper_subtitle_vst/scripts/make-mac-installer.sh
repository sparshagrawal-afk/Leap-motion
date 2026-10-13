#!/usr/bin/env bash
# Builds ONE file to give to someone: "Whisper Subtitles Installer.pkg".
# Run this on a Mac (with Xcode command line tools + CMake). The person you give the
# .pkg to needs nothing else: they double-click it and it installs
#   - Whisper Subtitles (AU + VST3) and Whisper Subtitles ARA (AU + VST3) for all users
#   - the Whisper Subtitles standalone app in /Applications
#   - the Whisper speech model (downloaded during install, or bundled with --bundle-model)
# The plugins pick the model up automatically, so there is nothing to set up.
#
# Usage:
#   ./scripts/make-mac-installer.sh                  # Apple Silicon + Intel, model downloaded at install
#   ./scripts/make-mac-installer.sh --bundle-model   # model inside the .pkg (~3 GB file, works offline)
#   ./scripts/make-mac-installer.sh --arm64-only     # faster build, Apple Silicon Macs only
#   ./scripts/make-mac-installer.sh --model medium   # smaller model (1.5 GB) instead of large-v3
#
# Optional signing (removes the "unidentified developer" warning), if you have an Apple Developer ID:
#   DEVELOPER_ID_INSTALLER="Developer ID Installer: Your Name (TEAMID)" \
#   DEVELOPER_ID_APP="Developer ID Application: Your Name (TEAMID)" ./scripts/make-mac-installer.sh
set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "This script builds a macOS installer and must run on a Mac." >&2
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(WhisperSubtitles VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
ARCHS=(arm64 x86_64)
MODEL="large-v3"
BUNDLE_MODEL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arm64-only)   ARCHS=(arm64) ;;
        --bundle-model) BUNDLE_MODEL=1 ;;
        --model)        MODEL="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

WORK="$ROOT/build-installer"
OUT="$ROOT/dist"
PKG_NAME="Whisper Subtitles Installer.pkg"
IDENTIFIER="com.leapmotion.whispersubtitles"
MODEL_DIR="/Users/Shared/Whisper Subtitles/models"
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${MODEL}.bin"

# name in build folder | destination in the payload
BUNDLES=(
    "WhisperSubtitles_artefacts/Release/AU/Whisper Subtitles.component|Library/Audio/Plug-Ins/Components"
    "WhisperSubtitles_artefacts/Release/VST3/Whisper Subtitles.vst3|Library/Audio/Plug-Ins/VST3"
    "WhisperSubtitlesARA_artefacts/Release/AU/Whisper Subtitles ARA.component|Library/Audio/Plug-Ins/Components"
    "WhisperSubtitlesARA_artefacts/Release/VST3/Whisper Subtitles ARA.vst3|Library/Audio/Plug-Ins/VST3"
    "WhisperSubtitles_artefacts/Release/Standalone/Whisper Subtitles.app|Applications"
)

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

mkdir -p "$WORK"

#-------------------------------------------------------------------------------
for arch in "${ARCHS[@]}"; do
    step "Building for $arch (the first build downloads JUCE, whisper.cpp and the ARA SDK)"
    cmake -S "$ROOT" -B "$WORK/build-$arch" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_OSX_ARCHITECTURES="$arch" \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DWHISPER_SUBTITLES_ARA=ON > "$WORK/configure-$arch.log" 2>&1 \
        || { tail -30 "$WORK/configure-$arch.log"; exit 1; }
    cmake --build "$WORK/build-$arch" --config Release -j "$(sysctl -n hw.ncpu)" \
          --target WhisperSubtitles_AU WhisperSubtitles_VST3 WhisperSubtitles_Standalone \
                   WhisperSubtitlesARA_AU WhisperSubtitlesARA_VST3
done

#-------------------------------------------------------------------------------
step "Assembling the installer contents"
PAYLOAD="$WORK/payload"
rm -rf "$PAYLOAD" "$WORK/scripts" "$WORK/resources" "$WORK"/*.pkg
mkdir -p "$PAYLOAD" "$WORK/scripts" "$WORK/resources" "$OUT"

for entry in "${BUNDLES[@]}"; do
    src="${entry%%|*}"
    dest="$PAYLOAD/${entry##*|}"
    bundle="$dest/$(basename "$src")"
    mkdir -p "$dest"
    ditto "$WORK/build-${ARCHS[0]}/$src" "$bundle"

    # Merge the Intel and Apple Silicon executables into universal ones
    if [[ ${#ARCHS[@]} -gt 1 ]]; then
        for exe in "$bundle/Contents/MacOS/"*; do
            rel="${exe#"$bundle"/}"
            lipo -create "$WORK/build-arm64/$src/$rel" "$WORK/build-x86_64/$src/$rel" -output "$exe"
        done
    fi

    if [[ -n "${DEVELOPER_ID_APP:-}" ]]; then
        codesign --force --deep --timestamp --options runtime --sign "$DEVELOPER_ID_APP" "$bundle"
    else
        codesign --force --deep --sign - "$bundle" # ad-hoc: fine for local use
    fi

    echo "  $(basename "$bundle"): $(lipo -archs "$bundle/Contents/MacOS/"* | head -1)"
done

if [[ $BUNDLE_MODEL -eq 1 ]]; then
    step "Adding the ggml-$MODEL model to the package"
    "$ROOT/scripts/download-model.sh" "$MODEL"
    mkdir -p "$PAYLOAD$MODEL_DIR"
    cp "$ROOT/models/ggml-$MODEL.bin" "$PAYLOAD$MODEL_DIR/"
fi

#-------------------------------------------------------------------------------
# Install scripts (run as root by macOS Installer)
cat > "$WORK/scripts/preinstall" <<'EOF'
#!/bin/bash
# Remove older versions so the new ones replace them cleanly.
rm -rf "/Library/Audio/Plug-Ins/Components/Whisper Subtitles.component" \
       "/Library/Audio/Plug-Ins/Components/Whisper Subtitles ARA.component" \
       "/Library/Audio/Plug-Ins/VST3/Whisper Subtitles.vst3" \
       "/Library/Audio/Plug-Ins/VST3/Whisper Subtitles ARA.vst3" \
       "/Applications/Whisper Subtitles.app"
exit 0
EOF

cat > "$WORK/scripts/postinstall" <<EOF
#!/bin/bash
MODEL_DIR="$MODEL_DIR"
MODEL_FILE="\$MODEL_DIR/ggml-$MODEL.bin"
MODEL_URL="$MODEL_URL"

mkdir -p "\$MODEL_DIR"

# Download the speech model unless it's already there (bundled, or from an earlier install).
if [ ! -f "\$MODEL_FILE" ] || [ "\$(stat -f%z "\$MODEL_FILE")" -lt 100000000 ]; then
    echo "Downloading \$MODEL_URL"
    if /usr/bin/curl -L --fail --retry 5 --retry-delay 5 -C - -o "\$MODEL_FILE.part" "\$MODEL_URL"; then
        mv -f "\$MODEL_FILE.part" "\$MODEL_FILE"
    else
        echo "Model download failed; the plugin's 'Load Whisper model...' button can be used later."
    fi
fi

# Let every user read the models and add their own.
chmod -R a+rwX "/Users/Shared/Whisper Subtitles"

# Make Logic & co. notice the new Audio Units straight away.
killall -9 AudioComponentRegistrar 2>/dev/null
exit 0
EOF
chmod +x "$WORK/scripts/preinstall" "$WORK/scripts/postinstall"

#-------------------------------------------------------------------------------
step "Building the package"
# Keep bundles at the install location even if copies exist elsewhere on the Mac
pkgbuild --analyze --root "$PAYLOAD" "$WORK/components.plist" > /dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$WORK/components.plist" > /dev/null 2>&1; do
    # newer pkgbuild versions sometimes leave the key out, so delete + add instead of Set
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$WORK/components.plist" > /dev/null 2>&1 || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$WORK/components.plist"
    i=$((i + 1))
done

pkgbuild --root "$PAYLOAD" \
         --component-plist "$WORK/components.plist" \
         --scripts "$WORK/scripts" \
         --identifier "$IDENTIFIER" \
         --version "$VERSION" \
         --install-location / \
         "$WORK/WhisperSubtitles.pkg"

if [[ $BUNDLE_MODEL -eq 1 ]]; then
    MODEL_NOTE="<p>The speech model is included, so no internet connection is needed.</p>"
else
    MODEL_NOTE="<p>The installer downloads the Whisper speech model (<b>ggml-$MODEL</b>, about 3&nbsp;GB for large-v3). This happens near the end, while the installer says <i>Running package scripts</i>. It can take several minutes, so please let it finish.</p>"
fi

cat > "$WORK/resources/welcome.html" <<EOF
<html><body style="font-family: -apple-system, Helvetica; font-size: 13px">
<h2>Whisper Subtitles $VERSION</h2>
<p>Audio plugins that turn dialogue into subtitles with Whisper, running entirely on this Mac.
They also translate Malayalam, Bengali, Punjabi and other languages into English.</p>
<p>This installs:</p>
<ul>
<li><b>Whisper Subtitles</b>: Audio Unit + VST3. Listens while you play the track.</li>
<li><b>Whisper Subtitles ARA</b>: Audio Unit + VST3. Transcribes whole clips without playing (Logic Pro, Studio One, Cubase, Reaper).</li>
<li><b>Whisper Subtitles.app</b>: a standalone version, in Applications.</li>
</ul>
$MODEL_NOTE
</body></html>
EOF

cat > "$WORK/resources/conclusion.html" <<'EOF'
<html><body style="font-family: -apple-system, Helvetica; font-size: 13px">
<h2>Ready to go</h2>
<ol>
<li>Open your DAW (restart it if it was open).</li>
<li>On the dialogue track, insert <b>Audio Units &gt; LeapMotion &gt; Whisper Subtitles ARA</b>.
In Logic Pro, use the <b>first</b> Audio FX slot. Or insert <b>Whisper Subtitles</b> and press play.</li>
<li>Pick the dialogue <b>Language</b> and, to get English subtitles, tick <b>Translate to English</b>.</li>
<li>Subtitles appear in the plugin. Click <b>Subtitle view</b> for a movable subtitle screen, or <b>Export .srt</b> to save them.</li>
</ol>
<p>The model is loaded automatically. Models are kept in <i>/Users/Shared/Whisper Subtitles/models</i>.</p>
</body></html>
EOF

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Whisper Subtitles</title>
    <welcome file="welcome.html"/>
    <conclusion file="conclusion.html"/>
    <options customize="never" require-scripts="false" hostArchitectures="$(IFS=,; echo "${ARCHS[*]}")"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" title="Whisper Subtitles">
        <pkg-ref id="$IDENTIFIER"/>
    </choice>
    <pkg-ref id="$IDENTIFIER" version="$VERSION" onConclusion="none">WhisperSubtitles.pkg</pkg-ref>
</installer-gui-script>
EOF

SIGN_ARGS=()
if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    SIGN_ARGS=(--sign "$DEVELOPER_ID_INSTALLER")
fi

productbuild --distribution "$WORK/distribution.xml" \
             --resources "$WORK/resources" \
             --package-path "$WORK" \
             ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} \
             "$OUT/$PKG_NAME"

step "Done"
ls -lh "$OUT/$PKG_NAME"
echo
echo "Send this file to your friend: $OUT/$PKG_NAME"
if [[ -z "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    echo "It isn't signed with an Apple Developer ID, so macOS will warn the first time it's opened:"
    echo "  double-click it, click Done, then System Settings > Privacy & Security > 'Open Anyway'."
fi
