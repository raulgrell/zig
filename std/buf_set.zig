const std = @import("index.zig");
const HashMap = @import("hash_map.zig").HashMap;
const mem = @import("mem.zig");
const Allocator = mem.Allocator;
const assert = std.debug.assert;

/// BufSet copies keys and values before they go into the set, and
/// frees them when they get removed.
pub const BufSet = struct {
    hash_map: BufSetHashMap,

    const BufSetHashMap = HashMap([]const u8, void, mem.hash_slice_u8, mem.eql_slice_u8);

    /// Initializes a set that manages its memory using `allocator`.
    /// Deinitialize with `deinit` or use `toOwnedSlice`.
    pub fn init(a: *Allocator) BufSet {
        var self = BufSet{ .hash_map = BufSetHashMap.init(a) };
        return self;
    }

    /// Removes and frees all entries in the set
    pub fn deinit(self: *const BufSet) void {
        var it = self.hash_map.iterator();
        while (true) {
            const entry = it.next() orelse break;
            self.free(entry.key);
        }

        self.hash_map.deinit();
    }

    /// Adds an entry to the set
    pub fn put(self: *BufSet, key: []const u8) !void {
        if (self.hash_map.get(key) == null) {
            const key_copy = try self.copy(key);
            errdefer self.free(key_copy);
            _ = try self.hash_map.put(key_copy, {});
        }
    }

    /// Removes and frees and entry from the set
    pub fn delete(self: *BufSet, key: []const u8) void {
        const entry = self.hash_map.remove(key) orelse return;
        self.free(entry.key);
    }

    /// Returns the number of entries in the set
    pub fn count(self: *const BufSet) usize {
        return self.hash_map.count();
    }

    /// Returns an iterator for the items in the set
    pub fn iterator(self: *const BufSet) BufSetHashMap.Iterator {
        return self.hash_map.iterator();
    }

    /// Returns the allocator used by the set
    pub fn allocator(self: *const BufSet) *Allocator {
        return self.hash_map.allocator;
    }

    fn free(self: *const BufSet, value: []const u8) void {
        self.hash_map.allocator.free(value);
    }

    fn copy(self: *const BufSet, value: []const u8) ![]const u8 {
        const result = try self.hash_map.allocator.alloc(u8, value.len);
        mem.copy(u8, result, value);
        return result;
    }
};

test "BufSet" {
    var direct_allocator = std.heap.DirectAllocator.init();
    defer direct_allocator.deinit();

    var bufset = BufSet.init(&direct_allocator.allocator);
    defer bufset.deinit();

    try bufset.put("x");
    assert(bufset.count() == 1);
    bufset.delete("x");
    assert(bufset.count() == 0);

    try bufset.put("x");
    try bufset.put("y");
    try bufset.put("z");
}
