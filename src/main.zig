const c = @import("c.zig").c;
const codegen = @import("codegen.zig");
const errmsg = @import("errmsg.zig");

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

enum Cmd {
    Build,
    Test,
    Version,
    ParseH,
    Targets,
}

export fn main(argc: c_int, argv: &&u8) -> c_int {
    const arg = argv[0];
    var cmd: ?Cmd = null;
    var in_file: ?&u8 = null;
    var out_file: ?&u8 = null;
    var is_release_build = false;
    var strip = false;
    var is_static = false;
    var out_type: ?codegen.OutType = null;
    var out_name: ?&u8 = null;
    var verbose = false;
    var color = errmsg.ErrColor.Auto;
    var libc_lib_dir: ?&u8 = null;
    var libc_static_lib_dir: ?&u8 = null;
    var libc_include_dir: ?&u8 = null;
    var dynamic_linker: ?&u8 = null;
    var linker_path: ?&u8 = null;
    ZigList<const char *> clang_argv = {0};
    ZigList<const char *> lib_dirs = {0};
    ZigList<const char *> link_libs = {0};
    var target_arch: ?&u8 = null;
    var target_os: ?&u8 = null;
    var target_environ: ?&u8 = null;
    var mwindows = false;
    var mconsole = false;
    var municode = false;
    var mlinker_version: ?&u8 = null;
    var rdynamic = false;
    var mmacosx_version_min: ?&u8 = null;
    var mios_version_min: ?&u8 = null;
    var check_unused = false;

    {var i: i32 = 1; while (i < argc; i += 1) {
        const arg = argv[i];

        if (arg[0] == '-') {
            if (c.strcmp(arg, "--release") == 0) {
                is_release_build = true;
            } else if (c.strcmp(arg, "--strip") == 0) {
                strip = true;
            } else if (c.strcmp(arg, "--static") == 0) {
                is_static = true;
            } else if (c.strcmp(arg, "--verbose") == 0) {
                verbose = true;
            } else if (c.strcmp(arg, "-mwindows") == 0) {
                mwindows = true;
            } else if (c.strcmp(arg, "-mconsole") == 0) {
                mconsole = true;
            } else if (c.strcmp(arg, "-municode") == 0) {
                municode = true;
            } else if (c.strcmp(arg, "-rdynamic") == 0) {
                rdynamic = true;
            } else if (c.strcmp(arg, "--check-unused") == 0) {
                check_unused = true;
            } else if (i + 1 >= argc) {
                return usage(arg0);
            } else {
                i += 1;
                if (i >= argc) {
                    return usage(arg0);
                } else if (c.strcmp(arg, "--output") == 0) {
                    out_file = argv[i];
                } else if (c.strcmp(arg, "--export") == 0) {
                    if (c.strcmp(argv[i], "exe") == 0) {
                        out_type = codegen.OutType.Exe;
                    } else if (c.strcmp(argv[i], "lib") == 0) {
                        out_type = codegen.OutType.Lib;
                    } else if (c.strcmp(argv[i], "obj") == 0) {
                        out_type = codegen.OutType.Obj;
                    } else {
                        return usage(arg0);
                    }
                } else if (c.strcmp(arg, "--color") == 0) {
                    if (c.strcmp(argv[i], "auto") == 0) {
                        color = errmsg.ErrColor.Auto;
                    } else if (c.strcmp(argv[i], "on") == 0) {
                        color = errmsg.ErrColor.On;
                    } else if (c.strcmp(argv[i], "off") == 0) {
                        color = errmsg.ErrColor.Off;
                    } else {
                        return usage(arg0);
                    }
                } else if (c.strcmp(arg, "--name") == 0) {
                    out_name = argv[i];
                } else if (c.strcmp(arg, "--libc-lib-dir") == 0) {
                    libc_lib_dir = argv[i];
                } else if (c.strcmp(arg, "--libc-static-lib-dir") == 0) {
                    libc_static_lib_dir = argv[i];
                } else if (c.strcmp(arg, "--libc-include-dir") == 0) {
                    libc_include_dir = argv[i];
                } else if (c.strcmp(arg, "--dynamic-linker") == 0) {
                    dynamic_linker = argv[i];
                } else if (c.strcmp(arg, "--ld-path") == 0) {
                    linker_path = argv[i];
                } else if (c.strcmp(arg, "-isystem") == 0) {
                    clang_argv.append("-isystem");
                    clang_argv.append(argv[i]);
                } else if (c.strcmp(arg, "-dirafter") == 0) {
                    clang_argv.append("-dirafter");
                    clang_argv.append(argv[i]);
                } else if (c.strcmp(arg, "--library-path") == 0) {
                    lib_dirs.append(argv[i]);
                } else if (c.strcmp(arg, "--library") == 0) {
                    link_libs.append(argv[i]);
                } else if (c.strcmp(arg, "--target-arch") == 0) {
                    target_arch = argv[i];
                } else if (c.strcmp(arg, "--target-os") == 0) {
                    target_os = argv[i];
                } else if (c.strcmp(arg, "--target-environ") == 0) {
                    target_environ = argv[i];
                } else if (c.strcmp(arg, "-mlinker-version") == 0) {
                    mlinker_version = argv[i];
                } else if (c.strcmp(arg, "-mmacosx-version-min") == 0) {
                    mmacosx_version_min = argv[i];
                } else if (c.strcmp(arg, "-mios-version-min") == 0) {
                    mios_version_min = argv[i];
                } else {
                    c.fprintf(c.stderr, "Invalid argument: %s\n", arg);
                    return usage(arg0);
                }
            }
        } else if (cmd == CmdInvalid) {
            if (c.strcmp(arg, "build") == 0) {
                cmd = Cmd.Build;
            } else if (c.strcmp(arg, "version") == 0) {
                cmd = Cmd.Version;
            } else if (c.strcmp(arg, "parseh") == 0) {
                cmd = Cmd.ParseH;
            } else if (c.strcmp(arg, "test") == 0) {
                cmd = Cmd.Test;
            } else if (c.strcmp(arg, "targets") == 0) {
                cmd = Cmd.Targets;
            } else {
                c.fprintf(c.stderr, "Unrecognized command: %s\n", arg);
                return usage(arg0);
            }
        } else {
            switch (cmd) {
                Build, ParseH, Test => {
                    if (!in_file) {
                        in_file = arg;
                    } else {
                        return usage(arg0);
                    }
                },
                Version, Targets => {
                    return usage(arg0);
                },
            }
        }
    }}

    switch (cmd) {
        Build, ParseH, Test => {
            if (!in_file)
                return usage(arg0);

            if (cmd == CmdBuild && !out_name) {
                c.fprintf(c.stderr, "--name [name] not provided\n\n");
                return usage(arg0);
            }

            if (cmd == CmdBuild && out_type == OutTypeUnknown) {
                c.fprintf(c.stderr, "--export [exe|lib|obj] not provided\n\n");
                return usage(arg0);
            }

            init_all_targets();

            ZigTarget alloc_target;
            ZigTarget *target;
            if (!target_arch && !target_os && !target_environ) {
                target = nullptr;
            } else {
                target = &alloc_target;
                get_unknown_target(target);
                if (target_arch) {
                    if (parse_target_arch(target_arch, &target->arch)) {
                        c.fprintf(c.stderr, "invalid --target-arch argument\n");
                        return usage(arg0);
                    }
                }
                if (target_os) {
                    if (parse_target_os(target_os, &target->os)) {
                        c.fprintf(c.stderr, "invalid --target-os argument\n");
                        return usage(arg0);
                    }
                }
                if (target_environ) {
                    if (parse_target_environ(target_environ, &target->env_type)) {
                        c.fprintf(c.stderr, "invalid --target-environ argument\n");
                        return usage(arg0);
                    }
                }
            }


            Buf in_file_buf = BUF_INIT;
            buf_init_from_str(&in_file_buf, in_file);

            Buf root_source_dir = BUF_INIT;
            Buf root_source_code = BUF_INIT;
            Buf root_source_name = BUF_INIT;
            if (buf_eql_str(&in_file_buf, "-")) {
                os_get_cwd(&root_source_dir);
                if ((err = os_fetch_file(stdin, &root_source_code))) {
                    c.fprintf(c.stderr, "unable to read stdin: %s\n", err_str(err));
                    return 1;
                }
                buf_init_from_str(&root_source_name, "");
            } else {
                os_path_split(&in_file_buf, &root_source_dir, &root_source_name);
                if ((err = os_fetch_file_path(buf_create_from_str(in_file), &root_source_code))) {
                    c.fprintf(c.stderr, "unable to open '%s': %s\n", in_file, err_str(err));
                    return 1;
                }
            }

            CodeGen *g = codegen_create(&root_source_dir, target);
            codegen_set_is_release(g, is_release_build);
            codegen_set_is_test(g, cmd == CmdTest);

            codegen_set_check_unused(g, check_unused);

            codegen_set_clang_argv(g, clang_argv.items, clang_argv.length);
            codegen_set_strip(g, strip);
            codegen_set_is_static(g, is_static);
            if (out_type != OutTypeUnknown) {
                codegen_set_out_type(g, out_type);
            } else if (cmd == CmdTest) {
                codegen_set_out_type(g, OutTypeExe);
            }
            if (out_name) {
                codegen_set_out_name(g, buf_create_from_str(out_name));
            } else if (cmd == CmdTest) {
                codegen_set_out_name(g, buf_create_from_str("test"));
            }
            if (libc_lib_dir)
                codegen_set_libc_lib_dir(g, buf_create_from_str(libc_lib_dir));
            if (libc_static_lib_dir)
                codegen_set_libc_static_lib_dir(g, buf_create_from_str(libc_static_lib_dir));
            if (libc_include_dir)
                codegen_set_libc_include_dir(g, buf_create_from_str(libc_include_dir));
            if (dynamic_linker)
                codegen_set_dynamic_linker(g, buf_create_from_str(dynamic_linker));
            if (linker_path)
                codegen_set_linker_path(g, buf_create_from_str(linker_path));
            codegen_set_verbose(g, verbose);
            codegen_set_errmsg_color(g, color);

            for (int i = 0; i < lib_dirs.length; i += 1) {
                codegen_add_lib_dir(g, lib_dirs.at(i));
            }
            for (int i = 0; i < link_libs.length; i += 1) {
                codegen_add_link_lib(g, link_libs.at(i));
            }

            codegen_set_windows_subsystem(g, mwindows, mconsole);
            codegen_set_windows_unicode(g, municode);
            codegen_set_rdynamic(g, rdynamic);
            if (mlinker_version) {
                codegen_set_mlinker_version(g, buf_create_from_str(mlinker_version));
            }
            if (mmacosx_version_min && mios_version_min) {
                c.fprintf(c.stderr, "-mmacosx-version-min and -mios-version-min options not allowed together\n");
                return EXIT_FAILURE;
            }
            if (mmacosx_version_min) {
                codegen_set_mmacosx_version_min(g, buf_create_from_str(mmacosx_version_min));
            }
            if (mios_version_min) {
                codegen_set_mios_version_min(g, buf_create_from_str(mios_version_min));
            }

            if (cmd == CmdBuild) {
                codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_link(g, out_file);
                return EXIT_SUCCESS;
            } else if (cmd == CmdParseH) {
                codegen_parseh(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_render_ast(g, stdout, 4);
                return EXIT_SUCCESS;
            } else if (cmd == CmdTest) {
                codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_link(g, "./test");
                ZigList<const char *> args = {0};
                int return_code;
                os_spawn_process("./test", args, &return_code);
                if (return_code != 0) {
                    c.fprintf(c.stderr, "\nTests failed. Use the following command to reproduce the failure:\n");
                    c.fprintf(c.stderr, "./test\n");
                }
                return return_code;
            } else {
                zig_unreachable();
            }
        },
        Version => {
            printf("%s\n", ZIG_VERSION_STRING);
            return EXIT_SUCCESS;
        },
        Targets => {
            return print_target_list(stdout);
        },
    }
}

static int print_target_list(FILE *f) {
    ZigTarget native;
    get_native_target(&native);

    c.fprintf(f, "Architectures:\n");
    int arch_count = target_arch_count();
    for (int arch_i = 0; arch_i < arch_count; arch_i += 1) {
        const ArchType *arch = get_target_arch(arch_i);
        char arch_name[50];
        get_arch_name(arch_name, arch);
        const char *native_str = (native.arch.arch == arch->arch && native.arch.sub_arch == arch->sub_arch) ?
            " (native)" : "";
        c.fprintf(f, "  %s%s\n", arch_name, native_str);
    }

    c.fprintf(f, "\nOperating Systems:\n");
    int os_count = target_os_count();
    for (int i = 0; i < os_count; i += 1) {
        ZigLLVM_OSType os_type = get_target_os(i);
        const char *native_str = (native.os == os_type) ? " (native)" : "";
        c.fprintf(f, "  %s%s\n", get_target_os_name(os_type), native_str);
    }

    c.fprintf(f, "\nEnvironments:\n");
    int environ_count = target_environ_count();
    for (int i = 0; i < environ_count; i += 1) {
        ZigLLVM_EnvironmentType environ_type = get_target_environ(i);
        const char *native_str = (native.env_type == environ_type) ? " (native)" : "";
        c.fprintf(f, "  %s%s\n", ZigLLVMGetEnvironmentTypeName(environ_type), native_str);
    }

    return EXIT_SUCCESS;
}

