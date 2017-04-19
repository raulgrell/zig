const assert = @import("std").debug.assert;

test "sizeofAndTypeOf" {
    const y: @typeOf(x) = 120;
    assert(@sizeOf(@typeOf(y)) == 2);
}
const x: u16 = 13;
const z: @typeOf(x) = 19;

const A = struct {
    a: u8,
    b: u32,
    c: u8,
};

const P = packed struct {
    a: u8,
    b: u32,
    c: u8,
};

test "offsetOf" {
    const a: A = undefined;
    const p: P = undefined;

    const aa = @offsetOf(@typeOf(a), "a");
    const ab = @offsetOf(@typeOf(a), "b");
    const ac = @offsetOf(@typeOf(a), "c");

    const pa = @offsetOf(@typeOf(p), "a");
    const pb = @offsetOf(@typeOf(p), "b");
    const pc = @offsetOf(@typeOf(p), "c");

    assert(aa == 0 and ab == 4 and ac == 8);
    assert(pa == 0 and pb == 1 and pc == 5);
}