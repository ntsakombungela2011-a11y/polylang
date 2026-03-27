#!/usr/bin/env bash
# =============================================================
# odin_build_pipeline_android.sh  —  PolyLang v6.3 Odin Android Cross-Compile
# Usage: odin_build_pipeline_android.sh <source.pl.odin> <output.so>
# =============================================================
# Cross-compiles a .pl.odin script for Android arm64-v8a.
#
# Odin supports -target:linux_arm64 which produces ARM64 Linux .so
# compatible with Android arm64-v8a (same ELF ABI, same syscall layer).
#
# Requirements:
#   - odin >= dev-2024-12 (stable Linux arm64 cross-compile support)
#   - Android NDK (for linker; sysroot path set via ANDROID_NDK_HOME)
#
# Environment variables:
#   ANDROID_NDK_HOME    — path to Android NDK root (required)
#   ANDROID_API_LEVEL   — minimum API level (default: 28)
#   POLYLANG_ODIN_SHIM_PATH — directory with shim .odin files
#   POLYLANG_ODIN_CACHE — cache directory
#   ODIN_BIN            — path to odin compiler
#   POLYLANG_DEBUG      — if set, enables debug mode
# =============================================================
set -euo pipefail

SRC="${1:-}"
OUT="${2:-}"

if [[ -z "$SRC" || -z "$OUT" ]]; then
    echo "[PolyLang/Odin/Android] Usage: $0 <source.pl.odin> <output.so>" >&2
    exit 1
fi

if [[ ! -f "$SRC" ]]; then
    echo "[PolyLang/Odin/Android] Source not found: $SRC" >&2
    exit 1
fi

NDK_HOME="${ANDROID_NDK_HOME:-}"
if [[ -z "$NDK_HOME" ]]; then
    echo "[PolyLang/Odin/Android] ANDROID_NDK_HOME not set" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIM_PATH="${POLYLANG_ODIN_SHIM_PATH:-$SCRIPT_DIR}"
ODIN="${ODIN_BIN:-odin}"
CACHE="${POLYLANG_ODIN_CACHE:-/tmp/polylang_odin_cache}"
API_LEVEL="${ANDROID_API_LEVEL:-28}"

mkdir -p "$CACHE"

SRC_ABS="$(realpath "$SRC")"
SRC_BASE="$(basename "$SRC_ABS")"

# DZ-10: Prevent filename shadowing of security-critical shims
if [[ "$SRC_BASE" == "polylang_odin_shim.odin" || "$SRC_BASE" == "polylang_odin_script_api.odin" ]]; then
    echo "[PolyLang/Odin/Android] FATAL: Source file cannot be named '$SRC_BASE' to prevent shim shadowing." >&2
    exit 1
fi

# ── NDK toolchain paths ───────────────────────────────────────
# NDK provides a standalone linker for arm64-v8a Linux.
NDK_TOOLCHAIN="$NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
NDK_SYSROOT="$NDK_TOOLCHAIN/sysroot"
NDK_LINKER="$NDK_TOOLCHAIN/bin/aarch64-linux-android${API_LEVEL}-clang"

if [[ ! -x "$NDK_LINKER" ]]; then
    echo "[PolyLang/Odin/Android] NDK linker not found: $NDK_LINKER" >&2
    echo "  Ensure ANDROID_NDK_HOME points to a valid NDK and API_LEVEL ($API_LEVEL) is installed." >&2
    exit 1
fi

# ── Optimization flags ────────────────────────────────────────
if [[ -n "${POLYLANG_DEBUG:-}" ]]; then
    OPT_FLAG="-opt:none"
    DEBUG_FLAG="-debug"
else
    OPT_FLAG="-opt:speed"
    DEBUG_FLAG=""
fi

# ── Build directory ───────────────────────────────────────────
HASH="$(echo -n "${SRC_ABS}_android" | sha256sum | cut -c1-16)"
BUILD_DIR="$CACHE/android_${HASH}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cp "$SHIM_PATH/polylang_odin_shim.odin"       "$BUILD_DIR/"
cp "$SHIM_PATH/polylang_odin_script_api.odin" "$BUILD_DIR/"
cp "$SRC_ABS" "$BUILD_DIR/${SRC_BASE}"

echo "[PolyLang/Odin/Android] Cross-compiling $SRC_BASE → $OUT (API=$API_LEVEL, arm64-v8a)"

# ── Odin cross-compile ────────────────────────────────────────
# Odin -target:linux_arm64 produces ARM64 Linux ELF shared library.
# We pass the NDK linker via -linker so the resulting .so has correct
# Android ABI annotations and links against the right libc/libdl.
"$ODIN" build "$BUILD_DIR"          \
    -build-mode:shared               \
    -target:linux_arm64              \
    -out:"$OUT"                      \
    $OPT_FLAG                        \
    ${DEBUG_FLAG}                    \
    -no-entry-point                  \
    -extra-linker-flags:"-fuse-ld=$NDK_TOOLCHAIN/bin/ld.lld \
        --sysroot=$NDK_SYSROOT        \
        -target aarch64-linux-android${API_LEVEL} \
        -Wl,-z,relro,-z,now \
        -lc -lm -ldl"

RC=$?
rm -rf "$BUILD_DIR"

if [[ $RC -ne 0 ]]; then
    echo "[PolyLang/Odin/Android] Build FAILED (rc=$RC)" >&2
    exit $RC
fi

echo "[PolyLang/Odin/Android] Build OK → $OUT"

# ── Verify the output ─────────────────────────────────────────
if command -v "$NDK_TOOLCHAIN/bin/llvm-readelf" &>/dev/null; then
    echo "[PolyLang/Odin/Android] Verifying exports..."
    "$NDK_TOOLCHAIN/bin/llvm-readelf" --syms "$OUT" 2>/dev/null \
        | grep -E "odin_script_|polylang_script_factory|pl_get_vtable" \
        | grep -v "UND" \
        | awk '{print "  exported: " $NF}'
fi

echo "[PolyLang/Odin/Android] Done."
