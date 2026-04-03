#!/usr/bin/env bash
set -euo pipefail

echo "=== Checking for artifacts ==="
ls -la release/ || true

test -f release/linux/polylang.so && echo "✓ Linux artifact found" || echo "⊘ Linux artifact missing"
test -f release/android/polylang.so && echo "✓ Android artifact found" || echo "⊘ Android artifact missing"
test -f release/windows/polylang.dll && echo "✓ Windows artifact found" || echo "⊘ Windows artifact missing"
