const c = @import("c.zig");

fn usage(arg0: &const u8) -> c_int {
    c.fprintf(c.stderr, c"Usage: %s [command] [options]\n" ++
        c"Commands:\n" ++
        c"  build [sources]              create executable, object, or library from source\n" ++
        c"  test [sources]               create and run a test build\n" ++
        c"  parseh [source]              convert a c header file to zig extern declarations\n" ++
        c"  version                      print version number and exit\n" ++
        c"  targets                      list available compilation targets\n" ++
        c"Options:\n" ++
        c"  --release                    build with optimizations on and debug protection off\n" ++
        c"  --static                     output will be statically linked\n" ++
        c"  --strip                      exclude debug symbols\n" ++
        c"  --export [exe|lib|obj]       override output type\n" ++
        c"  --name [name]                override output name\n" ++
        c"  --output [file]              override destination path\n" ++
        c"  --verbose                    turn on compiler debug output\n" ++
        c"  --color [auto|off|on]        enable or disable colored error messages\n" ++
        c"  --libc-lib-dir [path]        directory where libc crt1.o resides\n" ++
        c"  --libc-static-lib-dir [path] directory where libc crtbegin.o resides\n" ++
        c"  --libc-include-dir [path]    directory where libc stdlib.h resides\n" ++
        c"  --dynamic-linker [path]      set the path to ld.so\n" ++
        c"  --ld-path [path]             set the path to the linker\n" ++
        c"  -isystem [dir]               add additional search path for other .h files\n" ++
        c"  -dirafter [dir]              same as -isystem but do it last\n" ++
        c"  --library-path [dir]         add a directory to the library search path\n" ++
        c"  --library [lib]              link against lib\n" ++
        c"  --target-arch [name]         specify target architecture\n" ++
        c"  --target-os [name]           specify target operating system\n" ++
        c"  --target-environ [name]      specify target environment\n" ++
        c"  -mwindows                    (windows only) --subsystem windows to the linker\n" ++
        c"  -mconsole                    (windows only) --subsystem console to the linker\n" ++
        c"  -municode                    (windows only) link with unicode\n" ++
        c"  -mlinker-version [ver]       (darwin only) override linker version\n" ++
        c"  -rdynamic                    add all symbols to the dynamic symbol table\n" ++
        c"  -mmacosx-version-min [ver]   (darwin only) set Mac OS X deployment target\n" ++
        c"  -mios-version-min [ver]      (darwin only) set iOS deployment target\n" ++
        c"  --check-unused               perform semantic analysis on unused declarations\n"
    , arg0);
    return -1;
}

export fn main(argc: c_int, argv: &&u8) -> c_int {
    return usage(argv[0]);
}
