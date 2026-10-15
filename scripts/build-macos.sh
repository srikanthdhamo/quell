#!/usr/bin/env bash
# Build Quell on a Mac (Apple Silicon or Intel) -> universal VST3, AU and app.
# Requires: Xcode command line tools (xcode-select --install) and CMake (brew install cmake).
# Usage: ./scripts/build-macos.sh [--install]
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION=$(grep -m1 -oE 'project\(Quell VERSION [0-9.]+' CMakeLists.txt | awk '{print $3}')
BUILD=build-macos

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --config Release --parallel

"$BUILD/QuellEngineTest_artefacts/Release/QuellEngineTest"

ART="$BUILD/Quell_artefacts/Release"
echo "Architectures: $(lipo -archs "$ART/VST3/Quell.vst3/Contents/MacOS/Quell")"

for b in "$ART/VST3/Quell.vst3" "$ART/AU/Quell.component" "$ART/Standalone/Quell.app"; do
    codesign --force --deep --sign - "$b"
done

PKG="dist/Quell-$VERSION-macOS-Universal"
rm -rf "$PKG" "$PKG.zip"
mkdir -p "$PKG"
cp -R "$ART/VST3/Quell.vst3" "$ART/AU/Quell.component" "$ART/Standalone/Quell.app" "$PKG/"
cp packaging/INSTALL-macOS.txt "$PKG/INSTALL.txt"
ditto -c -k --keepParent "$PKG" "$PKG.zip"
echo "Packaged $PKG.zip"

if [[ "${1:-}" == "--install" ]]; then
    mkdir -p ~/Library/Audio/Plug-Ins/VST3 ~/Library/Audio/Plug-Ins/Components
    rm -rf ~/Library/Audio/Plug-Ins/VST3/Quell.vst3 ~/Library/Audio/Plug-Ins/Components/Quell.component
    cp -R "$ART/VST3/Quell.vst3" ~/Library/Audio/Plug-Ins/VST3/
    cp -R "$ART/AU/Quell.component" ~/Library/Audio/Plug-Ins/Components/
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    auval -v aufx Qul1 Qlau | tail -3
    echo "Installed VST3 + AU to ~/Library/Audio/Plug-Ins"
fi
