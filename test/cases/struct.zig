const assert = @import("std").debug.assert;

const StructWithNoFields = struct {
    fn add(a: i32, b: i32) -> i32 { a + b }
};
const empty_global_instance = StructWithNoFields {};

test "callStructStaticMethod" {
    const result = StructWithNoFields.add(3, 4);
    assert(result == 7);
}

test "returnEmptyStructInstance" {
    _ = returnEmptyStructInstance();
}
fn returnEmptyStructInstance() -> StructWithNoFields {
    return empty_global_instance;
}

const should_be_11 = StructWithNoFields.add(5, 6);

test "invokeStaticMethodInGlobalScope" {
    assert(should_be_11 == 11);
}

test "voidStructFields" {
    const foo = VoidStructFieldsFoo {
        .a = void{},
        .b = 1,
        .c = void{},
    };
    assert(foo.b == 1);
    assert(@sizeOf(VoidStructFieldsFoo) == 4);
}
const VoidStructFieldsFoo = struct {
    a : void,
    b : i32,
    c : void,
};


test "fn" {
    var foo: StructFoo = undefined;
    @memset(@ptrcast(&u8, &foo), 0, @sizeOf(StructFoo));
    foo.a += 1;
    foo.b = foo.a == 1;
    testFoo(foo);
    testMutation(&foo);
    assert(foo.c == 100);
}
const StructFoo = struct {
    a : i32,
    b : bool,
    c : f32,
};
fn testFoo(foo: &const StructFoo) {
    assert(foo.b);
}
fn testMutation(foo: &StructFoo) {
    foo.c = 100;
}


const Node = struct {
    val: Val,
    next: &Node,
};

const Val = struct {
    x: i32,
};

test "structPointToSelf" {
    var root : Node = undefined;
    root.val.x = 1;

    var node : Node = undefined;
    node.next = &root;
    node.val.x = 2;

    root.next = &node;

    assert(node.next.next.next.val.x == 1);
}

test "structByvalAssign" {
    var foo1 : StructFoo = undefined;
    var foo2 : StructFoo = undefined;

    foo1.a = 1234;
    foo2.a = 0;
    assert(foo2.a == 0);
    foo2 = foo1;
    assert(foo2.a == 1234);
}

fn structInitializer() {
    const val = Val { .x = 42 };
    assert(val.x == 42);
}


test "fnCallOfStructField" {
    assert(callStructField(Foo {.ptr = aFunc,}) == 13);
}

const Foo = struct {
    ptr: fn() -> i32,
};

fn aFunc() -> i32 { 13 }

fn callStructField(foo: &const Foo) -> i32 {
    return foo.ptr();
}


test "storeMemberFunctionInVariable" {
    const instance = MemberFnTestFoo { .x = 1234, };
    const memberFn = MemberFnTestFoo.member;
    const result = memberFn(instance);
    assert(result == 1234);
}
const MemberFnTestFoo = struct {
    x: i32,
    fn member(foo: &const MemberFnTestFoo) -> i32 { foo.x }
};


test "callMemberFunctionDirectly" {
    const instance = MemberFnTestFoo { .x = 1234, };
    const result = MemberFnTestFoo.member(instance);
    assert(result == 1234);
}

test "memberFunctions" {
    const r = MemberFnRand {.seed = 1234};
    assert(r.getSeed() == 1234);
}
const MemberFnRand = struct {
    seed: u32,
    pub fn getSeed(r: &const MemberFnRand) -> u32 {
        r.seed
    }
};

test "returnStructByvalFromFunction" {
    const bar = makeBar(1234, 5678);
    assert(bar.y == 5678);
}
const Bar = struct {
    x: i32,
    y: i32,
};
fn makeBar(x: i32, y: i32) -> Bar {
    Bar {
        .x = x,
        .y = y,
    }
}

test "emptyStructMethodCall" {
    const es = EmptyStruct{};
    assert(es.method() == 1234);
}
const EmptyStruct = struct {
    fn method(es: &const EmptyStruct) -> i32 {
        1234
    }
};


test "returnEmptyStructFromFn" {
    _ = testReturnEmptyStructFromFn();
}
const EmptyStruct2 = struct {};
fn testReturnEmptyStructFromFn() -> EmptyStruct2 {
    EmptyStruct2 {}
}

test "passSliceOfEmptyStructToFn" {
    assert(testPassSliceOfEmptyStructToFn([]EmptyStruct2{ EmptyStruct2{} }) == 1);
}
fn testPassSliceOfEmptyStructToFn(slice: []const EmptyStruct2) -> usize {
    slice.len
}

const APackedStruct = packed struct {
    x: u8,
    y: u8,
};

test "packedStruct" {
    var foo = APackedStruct {
        .x = 1,
        .y = 2,
    };
    foo.y += 1;
    const four = foo.x + foo.y;
    assert(four == 4);
}


const u2 = @IntType(false, 2);
const u3 = @IntType(false, 3);

const BitField1 = packed struct {
    a: u3,
    b: u3,
    c: u2,
};

const bit_field_1 = BitField1 {
    .a = 1,
    .b = 2,
    .c = 3,
};

test "bitFieldAccess" {
    var data = bit_field_1;
    assert(getA(&data) == 1);
    assert(getB(&data) == 2);
    assert(getC(&data) == 3);
    comptime assert(@sizeOf(BitField1) == 1);

    data.b += 1;
    assert(data.b == 3);

    data.a += 1;
    assert(data.a == 2);
    assert(data.b == 3);
}

fn getA(data: &const BitField1) -> u3 {
    return data.a;
}

fn getB(data: &const BitField1) -> u3 {
    return data.b;
}

fn getC(data: &const BitField1) -> u2 {
    return data.c;
}

const u24 = @IntType(false, 24);
const Foo24Bits = packed struct {
    field: u24,
};
const Foo96Bits = packed struct {
    a: u24,
    b: u24,
    c: u24,
    d: u24,
};

test "packedStruct24Bits" {
    comptime {
        assert(@sizeOf(Foo24Bits) == 3);
        assert(@sizeOf(Foo96Bits) == 12);
    }

    var value = Foo96Bits {
        .a = 0,
        .b = 0,
        .c = 0,
        .d = 0,
    };
    value.a += 1;
    assert(value.a == 1);
    assert(value.b == 0);
    assert(value.c == 0);
    assert(value.d == 0);

    value.b += 1;
    assert(value.a == 1);
    assert(value.b == 1);
    assert(value.c == 0);
    assert(value.d == 0);

    value.c += 1;
    assert(value.a == 1);
    assert(value.b == 1);
    assert(value.c == 1);
    assert(value.d == 0);

    value.d += 1;
    assert(value.a == 1);
    assert(value.b == 1);
    assert(value.c == 1);
    assert(value.d == 1);
}

const FooArray24Bits = packed struct {
    a: u16,
    b: [2]Foo24Bits,
    c: u16,
};

test "packedArray24Bits" {
    comptime {
        assert(@sizeOf([9]Foo24Bits) == 9 * 3);
        assert(@sizeOf(FooArray24Bits) == 2 + 2 * 3 + 2);
    }

    var bytes = []u8{0} ** (@sizeOf(FooArray24Bits) + 1);
    bytes[bytes.len - 1] = 0xaa;
    const ptr = &([]FooArray24Bits)(bytes[0...bytes.len - 1])[0];
    assert(ptr.a == 0);
    assert(ptr.b[0].field == 0);
    assert(ptr.b[1].field == 0);
    assert(ptr.c == 0);

    ptr.a = @maxValue(u16);
    assert(ptr.a == @maxValue(u16));
    assert(ptr.b[0].field == 0);
    assert(ptr.b[1].field == 0);
    assert(ptr.c == 0);

    ptr.b[0].field = @maxValue(u24);
    assert(ptr.a == @maxValue(u16));
    assert(ptr.b[0].field == @maxValue(u24));
    assert(ptr.b[1].field == 0);
    assert(ptr.c == 0);

    ptr.b[1].field = @maxValue(u24);
    assert(ptr.a == @maxValue(u16));
    assert(ptr.b[0].field == @maxValue(u24));
    assert(ptr.b[1].field == @maxValue(u24));
    assert(ptr.c == 0);

    ptr.c = @maxValue(u16);
    assert(ptr.a == @maxValue(u16));
    assert(ptr.b[0].field == @maxValue(u24));
    assert(ptr.b[1].field == @maxValue(u24));
    assert(ptr.c == @maxValue(u16));

    assert(bytes[bytes.len - 1] == 0xaa);
}

const FooStructAligned = packed struct {
    a: u8,
    b: u8,
};

const FooArrayOfAligned = packed struct {
    a: [2]FooStructAligned,
};

test "alignedArrayOfPackedStruct" {
    comptime {
        assert(@sizeOf(FooStructAligned) == 2);
        assert(@sizeOf(FooArrayOfAligned) == 2 * 2);
    }

    var bytes = []u8{0xbb} ** @sizeOf(FooArrayOfAligned);
    const ptr = &([]FooArrayOfAligned)(bytes[0...bytes.len])[0];

    assert(ptr.a[0].a == 0xbb);
    assert(ptr.a[0].b == 0xbb);
    assert(ptr.a[1].a == 0xbb);
    assert(ptr.a[1].b == 0xbb);
}



test "runtime struct initialization of bitfield" {
    const s1 = Nibbles { .x = x1, .y = x1 };
    const s2 = Nibbles { .x = u4(x2), .y = u4(x2) };

    assert(s1.x == x1);
    assert(s1.y == x1);
    assert(s2.x == u4(x2));
    assert(s2.y == u4(x2));
}

const u4 = @IntType(false, 4);

var x1 = u4(1);
var x2 = u8(2);

const Nibbles = packed struct {
    x: u4,
    y: u4,
};
