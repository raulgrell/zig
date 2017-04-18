/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "list.hpp"
#include "buffer.hpp"
#include "os.hpp"
#include "error.hpp"
#include "config.h"

#include <stdio.h>
#include <stdarg.h>

enum TestSpecial {
    TestSpecialNone,
    TestSpecialSelfHosted,
    TestSpecialStd,
    TestSpecialLinkStep,
};

struct TestSourceFile {
    const char *relative_path;
    const char *source_code;
};

enum AllowWarnings {
    AllowWarningsNo,
    AllowWarningsYes,
};

struct TestCase {
    const char *case_name;
    const char *output;
    ZigList<TestSourceFile> source_files;
    ZigList<const char *> compile_errors;
    ZigList<const char *> compiler_args;
    ZigList<const char *> linker_args;
    ZigList<const char *> program_args;
    bool is_parseh;
    TestSpecial special;
    bool is_release_mode;
    bool is_debug_safety;
    AllowWarnings allow_warnings;
};

static ZigList<TestCase*> test_cases = {0};
static const char *tmp_source_path = ".tmp_source.zig";
static const char *tmp_h_path = ".tmp_header.h";

#if defined(_WIN32)
static const char *tmp_exe_path = "./.tmp_exe.exe";
static const char *zig_exe = "./zig.exe";
#define NL "\r\n"
#else
static const char *tmp_exe_path = "./.tmp_exe";
static const char *zig_exe = "./zig";
#define NL "\n"
#endif

static void add_source_file(TestCase *test_case, const char *path, const char *source) {
    test_case->source_files.add_one();
    test_case->source_files.last().relative_path = path;
    test_case->source_files.last().source_code = source;
}

static TestCase *add_simple_case(const char *case_name, const char *source, const char *output) {
    TestCase *test_case = allocate<TestCase>(1);
    test_case->case_name = case_name;
    test_case->output = output;

    test_case->source_files.resize(1);
    test_case->source_files.at(0).relative_path = tmp_source_path;
    test_case->source_files.at(0).source_code = source;

    test_case->compiler_args.append("build_exe");
    test_case->compiler_args.append(tmp_source_path);
    test_case->compiler_args.append("--name");
    test_case->compiler_args.append("test");
    test_case->compiler_args.append("--output");
    test_case->compiler_args.append(tmp_exe_path);
    test_case->compiler_args.append("--release");
    test_case->compiler_args.append("--strip");
    test_case->compiler_args.append("--color");
    test_case->compiler_args.append("on");

    test_cases.append(test_case);

    return test_case;
}

static TestCase *add_asm_case(const char *case_name, const char *source, const char *output) {
    TestCase *test_case = allocate<TestCase>(1);
    test_case->case_name = case_name;
    test_case->output = output;
    test_case->special = TestSpecialLinkStep;

    test_case->source_files.resize(1);
    test_case->source_files.at(0).relative_path = ".tmp_source.s";
    test_case->source_files.at(0).source_code = source;

    test_case->compiler_args.append("asm");
    test_case->compiler_args.append(".tmp_source.s");
    test_case->compiler_args.append("--name");
    test_case->compiler_args.append("test");
    test_case->compiler_args.append("--color");
    test_case->compiler_args.append("on");

    test_case->linker_args.append("link_exe");
    test_case->linker_args.append("test.o");
    test_case->linker_args.append("--name");
    test_case->linker_args.append("test");
    test_case->linker_args.append("--output");
    test_case->linker_args.append(tmp_exe_path);
    test_case->linker_args.append("--color");
    test_case->linker_args.append("on");

    test_cases.append(test_case);

    return test_case;
}

static TestCase *add_simple_case_libc(const char *case_name, const char *source, const char *output) {
    TestCase *tc = add_simple_case(case_name, source, output);
    tc->compiler_args.append("--library");
    tc->compiler_args.append("c");
    return tc;
}

static TestCase *add_compile_fail_case(const char *case_name, const char *source, size_t count, ...) {
    va_list ap;
    va_start(ap, count);

    TestCase *test_case = allocate<TestCase>(1);
    test_case->case_name = case_name;
    test_case->source_files.resize(1);
    test_case->source_files.at(0).relative_path = tmp_source_path;
    test_case->source_files.at(0).source_code = source;

    for (size_t i = 0; i < count; i += 1) {
        const char *arg = va_arg(ap, const char *);
        test_case->compile_errors.append(arg);
    }

    test_case->compiler_args.append("build_obj");
    test_case->compiler_args.append(tmp_source_path);

    test_case->compiler_args.append("--name");
    test_case->compiler_args.append("test");

    test_case->compiler_args.append("--output");
    test_case->compiler_args.append(tmp_exe_path);

    test_case->compiler_args.append("--release");
    test_case->compiler_args.append("--strip");

    test_cases.append(test_case);

    return test_case;
}

static void add_debug_safety_case(const char *case_name, const char *source) {
    TestCase *test_case = allocate<TestCase>(1);
    test_case->is_debug_safety = true;
    test_case->case_name = buf_ptr(buf_sprintf("%s", case_name));
    test_case->source_files.resize(1);
    test_case->source_files.at(0).relative_path = tmp_source_path;
    test_case->source_files.at(0).source_code = source;

    test_case->compiler_args.append("build_exe");
    test_case->compiler_args.append(tmp_source_path);

    test_case->compiler_args.append("--name");
    test_case->compiler_args.append("test");

    test_case->compiler_args.append("--output");
    test_case->compiler_args.append(tmp_exe_path);

    test_cases.append(test_case);
}

static TestCase *add_parseh_case(const char *case_name, AllowWarnings allow_warnings,
    const char *source, size_t count, ...)
{
    va_list ap;
    va_start(ap, count);

    TestCase *test_case = allocate<TestCase>(1);
    test_case->case_name = case_name;
    test_case->is_parseh = true;
    test_case->allow_warnings = allow_warnings;

    test_case->source_files.resize(1);
    test_case->source_files.at(0).relative_path = tmp_h_path;
    test_case->source_files.at(0).source_code = source;

    for (size_t i = 0; i < count; i += 1) {
        const char *arg = va_arg(ap, const char *);
        test_case->compile_errors.append(arg);
    }

    test_case->compiler_args.append("parseh");
    test_case->compiler_args.append(tmp_h_path);
    //test_case->compiler_args.append("--verbose");

    test_cases.append(test_case);

    va_end(ap);
    return test_case;
}

static TestCase *add_example_compile_extra(const char *root_source_file, bool libc) {
    TestCase *test_case = allocate<TestCase>(1);
    test_case->case_name = buf_ptr(buf_sprintf("build example %s", root_source_file));
    test_case->output = nullptr;
    test_case->special = TestSpecialNone;

    test_case->compiler_args.append("build_exe");
    test_case->compiler_args.append(buf_ptr(buf_sprintf("../%s", root_source_file)));

    test_cases.append(test_case);

    return test_case;
}

static TestCase *add_example_compile(const char *root_source_file) {
    return add_example_compile_extra(root_source_file, false);
}

static TestCase *add_example_compile_libc(const char *root_source_file) {
    return add_example_compile_extra(root_source_file, true);
}

static void add_compiling_test_cases(void) {
    add_simple_case_libc("hello world with libc", R"SOURCE(
const c = @cImport(@cInclude("stdio.h"));
export fn main(argc: c_int, argv: &&u8) -> c_int {
    _ = c.puts(c"Hello, world!");
    return 0;
}
    )SOURCE", "Hello, world!" NL);

    {
        TestCase *tc = add_simple_case("multiple files with private function", R"SOURCE(
use @import("std").io;
use @import("foo.zig");

pub fn main() -> %void {
    privateFunction();
    %%stdout.printf("OK 2\n");
}

fn privateFunction() {
    printText();
}
        )SOURCE", "OK 1\nOK 2\n");

        add_source_file(tc, "foo.zig", R"SOURCE(
use @import("std").io;

// purposefully conflicting function with main.zig
// but it's private so it should be OK
fn privateFunction() {
    %%stdout.printf("OK 1\n");
}

pub fn printText() {
    privateFunction();
}
        )SOURCE");
    }

    {
        TestCase *tc = add_simple_case("import segregation", R"SOURCE(
use @import("foo.zig");
use @import("bar.zig");

pub fn main() -> %void {
    foo_function();
    bar_function();
}
        )SOURCE", "OK\nOK\n");

        add_source_file(tc, "foo.zig", R"SOURCE(
use @import("std").io;
pub fn foo_function() {
    %%stdout.printf("OK\n");
}
        )SOURCE");

        add_source_file(tc, "bar.zig", R"SOURCE(
use @import("other.zig");
use @import("std").io;

pub fn bar_function() {
    if (foo_function()) {
        %%stdout.printf("OK\n");
    }
}
        )SOURCE");

        add_source_file(tc, "other.zig", R"SOURCE(
pub fn foo_function() -> bool {
    // this one conflicts with the one from foo
    return true;
}
        )SOURCE");
    }

    {
        TestCase *tc = add_simple_case("two files use import each other", R"SOURCE(
use @import("a.zig");

pub fn main() -> %void {
    ok();
}
        )SOURCE", "OK\n");

        add_source_file(tc, "a.zig", R"SOURCE(
use @import("b.zig");
const io = @import("std").io;

pub const a_text = "OK\n";

pub fn ok() {
    %%io.stdout.printf(b_text);
}
        )SOURCE");

        add_source_file(tc, "b.zig", R"SOURCE(
use @import("a.zig");

pub const b_text = a_text;
        )SOURCE");
    }



    add_simple_case("hello world without libc", R"SOURCE(
const io = @import("std").io;

pub fn main() -> %void {
    %%io.stdout.printf("Hello, world!\n{d4} {x3} {c}\n", u32(12), u16(0x12), u8('a'));
}
    )SOURCE", "Hello, world!\n0012 012 a\n");


    add_simple_case_libc("number literals", R"SOURCE(
const c = @cImport(@cInclude("stdio.h"));

export fn main(argc: c_int, argv: &&u8) -> c_int {
    _ = c.printf(c"\n");

    _ = c.printf(c"0: %llu\n",
             u64(0));
    _ = c.printf(c"320402575052271: %llu\n",
         u64(320402575052271));
    _ = c.printf(c"0x01236789abcdef: %llu\n",
         u64(0x01236789abcdef));
    _ = c.printf(c"0xffffffffffffffff: %llu\n",
         u64(0xffffffffffffffff));
    _ = c.printf(c"0x000000ffffffffffffffff: %llu\n",
         u64(0x000000ffffffffffffffff));
    _ = c.printf(c"0o1777777777777777777777: %llu\n",
         u64(0o1777777777777777777777));
    _ = c.printf(c"0o0000001777777777777777777777: %llu\n",
         u64(0o0000001777777777777777777777));
    _ = c.printf(c"0b1111111111111111111111111111111111111111111111111111111111111111: %llu\n",
         u64(0b1111111111111111111111111111111111111111111111111111111111111111));
    _ = c.printf(c"0b0000001111111111111111111111111111111111111111111111111111111111111111: %llu\n",
         u64(0b0000001111111111111111111111111111111111111111111111111111111111111111));

    _ = c.printf(c"\n");

    _ = c.printf(c"0.0: %a\n",
         f64(0.0));
    _ = c.printf(c"0e0: %a\n",
         f64(0e0));
    _ = c.printf(c"0.0e0: %a\n",
         f64(0.0e0));
    _ = c.printf(c"000000000000000000000000000000000000000000000000000000000.0e0: %a\n",
         f64(000000000000000000000000000000000000000000000000000000000.0e0));
    _ = c.printf(c"0.000000000000000000000000000000000000000000000000000000000e0: %a\n",
         f64(0.000000000000000000000000000000000000000000000000000000000e0));
    _ = c.printf(c"0.0e000000000000000000000000000000000000000000000000000000000: %a\n",
         f64(0.0e000000000000000000000000000000000000000000000000000000000));
    _ = c.printf(c"1.0: %a\n",
         f64(1.0));
    _ = c.printf(c"10.0: %a\n",
         f64(10.0));
    _ = c.printf(c"10.5: %a\n",
         f64(10.5));
    _ = c.printf(c"10.5e5: %a\n",
         f64(10.5e5));
    _ = c.printf(c"10.5e+5: %a\n",
         f64(10.5e+5));
    _ = c.printf(c"50.0e-2: %a\n",
         f64(50.0e-2));
    _ = c.printf(c"50e-2: %a\n",
         f64(50e-2));

    _ = c.printf(c"\n");

    _ = c.printf(c"0x1.0: %a\n",
         f64(0x1.0));
    _ = c.printf(c"0x10.0: %a\n",
         f64(0x10.0));
    _ = c.printf(c"0x100.0: %a\n",
         f64(0x100.0));
    _ = c.printf(c"0x103.0: %a\n",
         f64(0x103.0));
    _ = c.printf(c"0x103.7: %a\n",
         f64(0x103.7));
    _ = c.printf(c"0x103.70: %a\n",
         f64(0x103.70));
    _ = c.printf(c"0x103.70p4: %a\n",
         f64(0x103.70p4));
    _ = c.printf(c"0x103.70p5: %a\n",
         f64(0x103.70p5));
    _ = c.printf(c"0x103.70p+5: %a\n",
         f64(0x103.70p+5));
    _ = c.printf(c"0x103.70p-5: %a\n",
         f64(0x103.70p-5));

    _ = c.printf(c"\n");

    _ = c.printf(c"0b10100.00010e0: %a\n",
         f64(0b10100.00010e0));
    _ = c.printf(c"0o10700.00010e0: %a\n",
         f64(0o10700.00010e0));

    return 0;
}
    )SOURCE", R"OUTPUT(
0: 0
320402575052271: 320402575052271
0x01236789abcdef: 320402575052271
0xffffffffffffffff: 18446744073709551615
0x000000ffffffffffffffff: 18446744073709551615
0o1777777777777777777777: 18446744073709551615
0o0000001777777777777777777777: 18446744073709551615
0b1111111111111111111111111111111111111111111111111111111111111111: 18446744073709551615
0b0000001111111111111111111111111111111111111111111111111111111111111111: 18446744073709551615

0.0: 0x0p+0
0e0: 0x0p+0
0.0e0: 0x0p+0
000000000000000000000000000000000000000000000000000000000.0e0: 0x0p+0
0.000000000000000000000000000000000000000000000000000000000e0: 0x0p+0
0.0e000000000000000000000000000000000000000000000000000000000: 0x0p+0
1.0: 0x1p+0
10.0: 0x1.4p+3
10.5: 0x1.5p+3
10.5e5: 0x1.0059p+20
10.5e+5: 0x1.0059p+20
50.0e-2: 0x1p-1
50e-2: 0x1p-1

0x1.0: 0x1p+0
0x10.0: 0x1p+4
0x100.0: 0x1p+8
0x103.0: 0x1.03p+8
0x103.7: 0x1.037p+8
0x103.70: 0x1.037p+8
0x103.70p4: 0x1.037p+12
0x103.70p5: 0x1.037p+13
0x103.70p+5: 0x1.037p+13
0x103.70p-5: 0x1.037p+3

0b10100.00010e0: 0x1.41p+4
0o10700.00010e0: 0x1.1c0001p+12
)OUTPUT");

    add_simple_case("order-independent declarations", R"SOURCE(
const io = @import("std").io;
const z = io.stdin_fileno;
const x : @typeOf(y) = 1234;
const y : u16 = 5678;
pub fn main() -> %void {
    var x_local : i32 = print_ok(x);
}
fn print_ok(val: @typeOf(x)) -> @typeOf(foo) {
    %%io.stdout.printf("OK\n");
    return 0;
}
const foo : i32 = 0;
    )SOURCE", "OK\n");

    add_simple_case_libc("expose function pointer to C land", R"SOURCE(
const c = @cImport(@cInclude("stdlib.h"));

export fn compare_fn(a: ?&const c_void, b: ?&const c_void) -> c_int {
    const a_int = @ptrcast(&i32, a ?? unreachable);
    const b_int = @ptrcast(&i32, b ?? unreachable);
    if (*a_int < *b_int) {
        -1
    } else if (*a_int > *b_int) {
        1
    } else {
        c_int(0)
    }
}

export fn main() -> c_int {
    var array = []u32 { 1, 7, 3, 2, 0, 9, 4, 8, 6, 5 };

    c.qsort(@ptrcast(&c_void, &array[0]), c_ulong(array.len), @sizeOf(i32), compare_fn);

    for (array) |item, i| {
        if (item != i) {
            c.abort();
        }
    }

    return 0;
}
    )SOURCE", "");



    add_simple_case_libc("casting between float and integer types", R"SOURCE(
const c = @cImport(@cInclude("stdio.h"));
export fn main(argc: c_int, argv: &&u8) -> c_int {
    const small: f32 = 3.25;
    const x: f64 = small;
    const y = i32(x);
    const z = f64(y);
    _ = c.printf(c"%.2f\n%d\n%.2f\n%.2f\n", x, y, z, f64(-0.4));
    return 0;
}
    )SOURCE", "3.25\n3\n3.00\n-0.40\n");


    add_simple_case("same named methods in incomplete struct", R"SOURCE(
const io = @import("std").io;

const Foo = struct {
    field1: Bar,

    fn method(a: &const Foo) -> bool { true }
};

const Bar = struct {
    field2: i32,

    fn method(b: &const Bar) -> bool { true }
};

pub fn main() -> %void {
    const bar = Bar {.field2 = 13,};
    const foo = Foo {.field1 = bar,};
    if (!foo.method()) {
        %%io.stdout.printf("BAD\n");
    }
    if (!bar.method()) {
        %%io.stdout.printf("BAD\n");
    }
    %%io.stdout.printf("OK\n");
}
    )SOURCE", "OK\n");


    add_simple_case("defer with only fallthrough", R"SOURCE(
const io = @import("std").io;
pub fn main() -> %void {
    %%io.stdout.printf("before\n");
    defer %%io.stdout.printf("defer1\n");
    defer %%io.stdout.printf("defer2\n");
    defer %%io.stdout.printf("defer3\n");
    %%io.stdout.printf("after\n");
}
    )SOURCE", "before\nafter\ndefer3\ndefer2\ndefer1\n");


    add_simple_case("defer with return", R"SOURCE(
const io = @import("std").io;
const os = @import("std").os;
pub fn main() -> %void {
    %%io.stdout.printf("before\n");
    defer %%io.stdout.printf("defer1\n");
    defer %%io.stdout.printf("defer2\n");
    if (os.args.count() == 1) return;
    defer %%io.stdout.printf("defer3\n");
    %%io.stdout.printf("after\n");
}
    )SOURCE", "before\ndefer2\ndefer1\n");


    add_simple_case("%defer and it fails", R"SOURCE(
const io = @import("std").io;
pub fn main() -> %void {
    do_test() %% return;
}
fn do_test() -> %void {
    %%io.stdout.printf("before\n");
    defer %%io.stdout.printf("defer1\n");
    %defer %%io.stdout.printf("deferErr\n");
    %return its_gonna_fail();
    defer %%io.stdout.printf("defer3\n");
    %%io.stdout.printf("after\n");
}
error IToldYouItWouldFail;
fn its_gonna_fail() -> %void {
    return error.IToldYouItWouldFail;
}
    )SOURCE", "before\ndeferErr\ndefer1\n");


    add_simple_case("%defer and it passes", R"SOURCE(
const io = @import("std").io;
pub fn main() -> %void {
    do_test() %% return;
}
fn do_test() -> %void {
    %%io.stdout.printf("before\n");
    defer %%io.stdout.printf("defer1\n");
    %defer %%io.stdout.printf("deferErr\n");
    %return its_gonna_pass();
    defer %%io.stdout.printf("defer3\n");
    %%io.stdout.printf("after\n");
}
fn its_gonna_pass() -> %void { }
    )SOURCE", "before\nafter\ndefer3\ndefer1\n");


    {
        TestCase *tc = add_simple_case("@embedFile", R"SOURCE(
const foo_txt = @embedFile("foo.txt");
const io = @import("std").io;

pub fn main() -> %void {
    %%io.stdout.printf(foo_txt);
}
        )SOURCE", "1234\nabcd\n");

        add_source_file(tc, "foo.txt", "1234\nabcd\n");
    }
}

////////////////////////////////////////////////////////////////////////////////////

static void add_build_examples(void) {
    add_example_compile("example/hello_world/hello.zig");
    add_example_compile_libc("example/hello_world/hello_libc.zig");
    add_example_compile("example/cat/main.zig");
    add_example_compile("example/guess_number/main.zig");
}


////////////////////////////////////////////////////////////////////////////////////

static void add_compile_failure_test_cases(void) {
    add_compile_fail_case("multiple function definitions", R"SOURCE(
fn a() {}
fn a() {}
export fn entry() { a(); }
    )SOURCE", 1, ".tmp_source.zig:3:1: error: redefinition of 'a'");

    add_compile_fail_case("unreachable with return", R"SOURCE(
fn a() -> noreturn {return;}
export fn entry() { a(); }
    )SOURCE", 1, ".tmp_source.zig:2:21: error: expected type 'noreturn', found 'void'");

    add_compile_fail_case("control reaches end of non-void function", R"SOURCE(
fn a() -> i32 {}
export fn entry() { _ = a(); }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: expected type 'i32', found 'void'");

    add_compile_fail_case("undefined function call", R"SOURCE(
export fn a() {
    b();
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: use of undeclared identifier 'b'");

    add_compile_fail_case("wrong number of arguments", R"SOURCE(
export fn a() {
    b(1);
}
fn b(a: i32, b: i32, c: i32) { }
    )SOURCE", 1, ".tmp_source.zig:3:6: error: expected 3 arguments, found 1");

    add_compile_fail_case("invalid type", R"SOURCE(
fn a() -> bogus {}
export fn entry() { _ = a(); }
    )SOURCE", 1, ".tmp_source.zig:2:11: error: use of undeclared identifier 'bogus'");

    add_compile_fail_case("pointer to unreachable", R"SOURCE(
fn a() -> &noreturn {}
export fn entry() { _ = a(); }
    )SOURCE", 1, ".tmp_source.zig:2:12: error: pointer to unreachable not allowed");

    add_compile_fail_case("unreachable code", R"SOURCE(
export fn a() {
    return;
    b();
}

fn b() {}
    )SOURCE", 1, ".tmp_source.zig:4:6: error: unreachable code");

    add_compile_fail_case("bad import", R"SOURCE(
const bogus = @import("bogus-does-not-exist.zig");
export fn entry() { bogus.bogo(); }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: unable to find 'bogus-does-not-exist.zig'");

    add_compile_fail_case("undeclared identifier", R"SOURCE(
export fn a() {
    b +
    c
}
    )SOURCE", 2,
            ".tmp_source.zig:3:5: error: use of undeclared identifier 'b'",
            ".tmp_source.zig:4:5: error: use of undeclared identifier 'c'");

    add_compile_fail_case("parameter redeclaration", R"SOURCE(
fn f(a : i32, a : i32) {
}
export fn entry() { f(1, 2); }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: redeclaration of variable 'a'");

    add_compile_fail_case("local variable redeclaration", R"SOURCE(
export fn f() {
    const a : i32 = 0;
    const a = 0;
}
    )SOURCE", 1, ".tmp_source.zig:4:5: error: redeclaration of variable 'a'");

    add_compile_fail_case("local variable redeclares parameter", R"SOURCE(
fn f(a : i32) {
    const a = 0;
}
export fn entry() { f(1); }
    )SOURCE", 1, ".tmp_source.zig:3:5: error: redeclaration of variable 'a'");

    add_compile_fail_case("variable has wrong type", R"SOURCE(
export fn f() -> i32 {
    const a = c"a";
    a
}
    )SOURCE", 1, ".tmp_source.zig:4:5: error: expected type 'i32', found '&const u8'");

    add_compile_fail_case("if condition is bool, not int", R"SOURCE(
export fn f() {
    if (0) {}
}
    )SOURCE", 1, ".tmp_source.zig:3:9: error: integer value 0 cannot be implicitly casted to type 'bool'");

    add_compile_fail_case("assign unreachable", R"SOURCE(
export fn f() {
    const a = return;
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: unreachable code");

    add_compile_fail_case("unreachable variable", R"SOURCE(
export fn f() {
    const a: noreturn = {};
}
    )SOURCE", 1, ".tmp_source.zig:3:14: error: variable of type 'noreturn' not allowed");

    add_compile_fail_case("unreachable parameter", R"SOURCE(
fn f(a: noreturn) {}
export fn entry() { f(); }
    )SOURCE", 1, ".tmp_source.zig:2:9: error: parameter of type 'noreturn' not allowed");

    add_compile_fail_case("bad assignment target", R"SOURCE(
export fn f() {
    3 = 3;
}
    )SOURCE", 1, ".tmp_source.zig:3:7: error: cannot assign to constant");

    add_compile_fail_case("assign to constant variable", R"SOURCE(
export fn f() {
    const a = 3;
    a = 4;
}
    )SOURCE", 1, ".tmp_source.zig:4:7: error: cannot assign to constant");

    add_compile_fail_case("use of undeclared identifier", R"SOURCE(
export fn f() {
    b = 3;
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: use of undeclared identifier 'b'");

    add_compile_fail_case("const is a statement, not an expression", R"SOURCE(
export fn f() {
    (const a = 0);
}
    )SOURCE", 1, ".tmp_source.zig:3:6: error: invalid token: 'const'");

    add_compile_fail_case("array access of undeclared identifier", R"SOURCE(
export fn f() {
    i[i] = i[i];
}
    )SOURCE", 2, ".tmp_source.zig:3:5: error: use of undeclared identifier 'i'",
                 ".tmp_source.zig:3:12: error: use of undeclared identifier 'i'");

    add_compile_fail_case("array access of non array", R"SOURCE(
export fn f() {
    var bad : bool = undefined;
    bad[bad] = bad[bad];
}
    )SOURCE", 2, ".tmp_source.zig:4:8: error: array access of non-array type 'bool'",
                 ".tmp_source.zig:4:19: error: array access of non-array type 'bool'");

    add_compile_fail_case("array access with non integer index", R"SOURCE(
export fn f() {
    var array = "aoeu";
    var bad = false;
    array[bad] = array[bad];
}
    )SOURCE", 2, ".tmp_source.zig:5:11: error: expected type 'usize', found 'bool'",
                 ".tmp_source.zig:5:24: error: expected type 'usize', found 'bool'");

    add_compile_fail_case("write to const global variable", R"SOURCE(
const x : i32 = 99;
fn f() {
    x = 1;
}
export fn entry() { f(); }
    )SOURCE", 1, ".tmp_source.zig:4:7: error: cannot assign to constant");


    add_compile_fail_case("missing else clause", R"SOURCE(
fn f(b: bool) {
    const x : i32 = if (b) { 1 };
    const y = if (b) { i32(1) };
}
export fn entry() { f(true); }
    )SOURCE", 2, ".tmp_source.zig:3:30: error: integer value 1 cannot be implicitly casted to type 'void'",
                 ".tmp_source.zig:4:15: error: incompatible types: 'i32' and 'void'");

    add_compile_fail_case("direct struct loop", R"SOURCE(
const A = struct { a : A, };
export fn entry() -> usize { @sizeOf(A) }
    )SOURCE", 1, ".tmp_source.zig:2:11: error: struct 'A' contains itself");

    add_compile_fail_case("indirect struct loop", R"SOURCE(
const A = struct { b : B, };
const B = struct { c : C, };
const C = struct { a : A, };
export fn entry() -> usize { @sizeOf(A) }
    )SOURCE", 1, ".tmp_source.zig:2:11: error: struct 'A' contains itself");

    add_compile_fail_case("invalid struct field", R"SOURCE(
const A = struct { x : i32, };
export fn f() {
    var a : A = undefined;
    a.foo = 1;
    const y = a.bar;
}
    )SOURCE", 2,
            ".tmp_source.zig:5:6: error: no member named 'foo' in 'A'",
            ".tmp_source.zig:6:16: error: no member named 'bar' in 'A'");

    add_compile_fail_case("redefinition of struct", R"SOURCE(
const A = struct { x : i32, };
const A = struct { y : i32, };
    )SOURCE", 1, ".tmp_source.zig:3:1: error: redefinition of 'A'");

    add_compile_fail_case("redefinition of enums", R"SOURCE(
const A = enum {};
const A = enum {};
    )SOURCE", 1, ".tmp_source.zig:3:1: error: redefinition of 'A'");

    add_compile_fail_case("redefinition of global variables", R"SOURCE(
var a : i32 = 1;
var a : i32 = 2;
    )SOURCE", 2,
            ".tmp_source.zig:3:1: error: redefinition of 'a'",
            ".tmp_source.zig:2:1: note: previous definition is here");

    add_compile_fail_case("byvalue struct parameter in exported function", R"SOURCE(
const A = struct { x : i32, };
export fn f(a : A) {}
    )SOURCE", 1, ".tmp_source.zig:3:13: error: byvalue types not yet supported on extern function parameters");

    add_compile_fail_case("byvalue struct return value in exported function", R"SOURCE(
const A = struct { x: i32, };
export fn f() -> A {
    A {.x = 1234 }
}
    )SOURCE", 1, ".tmp_source.zig:3:18: error: byvalue types not yet supported on extern function return values");

    add_compile_fail_case("duplicate field in struct value expression", R"SOURCE(
const A = struct {
    x : i32,
    y : i32,
    z : i32,
};
export fn f() {
    const a = A {
        .z = 1,
        .y = 2,
        .x = 3,
        .z = 4,
    };
}
    )SOURCE", 1, ".tmp_source.zig:12:9: error: duplicate field");

    add_compile_fail_case("missing field in struct value expression", R"SOURCE(
const A = struct {
    x : i32,
    y : i32,
    z : i32,
};
export fn f() {
    // we want the error on the '{' not the 'A' because
    // the A could be a complicated expression
    const a = A {
        .z = 4,
        .y = 2,
    };
}
    )SOURCE", 1, ".tmp_source.zig:10:17: error: missing field: 'x'");

    add_compile_fail_case("invalid field in struct value expression", R"SOURCE(
const A = struct {
    x : i32,
    y : i32,
    z : i32,
};
export fn f() {
    const a = A {
        .z = 4,
        .y = 2,
        .foo = 42,
    };
}
    )SOURCE", 1, ".tmp_source.zig:11:9: error: no member named 'foo' in 'A'");

    add_compile_fail_case("invalid break expression", R"SOURCE(
export fn f() {
    break;
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: 'break' expression outside loop");

    add_compile_fail_case("invalid continue expression", R"SOURCE(
export fn f() {
    continue;
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: 'continue' expression outside loop");

    add_compile_fail_case("invalid maybe type", R"SOURCE(
export fn f() {
    if (const x ?= true) { }
}
    )SOURCE", 1, ".tmp_source.zig:3:20: error: expected nullable type, found 'bool'");

    add_compile_fail_case("cast unreachable", R"SOURCE(
fn f() -> i32 {
    i32(return 1)
}
export fn entry() { _ = f(); }
    )SOURCE", 1, ".tmp_source.zig:3:8: error: unreachable code");

    add_compile_fail_case("invalid builtin fn", R"SOURCE(
fn f() -> @bogus(foo) {
}
export fn entry() { _ = f(); }
    )SOURCE", 1, ".tmp_source.zig:2:11: error: invalid builtin function: 'bogus'");

    add_compile_fail_case("top level decl dependency loop", R"SOURCE(
const a : @typeOf(b) = 0;
const b : @typeOf(a) = 0;
export fn entry() {
    const c = a + b;
}
    )SOURCE", 1, ".tmp_source.zig:2:1: error: 'a' depends on itself");

    add_compile_fail_case("noalias on non pointer param", R"SOURCE(
fn f(noalias x: i32) {}
export fn entry() { f(1234); }
    )SOURCE", 1, ".tmp_source.zig:2:6: error: noalias on non-pointer parameter");

    add_compile_fail_case("struct init syntax for array", R"SOURCE(
const foo = []u16{.x = 1024,};
export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:2:18: error: type '[]u16' does not support struct initialization syntax");

    add_compile_fail_case("type variables must be constant", R"SOURCE(
var foo = u8;
export fn entry() -> foo {
    return 1;
}
    )SOURCE", 1, ".tmp_source.zig:2:1: error: variable of type 'type' must be constant");


    add_compile_fail_case("variables shadowing types", R"SOURCE(
const Foo = struct {};
const Bar = struct {};

fn f(Foo: i32) {
    var Bar : i32 = undefined;
}

export fn entry() {
    f(1234);
}
    )SOURCE", 4,
            ".tmp_source.zig:5:6: error: redefinition of 'Foo'",
            ".tmp_source.zig:2:1: note: previous definition is here",
            ".tmp_source.zig:6:5: error: redefinition of 'Bar'",
            ".tmp_source.zig:3:1: note: previous definition is here");

    add_compile_fail_case("multiple else prongs in a switch", R"SOURCE(
fn f(x: u32) {
    const value: bool = switch (x) {
        1234 => false,
        else => true,
        else => true,
    };
}
export fn entry() {
    f(1234);
}
    )SOURCE", 1, ".tmp_source.zig:6:9: error: multiple else prongs in switch expression");

    add_compile_fail_case("global variable initializer must be constant expression", R"SOURCE(
extern fn foo() -> i32;
const x = foo();
export fn entry() -> i32 { x }
    )SOURCE", 1, ".tmp_source.zig:3:11: error: unable to evaluate constant expression");

    add_compile_fail_case("array concatenation with wrong type", R"SOURCE(
const src = "aoeu";
const derp = usize(1234);
const a = derp ++ "foo";

export fn entry() -> usize { @sizeOf(@typeOf(a)) }
    )SOURCE", 1, ".tmp_source.zig:4:11: error: expected array or C string literal, found 'usize'");

    add_compile_fail_case("non compile time array concatenation", R"SOURCE(
fn f() -> []u8 {
    s ++ "foo"
}
var s: [10]u8 = undefined;
export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:3:5: error: unable to evaluate constant expression");

    add_compile_fail_case("@cImport with bogus include", R"SOURCE(
const c = @cImport(@cInclude("bogus.h"));
export fn entry() -> usize { @sizeOf(@typeOf(c.bogo)) }
    )SOURCE", 2, ".tmp_source.zig:2:11: error: C import failed",
                 ".h:1:10: note: 'bogus.h' file not found");

    add_compile_fail_case("address of number literal", R"SOURCE(
const x = 3;
const y = &x;
fn foo() -> &const i32 { y }
export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:4:26: error: expected type '&const i32', found '&const (integer literal)'");

    add_compile_fail_case("integer overflow error", R"SOURCE(
const x : u8 = 300;
export fn entry() -> usize { @sizeOf(@typeOf(x)) }
    )SOURCE", 1, ".tmp_source.zig:2:16: error: integer value 300 cannot be implicitly casted to type 'u8'");

    add_compile_fail_case("incompatible number literals", R"SOURCE(
const x = 2 == 2.0;
export fn entry() -> usize { @sizeOf(@typeOf(x)) }
    )SOURCE", 1, ".tmp_source.zig:2:11: error: integer value 2 cannot be implicitly casted to type '(float literal)'");

    add_compile_fail_case("missing function call param", R"SOURCE(
const Foo = struct {
    a: i32,
    b: i32,

    fn member_a(foo: &const Foo) -> i32 {
        return foo.a;
    }
    fn member_b(foo: &const Foo) -> i32 {
        return foo.b;
    }
};

const member_fn_type = @typeOf(Foo.member_a);
const members = []member_fn_type {
    Foo.member_a,
    Foo.member_b,
};

fn f(foo: &const Foo, index: usize) {
    const result = members[index]();
}

export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:21:34: error: expected 1 arguments, found 0");

    add_compile_fail_case("missing function name and param name", R"SOURCE(
fn () {}
fn f(i32) {}
export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 2,
            ".tmp_source.zig:2:1: error: missing function name",
            ".tmp_source.zig:3:6: error: missing parameter name");

    add_compile_fail_case("wrong function type", R"SOURCE(
const fns = []fn(){ a, b, c };
fn a() -> i32 {0}
fn b() -> i32 {1}
fn c() -> i32 {2}
export fn entry() -> usize { @sizeOf(@typeOf(fns)) }
    )SOURCE", 1, ".tmp_source.zig:2:21: error: expected type 'fn()', found 'fn() -> i32'");

    add_compile_fail_case("extern function pointer mismatch", R"SOURCE(
const fns = [](fn(i32)->i32){ a, b, c };
pub fn a(x: i32) -> i32 {x + 0}
pub fn b(x: i32) -> i32 {x + 1}
export fn c(x: i32) -> i32 {x + 2}

export fn entry() -> usize { @sizeOf(@typeOf(fns)) }
    )SOURCE", 1, ".tmp_source.zig:2:37: error: expected type 'fn(i32) -> i32', found 'extern fn(i32) -> i32'");


    add_compile_fail_case("implicit cast from f64 to f32", R"SOURCE(
const x : f64 = 1.0;
const y : f32 = x;

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 1, ".tmp_source.zig:3:17: error: expected type 'f32', found 'f64'");


    add_compile_fail_case("colliding invalid top level functions", R"SOURCE(
fn func() -> bogus {}
fn func() -> bogus {}
export fn entry() -> usize { @sizeOf(@typeOf(func)) }
    )SOURCE", 2,
            ".tmp_source.zig:3:1: error: redefinition of 'func'",
            ".tmp_source.zig:2:14: error: use of undeclared identifier 'bogus'");


    add_compile_fail_case("bogus compile var", R"SOURCE(
const x = @compileVar("bogus");
export fn entry() -> usize { @sizeOf(@typeOf(x)) }
    )SOURCE", 1, ".tmp_source.zig:2:23: error: unrecognized compile variable: 'bogus'");


    add_compile_fail_case("non constant expression in array size outside function", R"SOURCE(
const Foo = struct {
    y: [get()]u8,
};
var global_var: usize = 1;
fn get() -> usize { global_var }

export fn entry() -> usize { @sizeOf(@typeOf(Foo)) }
    )SOURCE", 3,
            ".tmp_source.zig:6:21: error: unable to evaluate constant expression",
            ".tmp_source.zig:3:12: note: called from here",
            ".tmp_source.zig:3:8: note: called from here");


    add_compile_fail_case("addition with non numbers", R"SOURCE(
const Foo = struct {
    field: i32,
};
const x = Foo {.field = 1} + Foo {.field = 2};

export fn entry() -> usize { @sizeOf(@typeOf(x)) }
    )SOURCE", 1, ".tmp_source.zig:5:28: error: invalid operands to binary expression: 'Foo' and 'Foo'");


    add_compile_fail_case("division by zero", R"SOURCE(
const lit_int_x = 1 / 0;
const lit_float_x = 1.0 / 0.0;
const int_x = i32(1) / i32(0);
const float_x = f32(1.0) / f32(0.0);

export fn entry1() -> usize { @sizeOf(@typeOf(lit_int_x)) }
export fn entry2() -> usize { @sizeOf(@typeOf(lit_float_x)) }
export fn entry3() -> usize { @sizeOf(@typeOf(int_x)) }
export fn entry4() -> usize { @sizeOf(@typeOf(float_x)) }
    )SOURCE", 4,
            ".tmp_source.zig:2:21: error: division by zero is undefined",
            ".tmp_source.zig:3:25: error: division by zero is undefined",
            ".tmp_source.zig:4:22: error: division by zero is undefined",
            ".tmp_source.zig:5:26: error: division by zero is undefined");


    add_compile_fail_case("missing switch prong", R"SOURCE(
const Number = enum {
    One,
    Two,
    Three,
    Four,
};
fn f(n: Number) -> i32 {
    switch (n) {
        Number.One => 1,
        Number.Two => 2,
        Number.Three => i32(3),
    }
}

export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:9:5: error: enumeration value 'Number.Four' not handled in switch");

    add_compile_fail_case("normal string with newline", R"SOURCE(
const foo = "a
b";

export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:2:13: error: newline not allowed in string literal");

    add_compile_fail_case("invalid comparison for function pointers", R"SOURCE(
fn foo() {}
const invalid = foo > foo;

export fn entry() -> usize { @sizeOf(@typeOf(invalid)) }
    )SOURCE", 1, ".tmp_source.zig:3:21: error: operator not allowed for type 'fn()'");

    add_compile_fail_case("generic function instance with non-constant expression", R"SOURCE(
fn foo(comptime x: i32, y: i32) -> i32 { return x + y; }
fn test1(a: i32, b: i32) -> i32 {
    return foo(a, b);
}

export fn entry() -> usize { @sizeOf(@typeOf(test1)) }
    )SOURCE", 1, ".tmp_source.zig:4:16: error: unable to evaluate constant expression");

    add_compile_fail_case("goto jumping into block", R"SOURCE(
export fn f() {
    {
a_label:
    }
    goto a_label;
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: no label in scope named 'a_label'");

    add_compile_fail_case("goto jumping past a defer", R"SOURCE(
fn f(b: bool) {
    if (b) goto label;
    defer derp();
label:
}
fn derp(){}

export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:3:12: error: no label in scope named 'label'");

    add_compile_fail_case("assign null to non-nullable pointer", R"SOURCE(
const a: &u8 = null;

export fn entry() -> usize { @sizeOf(@typeOf(a)) }
    )SOURCE", 1, ".tmp_source.zig:2:16: error: expected type '&u8', found '(null)'");

    add_compile_fail_case("indexing an array of size zero", R"SOURCE(
const array = []u8{};
export fn foo() {
    const pointer = &array[0];
}
    )SOURCE", 1, ".tmp_source.zig:4:27: error: index 0 outside array of size 0");

    add_compile_fail_case("compile time division by zero", R"SOURCE(
const y = foo(0);
fn foo(x: i32) -> i32 {
    1 / x
}

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:7: error: division by zero is undefined",
            ".tmp_source.zig:2:14: note: called from here");

    add_compile_fail_case("branch on undefined value", R"SOURCE(
const x = if (undefined) true else false;

export fn entry() -> usize { @sizeOf(@typeOf(x)) }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: use of undefined value");


    add_compile_fail_case("endless loop in function evaluation", R"SOURCE(
const seventh_fib_number = fibbonaci(7);
fn fibbonaci(x: i32) -> i32 {
    return fibbonaci(x - 1) + fibbonaci(x - 2);
}

export fn entry() -> usize { @sizeOf(@typeOf(seventh_fib_number)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:21: error: evaluation exceeded 1000 backwards branches",
            ".tmp_source.zig:4:21: note: called from here");

    add_compile_fail_case("@embedFile with bogus file", R"SOURCE(
const resource = @embedFile("bogus.txt");

export fn entry() -> usize { @sizeOf(@typeOf(resource)) }
    )SOURCE", 2, ".tmp_source.zig:2:29: error: unable to find '", "/bogus.txt'");

    add_compile_fail_case("non-const expression in struct literal outside function", R"SOURCE(
const Foo = struct {
    x: i32,
};
const a = Foo {.x = get_it()};
extern fn get_it() -> i32;

export fn entry() -> usize { @sizeOf(@typeOf(a)) }
    )SOURCE", 1, ".tmp_source.zig:5:21: error: unable to evaluate constant expression");

    add_compile_fail_case("non-const expression function call with struct return value outside function", R"SOURCE(
const Foo = struct {
    x: i32,
};
const a = get_it();
fn get_it() -> Foo {
    global_side_effect = true;
    Foo {.x = 13}
}
var global_side_effect = false;

export fn entry() -> usize { @sizeOf(@typeOf(a)) }
    )SOURCE", 2,
            ".tmp_source.zig:7:24: error: unable to evaluate constant expression",
            ".tmp_source.zig:5:17: note: called from here");

    add_compile_fail_case("undeclared identifier error should mark fn as impure", R"SOURCE(
export fn foo() {
    test_a_thing();
}
fn test_a_thing() {
    bad_fn_call();
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: use of undeclared identifier 'bad_fn_call'");

    add_compile_fail_case("illegal comparison of types", R"SOURCE(
fn bad_eql_1(a: []u8, b: []u8) -> bool {
    a == b
}
const EnumWithData = enum {
    One,
    Two: i32,
};
fn bad_eql_2(a: &const EnumWithData, b: &const EnumWithData) -> bool {
    *a == *b
}

export fn entry1() -> usize { @sizeOf(@typeOf(bad_eql_1)) }
export fn entry2() -> usize { @sizeOf(@typeOf(bad_eql_2)) }
    )SOURCE", 2,
            ".tmp_source.zig:3:7: error: operator not allowed for type '[]u8'",
            ".tmp_source.zig:10:8: error: operator not allowed for type 'EnumWithData'");

    add_compile_fail_case("non-const switch number literal", R"SOURCE(
export fn foo() {
    const x = switch (bar()) {
        1, 2 => 1,
        3, 4 => 2,
        else => 3,
    };
}
fn bar() -> i32 {
    2
}
    )SOURCE", 1, ".tmp_source.zig:3:15: error: unable to infer expression type");

    add_compile_fail_case("atomic orderings of cmpxchg - failure stricter than success", R"SOURCE(
export fn f() {
    var x: i32 = 1234;
    while (!@cmpxchg(&x, 1234, 5678, AtomicOrder.Monotonic, AtomicOrder.SeqCst)) {}
}
    )SOURCE", 1, ".tmp_source.zig:4:72: error: failure atomic ordering must be no stricter than success");

    add_compile_fail_case("atomic orderings of cmpxchg - success Monotonic or stricter", R"SOURCE(
export fn f() {
    var x: i32 = 1234;
    while (!@cmpxchg(&x, 1234, 5678, AtomicOrder.Unordered, AtomicOrder.Unordered)) {}
}
    )SOURCE", 1, ".tmp_source.zig:4:49: error: success atomic ordering must be Monotonic or stricter");

    add_compile_fail_case("negation overflow in function evaluation", R"SOURCE(
const y = neg(-128);
fn neg(x: i8) -> i8 {
    -x
}

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:5: error: negation caused overflow",
            ".tmp_source.zig:2:14: note: called from here");

    add_compile_fail_case("add overflow in function evaluation", R"SOURCE(
const y = add(65530, 10);
fn add(a: u16, b: u16) -> u16 {
    a + b
}

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:7: error: operation caused overflow",
            ".tmp_source.zig:2:14: note: called from here");


    add_compile_fail_case("sub overflow in function evaluation", R"SOURCE(
const y = sub(10, 20);
fn sub(a: u16, b: u16) -> u16 {
    a - b
}

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:7: error: operation caused overflow",
            ".tmp_source.zig:2:14: note: called from here");

    add_compile_fail_case("mul overflow in function evaluation", R"SOURCE(
const y = mul(300, 6000);
fn mul(a: u16, b: u16) -> u16 {
    a * b
}

export fn entry() -> usize { @sizeOf(@typeOf(y)) }
    )SOURCE", 2,
            ".tmp_source.zig:4:7: error: operation caused overflow",
            ".tmp_source.zig:2:14: note: called from here");

    add_compile_fail_case("truncate sign mismatch", R"SOURCE(
fn f() -> i8 {
    const x: u32 = 10;
    @truncate(i8, x)
}

export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:4:19: error: expected signed integer type, found 'u32'");

    add_compile_fail_case("%return in function with non error return type", R"SOURCE(
export fn f() {
    %return something();
}
fn something() -> %void { }
    )SOURCE", 1,
            ".tmp_source.zig:3:5: error: expected type 'void', found 'error'");

    add_compile_fail_case("wrong return type for main", R"SOURCE(
pub fn main() { }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: expected return type of main to be '%void', instead is 'void'");

    add_compile_fail_case("double ?? on main return value", R"SOURCE(
pub fn main() -> ??void {
}
    )SOURCE", 1, ".tmp_source.zig:2:18: error: expected return type of main to be '%void', instead is '??void'");

    add_compile_fail_case("invalid pointer for var type", R"SOURCE(
extern fn ext() -> usize;
var bytes: [ext()]u8 = undefined;
export fn f() {
    for (bytes) |*b, i| {
        *b = u8(i);
    }
}
    )SOURCE", 1, ".tmp_source.zig:3:13: error: unable to evaluate constant expression");

    add_compile_fail_case("export function with comptime parameter", R"SOURCE(
export fn foo(comptime x: i32, y: i32) -> i32{
    x + y
}
    )SOURCE", 1, ".tmp_source.zig:2:15: error: comptime parameter not allowed in extern function");

    add_compile_fail_case("extern function with comptime parameter", R"SOURCE(
extern fn foo(comptime x: i32, y: i32) -> i32;
fn f() -> i32 {
    foo(1, 2)
}
export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:2:15: error: comptime parameter not allowed in extern function");

    add_compile_fail_case("convert fixed size array to slice with invalid size", R"SOURCE(
export fn f() {
    var array: [5]u8 = undefined;
    var foo = ([]const u32)(array)[0];
}
    )SOURCE", 1, ".tmp_source.zig:4:28: error: unable to convert [5]u8 to []const u32: size mismatch");

    add_compile_fail_case("non-pure function returns type", R"SOURCE(
var a: u32 = 0;
pub fn List(comptime T: type) -> type {
    a += 1;
    SmallList(T, 8)
}

pub fn SmallList(comptime T: type, comptime STATIC_SIZE: usize) -> type {
    struct {
        items: []T,
        length: usize,
        prealloc_items: [STATIC_SIZE]T,
    }
}

export fn function_with_return_type_type() {
    var list: List(i32) = undefined;
    list.length = 10;
}

    )SOURCE", 2,
            ".tmp_source.zig:4:7: error: unable to evaluate constant expression",
            ".tmp_source.zig:17:19: note: called from here");

    add_compile_fail_case("bogus method call on slice", R"SOURCE(
var self = "aoeu";
fn f(m: []const u8) {
    m.copy(u8, self[0...], m);
}
export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:4:6: error: no member named 'copy' in '[]const u8'");

    add_compile_fail_case("wrong number of arguments for method fn call", R"SOURCE(
const Foo = struct {
    fn method(self: &const Foo, a: i32) {}
};
fn f(foo: &const Foo) {

    foo.method(1, 2);
}
export fn entry() -> usize { @sizeOf(@typeOf(f)) }
    )SOURCE", 1, ".tmp_source.zig:7:15: error: expected 2 arguments, found 3");

    add_compile_fail_case("assign through constant pointer", R"SOURCE(
export fn f() {
  var cstr = c"Hat";
  cstr[0] = 'W';
}
    )SOURCE", 1, ".tmp_source.zig:4:11: error: cannot assign to constant");

    add_compile_fail_case("assign through constant slice", R"SOURCE(
export fn f() {
  var cstr: []const u8 = "Hat";
  cstr[0] = 'W';
}
    )SOURCE", 1, ".tmp_source.zig:4:11: error: cannot assign to constant");

    add_compile_fail_case("main function with bogus args type", R"SOURCE(
pub fn main(args: [][]bogus) -> %void {}
    )SOURCE", 1, ".tmp_source.zig:2:23: error: use of undeclared identifier 'bogus'");

    add_compile_fail_case("for loop missing element param", R"SOURCE(
fn foo(blah: []u8) {
    for (blah) { }
}
export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:3:5: error: for loop expression missing element parameter");

    add_compile_fail_case("misspelled type with pointer only reference", R"SOURCE(
const JasonHM = u8;
const JasonList = &JsonNode;

const JsonOA = enum {
    JSONArray: JsonList,
    JSONObject: JasonHM,
};

const JsonType = enum {
    JSONNull: void,
    JSONInteger: isize,
    JSONDouble: f64,
    JSONBool: bool,
    JSONString: []u8,
    JSONArray,
    JSONObject,
};

pub const JsonNode = struct {
    kind: JsonType,
    jobject: ?JsonOA,
};

fn foo() {
    var jll: JasonList = undefined;
    jll.init(1234);
    var jd = JsonNode {.kind = JsonType.JSONArray , .jobject = JsonOA.JSONArray {jll} };
}

export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:6:16: error: use of undeclared identifier 'JsonList'");

    add_compile_fail_case("method call with first arg type primitive", R"SOURCE(
const Foo = struct {
    x: i32,

    fn init(x: i32) -> Foo {
        Foo {
            .x = x,
        }
    }
};

export fn f() {
    const derp = Foo.init(3);

    derp.init();
}
    )SOURCE", 1, ".tmp_source.zig:15:5: error: expected type 'i32', found '&const Foo'");

    add_compile_fail_case("method call with first arg type wrong container", R"SOURCE(
pub const List = struct {
    len: usize,
    allocator: &Allocator,

    pub fn init(allocator: &Allocator) -> List {
        List {
            .len = 0,
            .allocator = allocator,
        }
    }
};

pub var global_allocator = Allocator {
    .field = 1234,
};

pub const Allocator = struct {
    field: i32,
};

export fn foo() {
    var x = List.init(&global_allocator);
    x.init();
}
    )SOURCE", 1, ".tmp_source.zig:24:5: error: expected type '&Allocator', found '&List'");

    add_compile_fail_case("binary not on number literal", R"SOURCE(
const TINY_QUANTUM_SHIFT = 4;
const TINY_QUANTUM_SIZE = 1 << TINY_QUANTUM_SHIFT;
var block_aligned_stuff: usize = (4 + TINY_QUANTUM_SIZE) & ~(TINY_QUANTUM_SIZE - 1);

export fn entry() -> usize { @sizeOf(@typeOf(block_aligned_stuff)) }
    )SOURCE", 1, ".tmp_source.zig:4:60: error: unable to perform binary not operation on type '(integer literal)'");

    {
        TestCase *tc = add_compile_fail_case("multiple files with private function error", R"SOURCE(
const foo = @import("foo.zig");

export fn callPrivFunction() {
    foo.privateFunction();
}
        )SOURCE", 2,
            ".tmp_source.zig:5:8: error: 'privateFunction' is private",
            "foo.zig:2:1: note: declared here");

        add_source_file(tc, "foo.zig", R"SOURCE(
fn privateFunction() { }
        )SOURCE");
    }

    add_compile_fail_case("container init with non-type", R"SOURCE(
const zero: i32 = 0;
const a = zero{1};

export fn entry() -> usize { @sizeOf(@typeOf(a)) }
    )SOURCE", 1, ".tmp_source.zig:3:11: error: expected type, found 'i32'");

    add_compile_fail_case("assign to constant field", R"SOURCE(
const Foo = struct {
    field: i32,
};
export fn derp() {
    const f = Foo {.field = 1234,};
    f.field = 0;
}
    )SOURCE", 1, ".tmp_source.zig:7:13: error: cannot assign to constant");

    add_compile_fail_case("return from defer expression", R"SOURCE(
pub fn testTrickyDefer() -> %void {
    defer canFail() %% {};

    defer %return canFail();

    const a = maybeInt() ?? return;
}

fn canFail() -> %void { }

pub fn maybeInt() -> ?i32 {
    return 0;
}

export fn entry() -> usize { @sizeOf(@typeOf(testTrickyDefer)) }
    )SOURCE", 1, ".tmp_source.zig:5:11: error: cannot return from defer expression");

    add_compile_fail_case("attempt to access var args out of bounds", R"SOURCE(
fn add(args: ...) -> i32 {
    args[0] + args[1]
}

fn foo() -> i32 {
    add(i32(1234))
}

export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 2,
            ".tmp_source.zig:3:19: error: index 1 outside argument list of size 1",
            ".tmp_source.zig:7:8: note: called from here");

    add_compile_fail_case("pass integer literal to var args", R"SOURCE(
fn add(args: ...) -> i32 {
    var sum = i32(0);
    {comptime var i: usize = 0; inline while (i < args.len; i += 1) {
        sum += args[i];
    }}
    return sum;
}

fn bar() -> i32 {
    add(1, 2, 3, 4)
}

export fn entry() -> usize { @sizeOf(@typeOf(bar)) }
    )SOURCE", 1, ".tmp_source.zig:11:9: error: parameter of type '(integer literal)' requires comptime");

    add_compile_fail_case("assign too big number to u16", R"SOURCE(
export fn foo() {
    var vga_mem: u16 = 0xB8000;
}
    )SOURCE", 1, ".tmp_source.zig:3:24: error: integer value 753664 cannot be implicitly casted to type 'u16'");

    add_compile_fail_case("set global variable alignment to non power of 2", R"SOURCE(
const some_data: [100]u8 = {
    @setGlobalAlign(some_data, 3);
    undefined
};
export fn entry() -> usize { @sizeOf(@typeOf(some_data)) }
    )SOURCE", 1, ".tmp_source.zig:3:32: error: alignment value must be power of 2");

    add_compile_fail_case("compile log", R"SOURCE(
export fn foo() {
    comptime bar(12, "hi");
}
fn bar(a: i32, b: []const u8) {
    @compileLog("begin");
    @compileLog("a", a, "b", b);
    @compileLog("end");
}
    )SOURCE", 6,
        ".tmp_source.zig:6:5: error: found compile log statement",
        ".tmp_source.zig:3:17: note: called from here",
        ".tmp_source.zig:7:5: error: found compile log statement",
        ".tmp_source.zig:3:17: note: called from here",
        ".tmp_source.zig:8:5: error: found compile log statement",
        ".tmp_source.zig:3:17: note: called from here");

    add_compile_fail_case("casting bit offset pointer to regular pointer", R"SOURCE(
const u2 = @IntType(false, 2);
const u3 = @IntType(false, 3);

const BitField = packed struct {
    a: u3,
    b: u3,
    c: u2,
};

fn foo(bit_field: &const BitField) -> u3 {
    return bar(&bit_field.b);
}

fn bar(x: &const u3) -> u3 {
    return *x;
}

export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:12:26: error: expected type '&const u3', found '&:3:6 const u3'");

    add_compile_fail_case("referring to a struct that is invalid", R"SOURCE(
const UsbDeviceRequest = struct {
    Type: u8,
};

export fn foo() {
    comptime assert(@sizeOf(UsbDeviceRequest) == 0x8);
}

fn assert(ok: bool) {
    if (!ok) unreachable;
}
    )SOURCE", 2,
            ".tmp_source.zig:11:14: error: unable to evaluate constant expression",
            ".tmp_source.zig:7:20: note: called from here");

    add_compile_fail_case("control flow uses comptime var at runtime", R"SOURCE(
export fn foo() {
    comptime var i = 0;
    while (i < 5; i += 1) {
        bar();
    }
}

fn bar() { }
    )SOURCE", 2,
            ".tmp_source.zig:4:5: error: control flow attempts to use compile-time variable at runtime",
            ".tmp_source.zig:4:21: note: compile-time variable assigned here");

    add_compile_fail_case("ignored return value", R"SOURCE(
export fn foo() {
    bar();
}
fn bar() -> i32 { 0 }
    )SOURCE", 1, ".tmp_source.zig:3:8: error: return value ignored");

    add_compile_fail_case("integer literal on a non-comptime var", R"SOURCE(
export fn foo() {
    var i = 0;
    while (i < 10; i += 1) { }
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: unable to infer variable type");

    add_compile_fail_case("undefined literal on a non-comptime var", R"SOURCE(
export fn foo() {
    var i = undefined;
    i = i32(1);
}
    )SOURCE", 1, ".tmp_source.zig:3:5: error: unable to infer variable type");

    add_compile_fail_case("dereference an array", R"SOURCE(
var s_buffer: [10]u8 = undefined;
pub fn pass(in: []u8) -> []u8 {
    var out = &s_buffer;
    *out[0] = in[0];
    return (*out)[0...1];
}

export fn entry() -> usize { @sizeOf(@typeOf(pass)) }
    )SOURCE", 1, ".tmp_source.zig:5:5: error: attempt to dereference non pointer type '[10]u8'");

    add_compile_fail_case("pass const ptr to mutable ptr fn", R"SOURCE(
fn foo() -> bool {
    const a = ([]const u8)("a");
    const b = &a;
    return ptrEql(b, b);
}
fn ptrEql(a: &[]const u8, b: &[]const u8) -> bool {
    return true;
}

export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:5:19: error: expected type '&[]const u8', found '&const []const u8'");

    {
        TestCase *tc = add_compile_fail_case("export collision", R"SOURCE(
const foo = @import("foo.zig");

export fn bar() -> usize {
    return foo.baz;
}
        )SOURCE", 2,
            "foo.zig:2:8: error: exported symbol collision: 'bar'",
            ".tmp_source.zig:4:8: note: other symbol is here");

        add_source_file(tc, "foo.zig", R"SOURCE(
export fn bar() {}
pub const baz = 1234;
        )SOURCE");
    }

    add_compile_fail_case("pass non-copyable type by value to function", R"SOURCE(
const Point = struct { x: i32, y: i32, };
fn foo(p: Point) { }
export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:3:11: error: type 'Point' is not copyable; cannot pass by value");

    add_compile_fail_case("implicit cast from array to mutable slice", R"SOURCE(
var global_array: [10]i32 = undefined;
fn foo(param: []i32) {}
export fn entry() {
    foo(global_array);
}
    )SOURCE", 1, ".tmp_source.zig:5:9: error: expected type '[]i32', found '[10]i32'");

    add_compile_fail_case("ptrcast to non-pointer", R"SOURCE(
export fn entry(a: &i32) -> usize {
    return @ptrcast(usize, a);
}
    )SOURCE", 1, ".tmp_source.zig:3:21: error: expected pointer, found 'usize'");

    add_compile_fail_case("too many error values to cast to small integer", R"SOURCE(
error A; error B; error C; error D; error E; error F; error G; error H;
const u2 = @IntType(false, 2);
fn foo(e: error) -> u2 {
    return u2(e);
}
export fn entry() -> usize { @sizeOf(@typeOf(foo)) }
    )SOURCE", 1, ".tmp_source.zig:5:14: error: too many error values to fit in 'u2'");

    add_compile_fail_case("asm at compile time", R"SOURCE(
comptime {
    doSomeAsm();
}

fn doSomeAsm() {
    asm volatile (
        \\.globl aoeu;
        \\.type aoeu, @function;
        \\.set aoeu, derp;
    );
}
    )SOURCE", 1, ".tmp_source.zig:7:5: error: unable to evaluate constant expression");

    add_compile_fail_case("invalid member of builtin enum", R"SOURCE(
export fn entry() {
    const foo = Arch.x86;
}
    )SOURCE", 1, ".tmp_source.zig:3:21: error: container 'Arch' has no member called 'x86'");

    add_compile_fail_case("int to ptr of 0 bits", R"SOURCE(
export fn foo() {
    var x: usize = 0x1000;
    var y: &void = @intToPtr(&void, x);
}
    )SOURCE", 1, ".tmp_source.zig:4:31: error: type '&void' has 0 bits and cannot store information");

    add_compile_fail_case("@fieldParentPtr - non struct", R"SOURCE(
const Foo = i32;
export fn foo(a: &i32) -> &Foo {
    return @fieldParentPtr(Foo, "a", a);
}
    )SOURCE", 1, ".tmp_source.zig:4:28: error: expected struct type, found 'i32'");

    add_compile_fail_case("@fieldParentPtr - bad field name", R"SOURCE(
const Foo = struct {
    derp: i32,
};
export fn foo(a: &i32) -> &Foo {
    return @fieldParentPtr(Foo, "a", a);
}
    )SOURCE", 1, ".tmp_source.zig:6:33: error: struct 'Foo' has no field 'a'");

    add_compile_fail_case("@fieldParentPtr - field pointer is not pointer", R"SOURCE(
const Foo = struct {
    a: i32,
};
export fn foo(a: i32) -> &Foo {
    return @fieldParentPtr(Foo, "a", a);
}
    )SOURCE", 1, ".tmp_source.zig:6:38: error: expected pointer, found 'i32'");

    add_compile_fail_case("@fieldParentPtr - comptime field ptr not based on struct", R"SOURCE(
const Foo = struct {
    a: i32,
    b: i32,
};
const foo = Foo { .a = 1, .b = 2, };

comptime {
    const field_ptr = @intToPtr(&i32, 0x1234);
    const another_foo_ptr = @fieldParentPtr(Foo, "b", field_ptr);
}
    )SOURCE", 1, ".tmp_source.zig:10:55: error: pointer value not based on parent struct");

    add_compile_fail_case("@fieldParentPtr - comptime wrong field index", R"SOURCE(
const Foo = struct {
    a: i32,
    b: i32,
};
const foo = Foo { .a = 1, .b = 2, };

comptime {
    const another_foo_ptr = @fieldParentPtr(Foo, "b", &foo.a);
}
    )SOURCE", 1, ".tmp_source.zig:9:29: error: field 'b' has index 1 but pointer value is index 0 of struct 'Foo'");
}

//////////////////////////////////////////////////////////////////////////////

static void add_parse_error_tests(void) {
    add_compile_fail_case("implicit semicolon - block statement", R"SOURCE(
export fn entry() {
    {}
    var good = {};
    ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - block expr", R"SOURCE(
export fn entry() {
    _ = {};
    var good = {};
    _ = {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - comptime statement", R"SOURCE(
export fn entry() {
    comptime {}
    var good = {};
    comptime ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - comptime expression", R"SOURCE(
export fn entry() {
    _ = comptime {};
    var good = {};
    _ = comptime {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - defer", R"SOURCE(
export fn entry() {
    defer {}
    var good = {};
    defer ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: expected token ';', found 'var'");

    add_compile_fail_case("implicit semicolon - if statement", R"SOURCE(
export fn entry() {
    if(true) {}
    var good = {};
    if(true) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if expression", R"SOURCE(
export fn entry() {
    _ = if(true) {};
    var good = {};
    _ = if(true) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else statement", R"SOURCE(
export fn entry() {
    if(true) {} else {}
    var good = {};
    if(true) ({}) else ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else expression", R"SOURCE(
export fn entry() {
    _ = if(true) {} else {};
    var good = {};
    _ = if(true) {} else {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else-if statement", R"SOURCE(
export fn entry() {
    if(true) {} else if(true) {}
    var good = {};
    if(true) ({}) else if(true) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else-if expression", R"SOURCE(
export fn entry() {
    _ = if(true) {} else if(true) {};
    var good = {};
    _ = if(true) {} else if(true) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else-if-else statement", R"SOURCE(
export fn entry() {
    if(true) {} else if(true) {} else {}
    var good = {};
    if(true) ({}) else if(true) ({}) else ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if-else-if-else expression", R"SOURCE(
export fn entry() {
    _ = if(true) {} else if(true) {} else {};
    var good = {};
    _ = if(true) {} else if(true) {} else {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var) statement", R"SOURCE(
export fn entry() {
    if(_=foo()) {}
    var good = {};
    if(_=foo()) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var) expression", R"SOURCE(
export fn entry() {
    _ = if(_=foo()) {};
    var good = {};
    _ = if(_=foo()) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else statement", R"SOURCE(
export fn entry() {
    if(_=foo()) {} else {}
    var good = {};
    if(_=foo()) ({}) else ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else expression", R"SOURCE(
export fn entry() {
    _ = if(_=foo()) {} else {};
    var good = {};
    _ = if(_=foo()) {} else {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else-if(var) statement", R"SOURCE(
export fn entry() {
    if(_=foo()) {} else if(_=foo()) {}
    var good = {};
    if(_=foo()) ({}) else if(_=foo()) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else-if(var) expression", R"SOURCE(
export fn entry() {
    _ = if(_=foo()) {} else if(_=foo()) {};
    var good = {};
    _ = if(_=foo()) {} else if(_=foo()) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else-if(var)-else statement", R"SOURCE(
export fn entry() {
    if(_=foo()) {} else if(_=foo()) {} else {}
    var good = {};
    if(_=foo()) ({}) else if(_=foo()) ({}) else ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - if(var)-else-if(var)-else expression", R"SOURCE(
export fn entry() {
    _ = if(_=foo()) {} else if(_=foo()) {} else {};
    var good = {};
    _ = if(_=foo()) {} else if(_=foo()) {} else {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - try statement", R"SOURCE(
export fn entry() {
    try (_ = foo()) {}
    var good = {};
    try (_ = foo()) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - try expression", R"SOURCE(
export fn entry() {
    _ = try (_ = foo()) {};
    var good = {};
    _ = try (_ = foo()) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - while statement", R"SOURCE(
export fn entry() {
    while(true) {}
    var good = {};
    while(true) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - while expression", R"SOURCE(
export fn entry() {
    _ = while(true) {};
    var good = {};
    _ = while(true) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - while-continue statement", R"SOURCE(
export fn entry() {
    while(true;{}) {}
    var good = {};
    while(true;{}) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - while-continue expression", R"SOURCE(
export fn entry() {
    _ = while(true;{}) {};
    var good = {};
    _ = while(true;{}) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - for statement", R"SOURCE(
export fn entry() {
    for(foo()) {}
    var good = {};
    for(foo()) ({})
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");

    add_compile_fail_case("implicit semicolon - for expression", R"SOURCE(
export fn entry() {
    _ = for(foo()) {};
    var good = {};
    _ = for(foo()) {}
    var bad = {};
}
    )SOURCE", 1, ".tmp_source.zig:6:5: error: invalid token: 'var'");
}

//////////////////////////////////////////////////////////////////////////////

static void add_debug_safety_test_cases(void) {
    add_debug_safety_case("calling panic", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
pub fn main() -> %void {
    @panic("oh no");
}
    )SOURCE");

    add_debug_safety_case("out of bounds slice access", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
pub fn main() -> %void {
    const a = []i32{1, 2, 3, 4};
    baz(bar(a));
}
fn bar(a: []const i32) -> i32 {
    a[4]
}
fn baz(a: i32) { }
    )SOURCE");

    add_debug_safety_case("integer addition overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = add(65530, 10);
    if (x == 0) return error.Whatever;
}
fn add(a: u16, b: u16) -> u16 {
    a + b
}
    )SOURCE");

    add_debug_safety_case("integer subtraction overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = sub(10, 20);
    if (x == 0) return error.Whatever;
}
fn sub(a: u16, b: u16) -> u16 {
    a - b
}
    )SOURCE");

    add_debug_safety_case("integer multiplication overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = mul(300, 6000);
    if (x == 0) return error.Whatever;
}
fn mul(a: u16, b: u16) -> u16 {
    a * b
}
    )SOURCE");

    add_debug_safety_case("integer negation overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = neg(-32768);
    if (x == 32767) return error.Whatever;
}
fn neg(a: i16) -> i16 {
    -a
}
    )SOURCE");

    add_debug_safety_case("signed integer division overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = div(-32768, -1);
    if (x == 32767) return error.Whatever;
}
fn div(a: i16, b: i16) -> i16 {
    a / b
}
    )SOURCE");

    add_debug_safety_case("signed shift left overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = shl(-16385, 1);
    if (x == 0) return error.Whatever;
}
fn shl(a: i16, b: i16) -> i16 {
    a << b
}
    )SOURCE");

    add_debug_safety_case("unsigned shift left overflow", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = shl(0b0010111111111111, 3);
    if (x == 0) return error.Whatever;
}
fn shl(a: u16, b: u16) -> u16 {
    a << b
}
    )SOURCE");

    add_debug_safety_case("integer division by zero", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = div0(999, 0);
}
fn div0(a: i32, b: i32) -> i32 {
    a / b
}
    )SOURCE");

    add_debug_safety_case("exact division failure", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = divExact(10, 3);
    if (x == 0) return error.Whatever;
}
fn divExact(a: i32, b: i32) -> i32 {
    @divExact(a, b)
}
    )SOURCE");

    add_debug_safety_case("cast []u8 to bigger slice of wrong size", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = widenSlice([]u8{1, 2, 3, 4, 5});
    if (x.len == 0) return error.Whatever;
}
fn widenSlice(slice: []const u8) -> []const i32 {
    ([]const i32)(slice)
}
    )SOURCE");

    add_debug_safety_case("value does not fit in shortening cast", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = shorten_cast(200);
    if (x == 0) return error.Whatever;
}
fn shorten_cast(x: i32) -> i8 {
    i8(x)
}
    )SOURCE");

    add_debug_safety_case("signed integer not fitting in cast to unsigned integer", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    const x = unsigned_cast(-10);
    if (x == 0) return error.Whatever;
}
fn unsigned_cast(x: i32) -> u32 {
    u32(x)
}
    )SOURCE");

    add_debug_safety_case("unwrap error", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
error Whatever;
pub fn main() -> %void {
    %%bar();
}
fn bar() -> %void {
    return error.Whatever;
}
    )SOURCE");

    add_debug_safety_case("cast integer to error and no code matches", R"SOURCE(
pub fn panic(message: []const u8) -> noreturn {
    @breakpoint();
    while (true) {}
}
pub fn main() -> %void {
    _ = bar(9999);
}
fn bar(x: u32) -> error {
    return error(x);
}
    )SOURCE");
}

//////////////////////////////////////////////////////////////////////////////

static void add_parseh_test_cases(void) {
    add_parseh_case("simple data types", AllowWarningsYes, R"SOURCE(
#include <stdint.h>
int foo(char a, unsigned char b, signed char c);
int foo(char a, unsigned char b, signed char c); // test a duplicate prototype
void bar(uint8_t a, uint16_t b, uint32_t c, uint64_t d);
void baz(int8_t a, int16_t b, int32_t c, int64_t d);
    )SOURCE", 3,
            "pub extern fn foo(a: u8, b: u8, c: i8) -> c_int;",
            "pub extern fn bar(a: u8, b: u16, c: u32, d: u64);",
            "pub extern fn baz(a: i8, b: i16, c: i32, d: i64);");

    add_parseh_case("noreturn attribute", AllowWarningsNo, R"SOURCE(
void foo(void) __attribute__((noreturn));
    )SOURCE", 1, R"OUTPUT(pub extern fn foo() -> noreturn;)OUTPUT");

    add_parseh_case("enums", AllowWarningsNo, R"SOURCE(
enum Foo {
    FooA,
    FooB,
    Foo1,
};
    )SOURCE", 5, R"(pub const enum_Foo = extern enum {
    A,
    B,
    @"1",
};)",
            R"(pub const FooA = 0;)",
            R"(pub const FooB = 1;)",
            R"(pub const Foo1 = 2;)",
            R"(pub const Foo = enum_Foo;)");

    add_parseh_case("restrict -> noalias", AllowWarningsNo, R"SOURCE(
void foo(void *restrict bar, void *restrict);
    )SOURCE", 1, R"OUTPUT(pub extern fn foo(noalias bar: ?&c_void, noalias arg1: ?&c_void);)OUTPUT");

    add_parseh_case("simple struct", AllowWarningsNo, R"SOURCE(
struct Foo {
    int x;
    char *y;
};
    )SOURCE", 2,
            R"OUTPUT(const struct_Foo = extern struct {
    x: c_int,
    y: ?&u8,
};)OUTPUT", R"OUTPUT(pub const Foo = struct_Foo;)OUTPUT");

    add_parseh_case("qualified struct and enum", AllowWarningsNo, R"SOURCE(
struct Foo {
    int x;
    int y;
};
enum Bar {
    BarA,
    BarB,
};
void func(struct Foo *a, enum Bar **b);
    )SOURCE", 7, R"OUTPUT(pub const struct_Foo = extern struct {
    x: c_int,
    y: c_int,
};)OUTPUT", R"OUTPUT(pub const enum_Bar = extern enum {
    A,
    B,
};)OUTPUT",
            R"OUTPUT(pub const BarA = 0;)OUTPUT",
            R"OUTPUT(pub const BarB = 1;)OUTPUT",
            "pub extern fn func(a: ?&struct_Foo, b: ?&?&enum_Bar);",
    R"OUTPUT(pub const Foo = struct_Foo;)OUTPUT",
    R"OUTPUT(pub const Bar = enum_Bar;)OUTPUT");

    add_parseh_case("constant size array", AllowWarningsNo, R"SOURCE(
void func(int array[20]);
    )SOURCE", 1, "pub extern fn func(array: ?&c_int);");


    add_parseh_case("self referential struct with function pointer",
        AllowWarningsNo, R"SOURCE(
struct Foo {
    void (*derp)(struct Foo *foo);
};
    )SOURCE", 2, R"OUTPUT(pub const struct_Foo = extern struct {
    derp: ?extern fn(?&struct_Foo),
};)OUTPUT", R"OUTPUT(pub const Foo = struct_Foo;)OUTPUT");


    add_parseh_case("struct prototype used in func", AllowWarningsNo, R"SOURCE(
struct Foo;
struct Foo *some_func(struct Foo *foo, int x);
    )SOURCE", 3, R"OUTPUT(pub const struct_Foo = @OpaqueType();)OUTPUT",
        R"OUTPUT(pub extern fn some_func(foo: ?&struct_Foo, x: c_int) -> ?&struct_Foo;)OUTPUT",
        R"OUTPUT(pub const Foo = struct_Foo;)OUTPUT");


    add_parseh_case("#define a char literal", AllowWarningsNo, R"SOURCE(
#define A_CHAR  'a'
    )SOURCE", 1, R"OUTPUT(pub const A_CHAR = 97;)OUTPUT");


    add_parseh_case("#define an unsigned integer literal", AllowWarningsNo,
        R"SOURCE(
#define CHANNEL_COUNT 24
    )SOURCE", 1, R"OUTPUT(pub const CHANNEL_COUNT = 24;)OUTPUT");


    add_parseh_case("#define referencing another #define", AllowWarningsNo,
        R"SOURCE(
#define THING2 THING1
#define THING1 1234
    )SOURCE", 2,
            "pub const THING1 = 1234;",
            "pub const THING2 = THING1;");


    add_parseh_case("variables", AllowWarningsNo, R"SOURCE(
extern int extern_var;
static const int int_var = 13;
    )SOURCE", 2,
            "pub extern var extern_var: c_int;",
            "pub const int_var: c_int = 13;");


    add_parseh_case("circular struct definitions", AllowWarningsNo, R"SOURCE(
struct Bar;

struct Foo {
    struct Bar *next;
};

struct Bar {
    struct Foo *next;
};
    )SOURCE", 2,
            R"SOURCE(pub const struct_Bar = extern struct {
    next: ?&struct_Foo,
};)SOURCE",
            R"SOURCE(pub const struct_Foo = extern struct {
    next: ?&struct_Bar,
};)SOURCE");


    add_parseh_case("typedef void", AllowWarningsNo, R"SOURCE(
typedef void Foo;
Foo fun(Foo *a);
    )SOURCE", 2,
            "pub const Foo = c_void;",
            "pub extern fn fun(a: ?&c_void);");

    add_parseh_case("generate inline func for #define global extern fn", AllowWarningsNo,
        R"SOURCE(
extern void (*fn_ptr)(void);
#define foo fn_ptr

extern char (*fn_ptr2)(int, float);
#define bar fn_ptr2
    )SOURCE", 4,
            "pub extern var fn_ptr: ?extern fn();",
            "pub fn foo();",
            "pub extern var fn_ptr2: ?extern fn(c_int, f32) -> u8;",
            "pub fn bar(arg0: c_int, arg1: f32) -> u8;");


    add_parseh_case("#define string", AllowWarningsNo, R"SOURCE(
#define  foo  "a string"
    )SOURCE", 1, "pub const foo: &const u8 = &(c str lit);");

    add_parseh_case("__cdecl doesn't mess up function pointers", AllowWarningsNo, R"SOURCE(
void foo(void (__cdecl *fn_ptr)(void));
    )SOURCE", 1, "pub extern fn foo(fn_ptr: ?extern fn());");

    add_parseh_case("comment after integer literal", AllowWarningsNo, R"SOURCE(
#define SDL_INIT_VIDEO 0x00000020  /**< SDL_INIT_VIDEO implies SDL_INIT_EVENTS */
    )SOURCE", 1, "pub const SDL_INIT_VIDEO = 32;");

    add_parseh_case("zig keywords in C code", AllowWarningsNo, R"SOURCE(
struct comptime {
    int defer;
};
    )SOURCE", 2, R"(pub const struct_comptime = extern struct {
    @"defer": c_int,
};)", R"(pub const @"comptime" = struct_comptime;)");

    add_parseh_case("macro defines string literal with octal", AllowWarningsNo, R"SOURCE(
#define FOO "aoeu\023 derp"
#define FOO2 "aoeu\0234 derp"
#define FOO_CHAR '\077'
    )SOURCE", 3,
            R"(pub const FOO: &const u8 = &(c str lit);)",
            R"(pub const FOO2: &const u8 = &(c str lit);)",
            R"(pub const FOO_CHAR = 63;)");
}

static void run_self_hosted_test(bool is_release_mode) {
    Buf self_hosted_tests_file = BUF_INIT;
    os_path_join(buf_create_from_str(ZIG_TEST_DIR),
        buf_create_from_str("self_hosted.zig"), &self_hosted_tests_file);

    Buf zig_stderr = BUF_INIT;
    Buf zig_stdout = BUF_INIT;
    ZigList<const char *> args = {0};
    args.append("test");
    args.append(buf_ptr(&self_hosted_tests_file));
    if (is_release_mode) {
        args.append("--release");
    }
    Termination term;
    os_exec_process(zig_exe, args, &term, &zig_stderr, &zig_stdout);

    if (term.how != TerminationIdClean || term.code != 0) {
        printf("\nSelf-hosted tests failed:\n");
        printf("./zig");
        for (size_t i = 0; i < args.length; i += 1) {
            printf(" %s", args.at(i));
        }
        printf("\n%s\n", buf_ptr(&zig_stderr));
        exit(1);
    }
}

static void run_std_lib_test(bool is_release_mode) {
    Buf std_index_file = BUF_INIT;
    os_path_join(buf_create_from_str(ZIG_STD_DIR),
        buf_create_from_str("index.zig"), &std_index_file);

    Buf zig_stderr = BUF_INIT;
    Buf zig_stdout = BUF_INIT;
    ZigList<const char *> args = {0};
    args.append("test");
    args.append(buf_ptr(&std_index_file));
    if (is_release_mode) {
        args.append("--release");
    }
    Termination term;
    os_exec_process(zig_exe, args, &term, &zig_stderr, &zig_stdout);

    if (term.how != TerminationIdClean || term.code != 0) {
        printf("\nstd lib tests failed:\n");
        printf("./zig");
        for (size_t i = 0; i < args.length; i += 1) {
            printf(" %s", args.at(i));
        }
        printf("\n%s\n", buf_ptr(&zig_stderr));
        exit(1);
    }
}


static void add_self_hosted_tests(void) {
    {
        TestCase *test_case = allocate<TestCase>(1);
        test_case->case_name = "self hosted tests (debug)";
        test_case->special = TestSpecialSelfHosted;
        test_case->is_release_mode = false;
        test_cases.append(test_case);
    }
    {
        TestCase *test_case = allocate<TestCase>(1);
        test_case->case_name = "self hosted tests (release)";
        test_case->special = TestSpecialSelfHosted;
        test_case->is_release_mode = true;
        test_cases.append(test_case);
    }
}

static void add_std_lib_tests(void) {
    {
        TestCase *test_case = allocate<TestCase>(1);
        test_case->case_name = "std (debug)";
        test_case->special = TestSpecialStd;
        test_case->is_release_mode = false;
        test_cases.append(test_case);
    }
    {
        TestCase *test_case = allocate<TestCase>(1);
        test_case->case_name = "std (release)";
        test_case->special = TestSpecialStd;
        test_case->is_release_mode = true;
        test_cases.append(test_case);
    }
}

static void add_asm_tests(void) {
#if defined(ZIG_OS_LINUX) && defined(ZIG_ARCH_X86_64)
    add_asm_case("assemble and link hello world linux x86_64", R"SOURCE(
.text
.globl _start

_start:
    mov rax, 1
    mov rdi, 1
    lea rsi, msg
    mov rdx, 14
    syscall

    mov rax, 60
    mov rdi, 0
    syscall

.data

msg:
    .ascii "Hello, world!\n"
    )SOURCE", "Hello, world!\n");

#endif
}


static void print_compiler_invocation(TestCase *test_case) {
    printf("%s", zig_exe);
    for (size_t i = 0; i < test_case->compiler_args.length; i += 1) {
        printf(" %s", test_case->compiler_args.at(i));
    }
    printf("\n");
}

static void print_linker_invocation(TestCase *test_case) {
    printf("%s", zig_exe);
    for (size_t i = 0; i < test_case->linker_args.length; i += 1) {
        printf(" %s", test_case->linker_args.at(i));
    }
    printf("\n");
}


static void print_exe_invocation(TestCase *test_case) {
    printf("%s", tmp_exe_path);
    for (size_t i = 0; i < test_case->program_args.length; i += 1) {
        printf(" %s", test_case->program_args.at(i));
    }
    printf("\n");
}

static void run_test(TestCase *test_case) {
    if (test_case->special == TestSpecialSelfHosted) {
        return run_self_hosted_test(test_case->is_release_mode);
    } else if (test_case->special == TestSpecialStd) {
        return run_std_lib_test(test_case->is_release_mode);
    }

    for (size_t i = 0; i < test_case->source_files.length; i += 1) {
        TestSourceFile *test_source = &test_case->source_files.at(i);
        os_write_file(
                buf_create_from_str(test_source->relative_path),
                buf_create_from_str(test_source->source_code));
    }

    Buf zig_stderr = BUF_INIT;
    Buf zig_stdout = BUF_INIT;
    int err;
    Termination term;
    if ((err = os_exec_process(zig_exe, test_case->compiler_args, &term, &zig_stderr, &zig_stdout))) {
        fprintf(stderr, "Unable to exec %s: %s\n", zig_exe, err_str(err));
    }

    if (!test_case->is_parseh && test_case->compile_errors.length) {
        if (term.how != TerminationIdClean || term.code != 0) {
            for (size_t i = 0; i < test_case->compile_errors.length; i += 1) {
                const char *err_text = test_case->compile_errors.at(i);
                if (!strstr(buf_ptr(&zig_stderr), err_text)) {
                    printf("\n");
                    printf("========= Expected this compile error: =========\n");
                    printf("%s\n", err_text);
                    printf("================================================\n");
                    print_compiler_invocation(test_case);
                    printf("%s\n", buf_ptr(&zig_stderr));
                    exit(1);
                }
            }
            return; // success
        } else {
            printf("\nCompile failed with return code 0 (Expected failure):\n");
            print_compiler_invocation(test_case);
            printf("%s\n", buf_ptr(&zig_stderr));
            exit(1);
        }
    }

    if (term.how != TerminationIdClean || term.code != 0) {
        printf("\nCompile failed:\n");
        print_compiler_invocation(test_case);
        printf("%s\n", buf_ptr(&zig_stderr));
        exit(1);
    }

    if (test_case->is_parseh) {
        if (buf_len(&zig_stderr) > 0) {
            printf("\nparseh emitted warnings:\n");
            printf("------------------------------\n");
            print_compiler_invocation(test_case);
            printf("%s\n", buf_ptr(&zig_stderr));
            printf("------------------------------\n");
            if (test_case->allow_warnings == AllowWarningsNo) {
                exit(1);
            }
        }

        for (size_t i = 0; i < test_case->compile_errors.length; i += 1) {
            const char *output = test_case->compile_errors.at(i);

            if (!strstr(buf_ptr(&zig_stdout), output)) {
                printf("\n");
                printf("========= Expected this output: =========\n");
                printf("%s\n", output);
                printf("================================================\n");
                print_compiler_invocation(test_case);
                printf("%s\n", buf_ptr(&zig_stdout));
                exit(1);
            }
        }
    } else {
        if (test_case->special == TestSpecialLinkStep) {
            Buf link_stderr = BUF_INIT;
            Buf link_stdout = BUF_INIT;
            int err;
            Termination term;
            if ((err = os_exec_process(zig_exe, test_case->linker_args, &term, &link_stderr, &link_stdout))) {
                fprintf(stderr, "Unable to exec %s: %s\n", zig_exe, err_str(err));
            }

            if (term.how != TerminationIdClean || term.code != 0) {
                printf("\nLink failed:\n");
                print_linker_invocation(test_case);
                printf("%s\n", buf_ptr(&zig_stderr));
                exit(1);
            }
        }

        Buf program_stderr = BUF_INIT;
        Buf program_stdout = BUF_INIT;
        os_exec_process(tmp_exe_path, test_case->program_args, &term, &program_stderr, &program_stdout);

        if (test_case->is_debug_safety) {
            int debug_trap_signal = 5;
            if (term.how != TerminationIdSignaled || term.code != debug_trap_signal) {
                if (term.how == TerminationIdClean) {
                    printf("\nProgram expected to hit debug trap (signal %d) but exited with return code %d\n",
                            debug_trap_signal, term.code);
                } else if (term.how == TerminationIdSignaled) {
                    printf("\nProgram expected to hit debug trap (signal %d) but signaled with code %d\n",
                            debug_trap_signal, term.code);
                } else {
                    printf("\nProgram expected to hit debug trap (signal %d) exited in an unexpected way\n",
                            debug_trap_signal);
                }
                print_compiler_invocation(test_case);
                print_exe_invocation(test_case);
                exit(1);
            }
        } else {
            if (term.how != TerminationIdClean || term.code != 0) {
                printf("\nProgram exited with error\n");
                print_compiler_invocation(test_case);
                print_exe_invocation(test_case);
                printf("%s\n", buf_ptr(&program_stderr));
                exit(1);
            }

            if (test_case->output != nullptr && !buf_eql_str(&program_stdout, test_case->output)) {
                printf("\n");
                print_compiler_invocation(test_case);
                print_exe_invocation(test_case);
                printf("==== Test failed. Expected output: ====\n");
                printf("%s\n", test_case->output);
                printf("========= Actual output: ==============\n");
                printf("%s\n", buf_ptr(&program_stdout));
                printf("=======================================\n");
                exit(1);
            }
        }
    }

    for (size_t i = 0; i < test_case->source_files.length; i += 1) {
        TestSourceFile *test_source = &test_case->source_files.at(i);
        remove(test_source->relative_path);
    }
}

static void run_all_tests(const char *grep_text) {
    for (size_t i = 0; i < test_cases.length; i += 1) {
        TestCase *test_case = test_cases.at(i);
        if (grep_text != nullptr && strstr(test_case->case_name, grep_text) == nullptr) {
            continue;
        }

        printf("Test %zu/%zu %s...", i + 1, test_cases.length, test_case->case_name);
        fflush(stdout);
        run_test(test_case);
        printf("OK\n");
    }
    printf("%zu tests passed.\n", test_cases.length);
}

static void cleanup(void) {
    remove(tmp_source_path);
    remove(tmp_h_path);
    remove(tmp_exe_path);
}

static int usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [--grep text]\n", arg0);
    return 1;
}

int main(int argc, char **argv) {
    const char *grep_text = nullptr;
    for (int i = 1; i < argc; i += 1) {
        const char *arg = argv[i];
        if (i + 1 >= argc) {
            return usage(argv[0]);
        } else {
            i += 1;
            if (strcmp(arg, "--grep") == 0) {
                grep_text = argv[i];
            } else {
                return usage(argv[0]);
            }
        }
    }
    add_compiling_test_cases();
    add_build_examples();
    add_debug_safety_test_cases();
    add_compile_failure_test_cases();
    add_parse_error_tests();
    add_parseh_test_cases();
    add_self_hosted_tests();
    add_std_lib_tests();
    add_asm_tests();
    run_all_tests(grep_text);
    cleanup();
}
