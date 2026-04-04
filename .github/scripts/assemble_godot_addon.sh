#!/usr/bin/env bash
set -euo pipefail

mkdir -p godot_addon/addons/polylang/bin/linux
mkdir -p godot_addon/addons/polylang/bin/android
mkdir -p godot_addon/addons/polylang/bin/windows
mkdir -p godot_addon/addons/polylang/include

copy_if_exists() {
  local source_path="$1"
  local dest_dir="$2"
  if [ -f "$source_path" ]; then
    cp "$source_path" "$dest_dir/"
  fi
}

copy_tree_matches() {
  local source_dir="$1"
  local pattern="$2"
  local dest_dir="$3"
  if [ -d "$source_dir" ]; then
    find "$source_dir" -maxdepth 1 -type f -name "$pattern" -exec cp {} "$dest_dir/" \;
  fi
}

copy_tree_matches release/linux "*.so" godot_addon/addons/polylang/bin/linux
copy_tree_matches release/android "*.so" godot_addon/addons/polylang/bin/android
copy_tree_matches release/windows "*.dll" godot_addon/addons/polylang/bin/windows

# Imported adapter libraries may live in the repo rather than the build output.
# Mirror the same platform-specific paths CMake uses so the addon zip includes
# any committed prebuilt adapters that were available at configure time.
copy_if_exists "polylang plugin/adapters/rust/target/release/libpolylang_rust.so" \
  godot_addon/addons/polylang/bin/linux
copy_if_exists "polylang plugin/adapters/zig/zig-out/lib/libpolylang_zig.so" \
  godot_addon/addons/polylang/bin/linux
copy_if_exists "polylang plugin/adapters/go/polylang_go_adapter.so" \
  godot_addon/addons/polylang/bin/linux
copy_if_exists "polylang plugin/adapters/nim/polylang_nim_adapter.so" \
  godot_addon/addons/polylang/bin/linux

copy_if_exists "polylang plugin/adapters/rust/target/aarch64-linux-android/release/libpolylang_rust.so" \
  godot_addon/addons/polylang/bin/android
copy_if_exists "polylang plugin/adapters/zig/zig-out/lib/libpolylang_zig_android.so" \
  godot_addon/addons/polylang/bin/android
copy_if_exists "polylang plugin/adapters/go/polylang_go_adapter_android.so" \
  godot_addon/addons/polylang/bin/android

copy_if_exists "polylang plugin/adapters/rust/target/release/polylang_rust.dll" \
  godot_addon/addons/polylang/bin/windows

cp -r "polylang plugin/addons/polylang/." godot_addon/addons/polylang/ 2>/dev/null || true
cp "polylang plugin/include/pl_adapter_vtable.h" godot_addon/addons/polylang/include/ 2>/dev/null || true

echo "=== Release layout ==="
find godot_addon -type f | sort
