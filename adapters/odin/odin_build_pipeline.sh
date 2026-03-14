#!/usr/bin/env bash
# =============================================================
# odin_build_pipeline.sh  —  PolyLang v6.3 Odin Script Compiler
# Usage: odin_build_pipeline.sh <source.pl.odin> <output.so>
# =============================================================
# Compiles a .pl.odin script into a shared library (.so).
#
# The script source must reside in a directory that also contains
# (or has access to via POLYLANG_ODIN_SHIM_PATH):
#   - polylang_odin_shim.odin
#   - polylang_odin_script_api.odin
#
# Environment variables:
#   POLYLANG_ODIN_SHIM_PATH — directory containing shim .odin files
#                              (default: same directory as this script)
#   POLYLANG_ODIN_CACHE     — cache directory for intermediate objects
#   ODIN_BIN                — path to odin compiler (default: odin)
#   ODIN_OPTIMIZE           — optimization level: none | minimal | size | speed | aggressive
#                              (default: speed for release, none for debug)
#   POLYLANG_DEBUG          — if set, enables debug symbols and disables opt
# =============================================================
set -euo pipefail

SRC="${1:-}"
OUT="${2:-}"

if [[ -z "$SRC" || -z "$OUT" ]]; then
    echo "[PolyLang/Odin] Usage: $0 <source.pl.odin> <output.so>" >&2
    exit 1
fi

if [[ ! -f "$SRC" ]]; then
    echo "[PolyLang/Odin] Source file not found: $SRC" >&2
    exit 1
fi

# ── Resolve paths ─────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIM_PATH="${POLYLANG_ODIN_SHIM_PATH:-$SCRIPT_DIR}"
ODIN="${ODIN_BIN:-odin}"
CACHE="${POLYLANG_ODIN_CACHE:-/tmp/polylang_odin_cache}"

mkdir -p "$CACHE"

SRC_ABS="$(realpath "$SRC")"
SRC_DIR="$(dirname "$SRC_ABS")"
SRC_BASE="$(basename "$SRC_ABS")"

# ── Determine optimization ────────────────────────────────────
if [[ -n "${POLYLANG_DEBUG:-}" ]]; then
    OPT_FLAG="-opt:none"
    DEBUG_FLAG="-debug"
else
    OPT="${ODIN_OPTIMIZE:-speed}"
    OPT_FLAG="-opt:${OPT}"
    DEBUG_FLAG=""
fi

# ── Scratch build directory ───────────────────────────────────
# We copy shim files into a temp package dir alongside the user script
# so odin sees them all in the same package.
HASH="$(echo -n "$SRC_ABS" | sha256sum | cut -c1-16)"
BUILD_DIR="$CACHE/build_${HASH}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Copy shim files into the build package
cp "$SHIM_PATH/polylang_odin_shim.odin"       "$BUILD_DIR/"
cp "$SHIM_PATH/polylang_odin_script_api.odin" "$BUILD_DIR/"

# Copy the user script
cp "$SRC_ABS" "$BUILD_DIR/${SRC_BASE}"

# ── Detect target ─────────────────────────────────────────────
HOST_ARCH="$(uname -m)"
case "$HOST_ARCH" in
    x86_64)  TARGET="linux_amd64"  ;;
    aarch64) TARGET="linux_arm64"  ;;
    armv7*)  TARGET="linux_arm32"  ;;
    *)       TARGET="linux_amd64"  ;;
esac

echo "[PolyLang/Odin] Compiling $SRC_BASE → $OUT (target=$TARGET, opt=${OPT_FLAG})"

# ── Build ─────────────────────────────────────────────────────
"$ODIN" build "$BUILD_DIR" \
    -build-mode:shared     \
    -target:"$TARGET"      \
    -out:"$OUT"            \
    $OPT_FLAG              \
    ${DEBUG_FLAG}          \
    -no-entry-point        \
    -vet                   \
    -strict-style

RC=$?
rm -rf "$BUILD_DIR"

if [[ $RC -ne 0 ]]; then
    echo "[PolyLang/Odin] Build FAILED (rc=$RC) for $SRC_BASE" >&2
    exit $RC
fi

echo "[PolyLang/Odin] Build OK → $OUT"
