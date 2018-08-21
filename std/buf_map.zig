const std = @import("index.zig");
const HashMap = std.HashMap;
const mem = std.mem;
const Allocator = mem.Allocator;
const assert = std.debug.assert;

/// BufMap copies keys and values before they go into the map, and
/// frees them when they get removed.
pub const BufMap = struct {
    hash_map: BufMapHashMap,

    const BufMapHashMap = HashMap([]const u8, []const u8, mem.hash_slice_u8, mem.eql_slice_u8);

    /// Initializes a map that manages its memory using `allocator`.
    /// Deinitialize with `deinit` or use `toOwnedSlice`.
    pub fn init(allocator: *Allocator) BufMap {
        var self = BufMap{ .hash_map = BufMapHashMap.init(allocator) };
        return self;
    }

    /// Removes and frees all entries from the map
    pub fn deinit(self: *const BufMap) void {
        var it = self.hash_map.iterator();
        while (true) {
            const entry = it.next() orelse break;
            self.free(entry.key);
            self.free(entry.value);
        }

        self.hash_map.deinit();
    }

    /// Sets the the value of the entry for `key` to `value`
    pub fn set(self: *BufMap, key: []const u8, value: []const u8) !void {
        self.delete(key);
        const key_copy = try self.copy(key);
        errdefer self.free(key_copy);
        const value_copy = try self.copy(value);
        errdefer self.free(value_copy);
        _ = try self.hash_map.put(key_copy, value_copy);
    }

    /// Returns the value of the entry for `key` or null if it does not exist
    pub fn get(self: *const BufMap, key: []const u8) ?[]const u8 {
        const entry = self.hash_map.get(key) orelse return null;
        return entry.value;
    }

    /// Deletes the entry for `key`
    pub fn delete(self: *BufMap, key: []const u8) void {
        const entry = self.hash_map.remove(key) orelse return;
        self.free(entry.key);
        self.free(entry.value);
    }

    /// Returns the number of entries in the map
    pub fn count(self: *const BufMap) usize {
        return self.hash_map.count();
    }

    /// Returns an iterator for the entries in the map
    pub fn iterator(self: *const BufMap) BufMapHashMap.Iterator {
        return self.hash_map.iterator();
    }

    /// Returns the allocator used by the map
    pub fn allocator(self: *const BufSet) *Allocator {
        return self.hash_map.allocator;
    }

    fn free(self: *const BufMap, value: []const u8) void {
        self.hash_map.allocator.free(value);
    }

    fn copy(self: *const BufMap, value: []const u8) ![]const u8 {
        return mem.dupe(self.hash_map.allocator, u8, value);
    }
};

test "BufMap" {
    var direct_allocator = std.heap.DirectAllocator.init();
    defer direct_allocator.deinit();

    var bufmap = BufMap.init(&direct_allocator.allocator);
    defer bufmap.deinit();

    try bufmap.set("x", "1");
    assert(mem.eql(u8, bufmap.get("x").?, "1"));
    assert(1 == bufmap.count());

    try bufmap.set("x", "2");
    assert(mem.eql(u8, bufmap.get("x").?, "2"));
    assert(1 == bufmap.count());

    try bufmap.set("x", "3");
    assert(mem.eql(u8, bufmap.get("x").?, "3"));
    assert(1 == bufmap.count());

    bufmap.delete("x");
    assert(0 == bufmap.count());
}
