const std = @import("std");

const c_flags_default = [_][]const u8{
    "-std=c99",
    "-g",
    "-Wextra",
    "-Wall",
    "-Werror=vla",
    "-MD",
    "-03",
    "-pedantic",
    "-Ideps",
};

const c_files = [_][]const u8{
    "main.c",
    "type.c",
    "codegen.c",
    "unicode.c",
    "strings.c",
    "hashmap.c",
    "tokenize.c",
    "parse.c",
    "preprocess.c",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const exe = b.addExecutable(.{
        .name = "fzy_exe",
        .optimize = optimize,
        .target = target,
    });
    exe.root_module.addIncludePath(b.path("."));
    exe.root_module.addCSourceFiles(.{
        .root = b.path("."),
        .files = &c_files,
        .flags = &c_flags_default,
    });
    exe.root_module.link_libc = true;
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
