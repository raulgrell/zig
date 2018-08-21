const std = @import("index.zig");
const debug = std.debug;
const assert = debug.assert;
const assertError = debug.assertError;
const mem = std.mem;
const Allocator = mem.Allocator;

pub fn ArrayList(comptime T: type) type {
    return AlignedArrayList(T, @alignOf(T));
}

pub fn AlignedArrayList(comptime T: type, comptime A: u29) type {
    return struct {
        const Self = this;

        /// Use toSlice instead of slicing this directly, because if you don't
        /// specify the end position of the slice, this will potentially give
        /// you uninitialized memory.
        items: []align(A) T,
        len: usize,
        allocator: *Allocator,

        /// Initializes a list that manages memory for items using `allocator`
        /// Deinitialize with `deinit` or use `toOwnedSlice`.
        pub fn init(allocator: *Allocator) Self {
            return Self{
                .items = []align(A) T{},
                .len = 0,
                .allocator = allocator,
            };
        }

        /// Frees the list's allocated memory
        pub fn deinit(self: Self) void {
            self.allocator.free(self.items);
        }

        /// Returns a slice containing all items
        pub fn toSlice(self: Self) []align(A) T {
            return self.items[0..self.len];
        }

        /// Returns a const slice containint all items
        pub fn toSliceConst(self: Self) []align(A) const T {
            return self.items[0..self.len];
        }

        /// Returns the item at `i`
        pub fn at(self: Self, i: usize) T {
            return self.toSliceConst()[i];
        }

        /// Sets the value at index `i`, or returns `error.OutOfBounds` if
        /// the index is not in range.
        pub fn setOrError(self: Self, i: usize, item: T) !void {
            if (i >= self.len) return error.OutOfBounds;
            self.items[i] = item;
        }

        /// Sets the value at index `i`, asserting that the value is in range.
        pub fn set(self: *Self, i: usize, item: T) void {
            assert(i < self.len);
            self.items[i] = item;
        }

        /// Returns the number of items in the list
        pub fn count(self: Self) usize {
            return self.len;
        }

        /// ArrayList takes ownership of the passed in slice. The slice must have been
        /// allocated with `allocator`.
        /// Deinitialize with `deinit` or use `toOwnedSlice`.
        pub fn fromOwnedSlice(allocator: *Allocator, slice: []align(A) T) Self {
            return Self{
                .items = slice,
                .len = slice.len,
                .allocator = allocator,
            };
        }

        /// The caller owns the returned memory. ArrayList becomes empty.
        pub fn toOwnedSlice(self: *Self) []align(A) T {
            const allocator = self.allocator;
            const result = allocator.alignedShrink(T, A, self.items, self.len);
            self.* = init(allocator);
            return result;
        }

        /// Inserts `item` at index `n`
        pub fn insert(self: *Self, n: usize, item: T) !void {
            try self.ensureCapacity(self.len + 1);
            self.len += 1;

            mem.copyBackwards(T, self.items[n + 1 .. self.len], self.items[n .. self.len - 1]);
            self.items[n] = item;
        }

        /// Inserts the elements `items` into the the list at index `n`
        pub fn insertSlice(self: *Self, n: usize, items: []align(A) const T) !void {
            try self.ensureCapacity(self.len + items.len);
            self.len += items.len;

            mem.copyBackwards(T, self.items[n + items.len .. self.len], self.items[n .. self.len - items.len]);
            mem.copy(T, self.items[n .. n + items.len], items);
        }

        /// Inserts `item` at the end of the list
        pub fn append(self: *Self, item: T) !void {
            const new_item_ptr = try self.addOne();
            new_item_ptr.* = item;
        }

        /// Removes the element at the specified index and returns it.
        /// The empty slot is filled from the end of the list.
        pub fn swapRemove(self: *Self, i: usize) T {
            if (self.len - 1 == i) return self.pop();

            const slice = self.toSlice();
            const old_item = slice[i];
            slice[i] = self.pop();
            return old_item;
        }

        /// Removes the element at the specified index and returns it
        /// or an error.OutOfBounds is returned. If no error then
        /// the empty slot is filled from the end of the list.
        pub fn swapRemoveOrError(self: *Self, i: usize) !T {
            if (i >= self.len) return error.OutOfBounds;
            return self.swapRemove(i);
        }

        /// Inserts the elements of `items` at the end of the list.
        pub fn appendSlice(self: *Self, items: []align(A) const T) !void {
            try self.ensureCapacity(self.len + items.len);
            mem.copy(T, self.items[self.len..], items);
            self.len += items.len;
        }

        /// Sets the length of the list to `new_len`, returning an error if
        /// it is unable to allocate enough memory.
        pub fn resize(self: *Self, new_len: usize) !void {
            try self.ensureCapacity(new_len);
            self.len = new_len;
        }

        /// Sets the length of the list to `new_len`, asserting it is smaller than
        /// the current length.
        pub fn shrink(self: *Self, new_len: usize) void {
            assert(new_len <= self.len);
            self.len = new_len;
        }

        /// Ensures the list has enough memory to hold `new_capacity` items,
        /// reallocating `self.items` if necessary
        pub fn ensureCapacity(self: *Self, new_capacity: usize) !void {
            var better_capacity = self.items.len;
            if (better_capacity >= new_capacity) return;
            while (true) {
                better_capacity += better_capacity / 2 + 8;
                if (better_capacity >= new_capacity) break;
            }
            self.items = try self.allocator.alignedRealloc(T, A, self.items, better_capacity);
        }

        /// Increases the length of the list, and capacity if necessary, returning
        /// a pointer to the new item or an error if allocation fails.
        pub fn addOne(self: *Self) !*T {
            const new_length = self.len + 1;
            try self.ensureCapacity(new_length);
            const result = &self.items[self.len];
            self.len = new_length;
            return result;
        }

        /// Removes and returns the last item of the list
        pub fn pop(self: *Self) T {
            self.len -= 1;
            return self.items[self.len];
        }

        /// Removes and returns the last item of the list, returning null if the
        /// list is empty.
        pub fn popOrNull(self: *Self) ?T {
            if (self.len == 0) return null;
            return self.pop();
        }

        /// Removes the item at `index`
        pub fn remove(self: *Self, index: usize) !void {
            try remove_range(self, index, 1);
        }

        /// Removes the items in `self.items[index..index + length]`, copying
        /// the items that followed it into their positions
        pub fn removeRange(self: *Self, index: usize, length: usize) !void {
            if (index > self.len or index + length > self.len) return error.InvalidRange;
            mem.copy(T, self.items[n .. self.items.len - length], self.items[n + length .. self.len]);
            self.len -= length;
        }

        /// Removes the items in `self.items[index..index + length]`, replacing them
        /// with the items in `array`.
        pub fn splice(self: *Self, index: usize, length: usize, array: []const T) !void {
            if (index > self.len or index + length > self.len) return error.InvalidRange;
            
            if (length == array.len) {
                mem.copy(T, self.items[index .. index + array.len], array);
                return;
            }
            
            const new_len = self.len + array.len - length;
            try self.ensureCapacity(new_len);
            mem.copy(T, self.items[index + array.len .. index + new_len], self.items[index + length .. index + self.len]);
            mem.copy(T, self.items[index .. index + array.len], array);
            self.len += adj;
        }

        /// Returns the first element in the list for which `eql_fn(item, data)` returns true
        pub fn indexOfFirst(self: *Self, eql_fn: EqualityFunc, data: T) !usize {
            for (self.items[0..self.len]) | item, i | {
                if(eql_fn(item, data)) return i;
            }
            return error.NotFound;
        }

        /// Returns the last element in the list for which `eql_fn(item, data)` returns true
        pub fn indexOfLast(self: *Self, eql_fn: EqualityFunc, data: T) !usize {
            for (self.items[0..self.len]) | item, i | {
                if(eql_fn(item, data)) return i;
            }
            return error.NotFound;
        }

        /// Removes all items from the list without deallocating the used memory.
        pub fn clear(self: *Self) void {
            self.len = 0;
        }

        pub const Iterator = struct {
            list: *const Self,
            /// The number of items returned
            count: usize,

            /// Returns the next item in the list, or null if the iterator has
            /// reached the end.
            pub fn next(it: *Iterator) ?T {
                if (it.count >= it.list.len) return null;
                const val = it.list.at(it.count);
                it.count += 1;
                return val;
            }

            /// Resets the iterator count
            pub fn reset(it: *Iterator) void {
                it.count = 0;
            }
        };

        /// Returns an Iterator for the list, starting at the first item.
        pub fn iterator(self: *const Self) Iterator {
            return Iterator{
                .list = self,
                .count = 0,
            };
        }
    };
}

test "std.ArrayList.basic" {
    var bytes: [1024]u8 = undefined;
    const allocator = &std.heap.FixedBufferAllocator.init(bytes[0..]).allocator;

    var list = ArrayList(i32).init(allocator);
    defer list.deinit();

    // setting on empty list is out of bounds
    assertError(list.setOrError(0, 1), error.OutOfBounds);

    {
        var i: usize = 0;
        while (i < 10) : (i += 1) {
            list.append(@intCast(i32, i + 1)) catch unreachable;
        }
    }

    {
        var i: usize = 0;
        while (i < 10) : (i += 1) {
            assert(list.items[i] == @intCast(i32, i + 1));
        }
    }

    for (list.toSlice()) |v, i| {
        assert(v == @intCast(i32, i + 1));
    }

    for (list.toSliceConst()) |v, i| {
        assert(v == @intCast(i32, i + 1));
    }

    assert(list.pop() == 10);
    assert(list.len == 9);

    list.appendSlice([]const i32{
        1,
        2,
        3,
    }) catch unreachable;
    assert(list.len == 12);
    assert(list.pop() == 3);
    assert(list.pop() == 2);
    assert(list.pop() == 1);
    assert(list.len == 9);

    list.appendSlice([]const i32{}) catch unreachable;
    assert(list.len == 9);

    // can only set on indices < self.len
    list.set(7, 33);
    list.set(8, 42);

    assertError(list.setOrError(9, 99), error.OutOfBounds);
    assertError(list.setOrError(10, 123), error.OutOfBounds);

    assert(list.pop() == 42);
    assert(list.pop() == 33);
}

test "std.ArrayList.swapRemove" {
    var list = ArrayList(i32).init(debug.global_allocator);
    defer list.deinit();

    try list.append(1);
    try list.append(2);
    try list.append(3);
    try list.append(4);
    try list.append(5);
    try list.append(6);
    try list.append(7);

    //remove from middle
    assert(list.swapRemove(3) == 4);
    assert(list.at(3) == 7);
    assert(list.len == 6);

    //remove from end
    assert(list.swapRemove(5) == 6);
    assert(list.len == 5);

    //remove from front
    assert(list.swapRemove(0) == 1);
    assert(list.at(0) == 5);
    assert(list.len == 4);
}

test "std.ArrayList.swapRemoveOrError" {
    var list = ArrayList(i32).init(debug.global_allocator);
    defer list.deinit();

    // Test just after initialization
    assertError(list.swapRemoveOrError(0), error.OutOfBounds);

    // Test after adding one item and remote it
    try list.append(1);
    assert((try list.swapRemoveOrError(0)) == 1);
    assertError(list.swapRemoveOrError(0), error.OutOfBounds);

    // Test after adding two items and remote both
    try list.append(1);
    try list.append(2);
    assert((try list.swapRemoveOrError(1)) == 2);
    assert((try list.swapRemoveOrError(0)) == 1);
    assertError(list.swapRemoveOrError(0), error.OutOfBounds);

    // Test out of bounds with one item
    try list.append(1);
    assertError(list.swapRemoveOrError(1), error.OutOfBounds);

    // Test out of bounds with two items
    try list.append(2);
    assertError(list.swapRemoveOrError(2), error.OutOfBounds);
}

test "std.ArrayList.iterator" {
    var list = ArrayList(i32).init(debug.global_allocator);
    defer list.deinit();

    try list.append(1);
    try list.append(2);
    try list.append(3);

    var count: i32 = 0;
    var it = list.iterator();
    while (it.next()) |next| {
        assert(next == count + 1);
        count += 1;
    }

    assert(count == 3);
    assert(it.next() == null);
    it.reset();
    count = 0;
    while (it.next()) |next| {
        assert(next == count + 1);
        count += 1;
        if (count == 2) break;
    }

    it.reset();
    assert(it.next().? == 1);
}

test "std.ArrayList.insert" {
    var list = ArrayList(i32).init(debug.global_allocator);
    defer list.deinit();

    try list.append(1);
    try list.append(2);
    try list.append(3);
    try list.insert(0, 5);
    assert(list.items[0] == 5);
    assert(list.items[1] == 1);
    assert(list.items[2] == 2);
    assert(list.items[3] == 3);
}

test "std.ArrayList.insertSlice" {
    var list = ArrayList(i32).init(debug.global_allocator);
    defer list.deinit();

    try list.append(1);
    try list.append(2);
    try list.append(3);
    try list.append(4);
    try list.insertSlice(1, []const i32{
        9,
        8,
    });
    assert(list.items[0] == 1);
    assert(list.items[1] == 9);
    assert(list.items[2] == 8);
    assert(list.items[3] == 2);
    assert(list.items[4] == 3);
    assert(list.items[5] == 4);

    const items = []const i32{1};
    try list.insertSlice(0, items[0..0]);
    assert(list.len == 6);
    assert(list.items[0] == 1);
}
