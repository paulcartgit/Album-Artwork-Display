#!/bin/zsh
# Build the webcam capture helper used by the calibration workflow.
#
# It has to be an app bundle, not a bare binary. macOS refuses camera access to
# any process without an NSCameraUsageDescription and denies it instantly,
# without ever showing a prompt — which looks exactly like a broken permission.
# It also has to be launched via LaunchServices (`open -a`), so that TCC treats
# the bundle as the responsible process rather than whatever spawned it.
#
#   ./build-snap.sh          then:   open -a "$PWD/Snap.app" --args out.jpg 3
set -e
cd "$(dirname "$0")"

swiftc -O snap.swift -o snap

rm -rf Snap.app
mkdir -p Snap.app/Contents/MacOS
cp snap Snap.app/Contents/MacOS/snap

cat > Snap.app/Contents/Info.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Snap</string>
    <key>CFBundleDisplayName</key><string>Album Artwork Calibration Capture</string>
    <key>CFBundleIdentifier</key><string>local.albumartwork.snap</string>
    <key>CFBundleExecutable</key><string>snap</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleVersion</key><string>1.0</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>LSUIElement</key><true/>
    <key>NSCameraUsageDescription</key>
    <string>Photographs the e-ink display to calibrate its colour palette.</string>
</dict>
</plist>
PLIST

codesign --force --sign - --identifier local.albumartwork.snap Snap.app
xattr -dr com.apple.quarantine Snap.app 2>/dev/null || true

echo "Built Snap.app — first run will ask for camera permission."
