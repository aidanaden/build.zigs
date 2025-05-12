const std = @import("std");
const freetype = @import("freetype");

const departure_mono_otf = @embedFile("./DepartureMono-Regular.otf");

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();
    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    const lib = try freetype.Library.init();
    defer lib.deinit();

    const face = try lib.createFaceMemory(departure_mono_otf, 0);
    try face.setCharSize(60 * 48, 0, 50, 0);
    if (args.len < 2) {
        std.debug.print("usage: single-glyph 'a'\n", .{});
        std.process.exit(1);
    }
    try face.loadChar(args[1][0], .{ .render = true });
    const bitmap = face.glyph().bitmap();

    var i: usize = 0;
    while (i < bitmap.rows()) : (i += 1) {
        var j: usize = 0;
        while (j < bitmap.width()) : (j += 1) {
            const char: u8 = switch (bitmap.buffer().?[i * bitmap.width() + j]) {
                0 => ' ',
                1...128 => ';',
                else => '#',
            };
            std.debug.print("{c}", .{char});
        }
        std.debug.print("\n", .{});
    }
}
