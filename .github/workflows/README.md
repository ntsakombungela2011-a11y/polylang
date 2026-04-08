# PolyLang — Vendored Dependencies

This directory contains `CMakeLists.txt` which builds all C/C++ library
dependencies **from source** so CI never relies on `apt-get` or system
package managers for the core libraries.

## What is vendored

| Library | Version | Used by adapter |
|---------|---------|-----------------|
| Lua | 5.4.7 | `lua` |
| QuickJS | 2024-01-13 | `javascript`, `typescript` |
| Wren | 0.4.0 | `wren` |
| Squirrel | 3.2 | `squirrel` |
| AngelScript | 2.36.1 | `angelscript` |

## What is NOT vendored (handled elsewhere)

| Dependency | How it's set up |
|------------|-----------------|
| godot-cpp | Downloaded as tarball from GitHub, cached per-branch |
| Python 3.10+ | Pre-installed on GitHub runners / `actions/setup-python` |
| Java/JVM | `actions/setup-java` |
| Rust stdlib | `dtolnay/rust-toolchain` |
| Zig | `goto-bus-stop/setup-zig` |
| Go | Pre-installed on ubuntu-24.04 runners |
| Nim | `apt-get` (small, reliable package) |
| .NET | Pre-installed on ubuntu-24.04 and windows-2022 runners |

## How CI uses this

Each platform workflow does:

```
cmake -S vendor -B vendor/build[-platform] \
      [-DCMAKE_TOOLCHAIN_FILE=... for Android] \
      -DCMAKE_INSTALL_PREFIX=vendor/install[-platform]
cmake --build vendor/build[-platform] --parallel
cmake --install vendor/build[-platform]
```

Then passes `-DCMAKE_PREFIX_PATH=vendor/install[-platform]` to the main
PolyLang cmake configure step. This causes cmake's `find_library()` and
`find_path()` calls to find the vendored builds first.

The built output is cached in CI using the hash of `vendor/CMakeLists.txt`
as the cache key, so it only rebuilds when you change a dependency version.

## Updating a dependency version

1. Edit `vendor/CMakeLists.txt` — change the `URL` / `GIT_TAG` and update
   the `URL_HASH` (run `sha256sum` on the downloaded tarball to get it).
2. Bump the cache key suffix (`-v1` → `-v2`) in each `build-*.yml` workflow
   under the `Cache vendored deps` step so the old cache is invalidated.
3. Commit. CI will rebuild the vendor deps on the next run and cache the result.

## AngelScript hash

The AngelScript `URL_HASH` in `CMakeLists.txt` is marked as a placeholder.
Download the zip from `https://www.angelcode.com/angelscript/` and run:

```sh
sha256sum angelscript_2.36.1.zip
```

Then update the `URL_HASH SHA256=...` line in `vendor/CMakeLists.txt`.
