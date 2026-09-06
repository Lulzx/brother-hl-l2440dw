#!/bin/sh
# Wrap the SwiftPM executable in a .app. SwiftPM emits a bare binary; a SwiftUI
# app needs a bundle for its menu bar, Dock tile and activation policy.
set -eu
cd "$(dirname "$0")"
CONFIG=${1:-release}
swift build -c "$CONFIG"
BIN="$(swift build -c "$CONFIG" --show-bin-path)/BrhbpApp"
APP="build/Brhbp.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Brhbp"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>              <string>Brhbp</string>
  <key>CFBundleDisplayName</key>       <string>Brhbp Print</string>
  <key>CFBundleIdentifier</key>        <string>dev.brhbp.mac</string>
  <key>CFBundleExecutable</key>        <string>Brhbp</string>
  <key>CFBundlePackageType</key>       <string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>CFBundleVersion</key>           <string>1</string>
  <key>LSMinimumSystemVersion</key>    <string>26.0</string>
  <key>NSHighResolutionCapable</key>   <true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key>     <string>PDF</string>
      <key>CFBundleTypeRole</key>     <string>Viewer</string>
      <key>LSItemContentTypes</key>   <array><string>com.adobe.pdf</string></array>
    </dict>
  </array>
  <key>NSLocalNetworkUsageDescription</key>
  <string>Brhbp sends print jobs directly to a printer on your local network.</string>
</dict>
</plist>
PLIST

# Ad-hoc sign so the local network prompt and Dock behaviour work.
codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1 || true
echo "built $APP"
