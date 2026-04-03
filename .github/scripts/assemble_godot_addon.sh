#!/usr/bin/env bash
set -euo pipefail

mkdir -p godot_addon/addons/polylang/bin/linux
mkdir -p godot_addon/addons/polylang/bin/android
mkdir -p godot_addon/addons/polylang/bin/windows
mkdir -p godot_addon/addons/polylang/include

cp release/linux/polylang.so   godot_addon/addons/polylang/bin/linux/ 2>/dev/null || echo "Linux polylang.so not available"
cp release/android/polylang.so godot_addon/addons/polylang/bin/android/ 2>/dev/null || echo "Android polylang.so not available"
cp release/windows/polylang.dll godot_addon/addons/polylang/bin/windows/ 2>/dev/null || echo "Windows polylang.dll not available"

cp release/linux/libpolylang_odin.so    godot_addon/addons/polylang/bin/linux/  2>/dev/null || true
cp release/android/libpolylang_odin.so  godot_addon/addons/polylang/bin/android/ 2>/dev/null || true

find release/linux -name "libpolylang_adapter_*.so" -exec cp {} godot_addon/addons/polylang/bin/linux/ \; 2>/dev/null || true

cp -r "polylang plugin/addons/polylang/." godot_addon/addons/polylang/ 2>/dev/null || true
cp "polylang plugin/include/pl_adapter_vtable.h" godot_addon/addons/polylang/include/ 2>/dev/null || true

echo "=== Release layout ==="
find godot_addon -type f | sort
