const std = @import("std");
const builtin = @import("builtin");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const lib = b.addStaticLibrary(.{
        .name = "freetype",
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    lib.addIncludePath(b.path("include"));
    lib.addCSourceFiles(.{
        .root = b.path("."),
        .files = &source_files,
        .flags = &.{},
    });

    // Required for `@cImport` to work
    lib.installHeadersDirectory(b.path("include/freetype"), "freetype", .{});
    lib.installHeader(b.path("include/ft2build.h"), "ft2build.h");

    switch (builtin.target.os.tag) {
        .windows => {
            lib.addCSourceFiles(.{
                .root = b.path("."),
                .files = &windows_source_files,
                .flags = &.{},
            });
        },
        .macos => {
            lib.addCSourceFiles(.{
                .root = b.path("."),
                .files = &mac_source_files,
                .flags = &.{},
            });
        },
        else => {},
    }

    // Following are taken from CMakeLists.txt
    lib.root_module.addCMacro("FT_DISABLE_ZLIB", "1");
    lib.root_module.addCMacro("FT_DISABLE_BZIP2", "1");
    lib.root_module.addCMacro("FT_DISABLE_PNG", "1");
    lib.root_module.addCMacro("FT_DISABLE_HARFBUZZ", "1");
    lib.root_module.addCMacro("FT_DISABLE_BROTLI", "1");

    // Taken from builds/freetype.mk, Needed to avoid erros about the FT_ERR_PREFIX macro.
    // See https://freetype-devel.nongnu.narkive.com/DToge9Fj/ft-devel-compiling-freetype-2-5-with-c-builder-ft-throw-problem
    lib.root_module.addCMacro("FT2_BUILD_LIBRARY", "1");

    const freetype_module = b.addModule("freetype", .{
        .root_source_file = b.path("freetype.zig"),
        .link_libc = true,
    });
    freetype_module.linkLibrary(lib);

    const mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .root_source_file = b.path("main.zig"),
    });
    const exe = b.addExecutable(.{
        .name = "single-glyph",
        .root_module = mod,
    });
    exe.root_module.addImport("freetype", freetype_module);

    const install = b.addInstallArtifact(exe, .{});
    b.getInstallStep().dependOn(&install.step);

    const run = b.addRunArtifact(exe);
    run.step.dependOn(&install.step);
    if (b.args) |args| {
        run.addArgs(args);
    }

    const run_step = b.step("run", "run build");
    run_step.dependOn(&run.step);
}

// Taken from line 385 of CMakeLists.txt
const source_files = [_][]const u8{
    "src/base/ftsystem.c",
    "src/base/ftdebug.c",
    "src/autofit/autofit.c",
    "src/base/ftbase.c",
    "src/base/ftsystem.c",
    "src/base/ftdebug.c",
    "src/base/ftbbox.c",
    "src/base/ftbdf.c",
    "src/base/ftbitmap.c",
    "src/base/ftcid.c",
    "src/base/ftfstype.c",
    "src/base/ftgasp.c",
    "src/base/ftglyph.c",
    "src/base/ftgxval.c",
    "src/base/ftinit.c",
    "src/base/ftmm.c",
    "src/base/ftotval.c",
    "src/base/ftpatent.c",
    "src/base/ftpfr.c",
    "src/base/ftstroke.c",
    "src/base/ftsynth.c",
    "src/base/fttype1.c",
    "src/base/ftwinfnt.c",
    "src/bdf/bdf.c",
    "src/bzip2/ftbzip2.c",
    "src/cache/ftcache.c",
    "src/cff/cff.c",
    "src/cid/type1cid.c",
    "src/gzip/ftgzip.c",
    "src/lzw/ftlzw.c",
    "src/pcf/pcf.c",
    "src/pfr/pfr.c",
    "src/psaux/psaux.c",
    "src/pshinter/pshinter.c",
    "src/psnames/psnames.c",
    "src/raster/raster.c",
    "src/sdf/sdf.c",
    "src/sfnt/sfnt.c",
    "src/smooth/smooth.c",
    "src/svg/svg.c",
    "src/truetype/truetype.c",
    "src/type1/type1.c",
    "src/type42/type42.c",
    "src/winfonts/winfnt.c",
};

const windows_source_files = [_][]const u8{
    "builds/windows/ftsystem.c",
    "builds/windows/ftdebug.c",
    "src/base/ftver.rc",
};

const mac_source_files = [_][]const u8{
    "src/base/ftmac.c",
};
