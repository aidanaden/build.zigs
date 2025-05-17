const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    build_figlet(b, target, optimize);
    build_chkfont(b, target, optimize);
}

fn build_figlet(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    const c_files = [_][]const u8{
        "figlet.c",
        "inflate.c",
        "utf8.c",
        "zipio.c",
        "crc.c",
    };
    const exe = b.addExecutable(.{
        .name = "figlet",
        .optimize = optimize,
        .target = target,
    });
    exe.root_module.addIncludePath(b.path("."));
    exe.root_module.addCSourceFiles(.{
        .root = b.path("."),
        .files = &c_files,
        .flags = &c_flags,
    });
    exe.root_module.link_libc = true;
    exe.root_module.addCMacro("DEFAULTFONTDIR", "\"fonts\"");
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    // Usage: zig build run-figlet -- -f <font-name> <text-to-print>
    // Example: zig build run-figlet -- -f standard hello world
    const run_step = b.step("run-figlet", "Run figlet executable");
    run_step.dependOn(&run_cmd.step);
}

fn build_chkfont(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    const c_files = [_][]const u8{"chkfont.c"};
    const exe = b.addExecutable(.{
        .name = "chkfont",
        .optimize = optimize,
        .target = target,
    });
    exe.root_module.addIncludePath(b.path("."));
    exe.root_module.addCSourceFiles(.{
        .root = b.path("."),
        .files = &c_files,
        .flags = &c_flags,
    });
    exe.root_module.link_libc = true;
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    // Usage: zig build run-chkfont -- <font-file>
    // Example: zig build run-chkfont -- fonts/standard.flf
    const run_step = b.step("run-chkfont", "Run chkfont executable");
    run_step.dependOn(&run_cmd.step);
}

const c_flags = [_][]const u8{
    "-g",
    "-O2",
    "-Wno-unused-value",
    "-Wall",
};
