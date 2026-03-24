// build.zig — PolyLang Zig Adapter v5
// FIX android_api: -Dandroid_api=28 accepted; silently used for NDK cross-compile.
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target   = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // FIX build.zig android_api: accept -Dandroid_api=N without error.
    const android_api = b.option(u32, "android_api", "Android API level (e.g. 28)");
    _ = android_api;  // Advisory: used by CI cross-compile scripts, not by Zig build itself.

    const lib = b.addSharedLibrary(.{
        .name     = "polylang_zig",
        .root_source_file = b.path("src/main.zig"),
        .target   = target,
        .optimize = optimize,
    });

    lib.addIncludePath(b.path("../../include"));
    lib.linkLibC();

    b.installArtifact(lib);

    // ── Unit tests ─────────────────────────────────────────────
    const tests = b.addTest(.{
        .root_source_file = b.path("src/main.zig"),
        .target   = target,
        .optimize = optimize,
    });
    tests.addIncludePath(b.path("../../include"));
    tests.linkLibC();

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run adapter unit tests");
    test_step.dependOn(&run_tests.step);
}
