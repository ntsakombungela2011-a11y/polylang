# PolyLang — Vendored Dependencies

Last updated: 2026-04-11 11:57 UTC

| Dependency | Version | Location |
|------------|---------|----------|
| godot-cpp | godot-4.3-stable | `polylang plugin/godot-cpp` (submodule) |
| Lua | 5.4.7 | `vendor/lua/` |
| QuickJS | 2024-01-13 | `vendor/quickjs/` |
| Wren | 0.4.0 | `vendor/wren/` |
| Squirrel | v3.2 | `vendor/squirrel/` |
| AngelScript | 2.36.1 | `vendor/angelscript/` |
| Nim | 2.0.8 | `vendor/nim/` |
| Julia | 1.10.4 | `vendor/julia/` |
| hxcpp | latest | `vendor/hxcpp/` |

## godot-cpp
Managed as a git submodule at `polylang plugin/godot-cpp`.
Pinned to tag `godot-4.3-stable` — must match your Godot engine version.
To update: run this workflow again with a different `godot_cpp_tag`.

## Updating a dependency
Re-run this workflow (Actions → Vendor Dependencies → Run workflow)
with the new version numbers. It will reset the vendor branch and open
a fresh PR.
