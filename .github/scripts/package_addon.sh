#!/usr/bin/env bash
set -euo pipefail

VERSION=$(grep "VERSION" "polylang plugin/CMakeLists.txt" | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || echo "6.7.0")
if [ -z "$VERSION" ]; then
  VERSION="6.7.0"
fi

echo "Packaging version: $VERSION"
cd godot_addon
ZIP_NAME="polylang_v${VERSION}_addon.zip"
zip -r "../$ZIP_NAME" addons/
cd ..
sha256sum "$ZIP_NAME" > "SHA256SUMS.txt"
echo "=== SHA256 Checksum ==="
cat "SHA256SUMS.txt"
