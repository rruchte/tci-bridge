#!/usr/bin/env bash
# Package a relocatable macOS command-line executable with Qt frameworks/plugins.
set -euo pipefail

binary="${1:?path to built tci-bridge required}"
destination="${2:?destination directory required}"
binary="$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")"
mkdir -p "$destination/tci-bridge.app/Contents/MacOS"
app="$destination/tci-bridge.app"
cp "$binary" "$app/Contents/MacOS/tci-bridge"

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>tci-bridge</string>
  <key>CFBundleIdentifier</key><string>com.rruchte.tci-bridge</string>
  <key>CFBundleName</key><string>tci-bridge</string>
  <key>CFBundlePackageType</key><string>APPL</string>
</dict>
</plist>
PLIST

qt_prefix="$(brew --prefix qt)"
plugin_dir="$("$qt_prefix/bin/qtpaths" --query QT_INSTALL_PLUGINS)"
if [[ ! -d "$plugin_dir/multimedia" ]]; then
  echo "ERROR: Qt multimedia plugins missing at $plugin_dir/multimedia" >&2
  exit 1
fi

# A headless bridge needs multimedia backends, not Qt's entire QML, image,
# icon, platform input or PDF plugin collection. macdeployqt's default plugin
# deployment pulls those in on Homebrew and leaves broken transitive paths.
mkdir -p "$app/Contents/PlugIns"
cp -RL "$plugin_dir/multimedia" "$app/Contents/PlugIns/multimedia"
plugin_executables=()
while IFS= read -r -d '' plugin; do
  plugin_executables+=("-executable=$plugin")
done < <(find -L "$app/Contents/PlugIns/multimedia" -name '*.dylib' -type f -print0)
if (( ${#plugin_executables[@]} == 0 )); then
  echo "ERROR: No Qt multimedia plugin dylibs found in $plugin_dir/multimedia" >&2
  find -L "$plugin_dir/multimedia" -maxdepth 2 -print >&2
  exit 1
fi
"$qt_prefix/bin/macdeployqt" "$app" -no-plugins "${plugin_executables[@]}" -verbose=1

# macdeployqt may not copy Homebrew's separate yaml-cpp dependency.
yaml_dep="$(otool -L "$app/Contents/MacOS/tci-bridge" | awk '/libyaml-cpp.*\.dylib/ { print $1; exit }')"
if [[ -n "$yaml_dep" && "$yaml_dep" == /* ]]; then
  mkdir -p "$app/Contents/Frameworks"
  yaml_name="$(basename "$yaml_dep")"
  cp -L "$yaml_dep" "$app/Contents/Frameworks/$yaml_name"
  chmod u+w "$app/Contents/Frameworks/$yaml_name"
  install_name_tool -id "@rpath/$yaml_name" "$app/Contents/Frameworks/$yaml_name"
  install_name_tool -change "$yaml_dep" "@executable_path/../Frameworks/$yaml_name" "$app/Contents/MacOS/tci-bridge"
fi

# The distribution retains the familiar ./tci-bridge command.
# Starting the actual Mach-O inside the .app enables @executable_path Qt paths.
cat > "$destination/tci-bridge" <<'LAUNCHER'
#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$here/tci-bridge.app/Contents/MacOS/tci-bridge" "$@"
LAUNCHER
chmod +x "$destination/tci-bridge"

# Relocate every copied Mach-O dependency, including Homebrew's embedded Qt
# framework IDs and non-Qt shared libraries required by multimedia plugins.
python3 tools/relocate-macos.py "$app"

# Ad-hoc sign after all install_name_tool changes (required on Apple Silicon).
codesign --force --deep --sign - "$app"

# Exercise the packaged executable, not the original build tree.
"$destination/tci-bridge" --version
"$destination/tci-bridge" --help
"$destination/tci-bridge" --list-audio-devices
