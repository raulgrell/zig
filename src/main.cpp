/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "ast_render.hpp"
#include "buffer.hpp"
#include "codegen.hpp"
#include "config.h"
#include "error.hpp"
#include "link.hpp"
#include "os.hpp"
#include "target.hpp"

#include <stdio.h>

static int usage(const char *arg0) {
    fprintf(stderr, "Usage: %s [command] [options]\n"
        "Commands:\n"
        "  asm [source]                 create object from assembly\n"
        "  build                        build project from build.zig\n"
        "  build_exe [source]           create executable from source\n"
        "  build_lib [source]           create library from source\n"
        "  build_obj [source]           create object from source\n"
        "  link_exe [objects]           create executable from objects\n"
        "  link_lib [objects]           create library from objects\n"
        "  parseh [source]              convert a c header file to zig extern declarations\n"
        "  targets                      list available compilation targets\n"
        "  test [source]                create and run a test build\n"
        "  version                      print version number and exit\n"
        "Options:\n"
        "  --ar-path [path]             set the path to ar\n"
        "  --color [auto|off|on]        enable or disable colored error messages\n"
        "  --dynamic-linker [path]      set the path to ld.so\n"
        "  --each-lib-rpath             add rpath for each used dynamic library\n"
        "  --ld-path [path]             set the path to the linker\n"
        "  --libc-include-dir [path]    directory where libc stdlib.h resides\n"
        "  --libc-lib-dir [path]        directory where libc crt1.o resides\n"
        "  --libc-static-lib-dir [path] directory where libc crtbegin.o resides\n"
        "  --library [lib]              link against lib\n"
        "  --library-path [dir]         add a directory to the library search path\n"
        "  --linker-script [path]       use a custom linker script\n"
        "  --name [name]                override output name\n"
        "  --output [file]              override destination path\n"
        "  --release                    build with optimizations on and debug protection off\n"
        "  --static                     output will be statically linked\n"
        "  --strip                      exclude debug symbols\n"
        "  --target-arch [name]         specify target architecture\n"
        "  --target-environ [name]      specify target environment\n"
        "  --target-os [name]           specify target operating system\n"
        "  --verbose                    turn on compiler debug output\n"
        "  --zig-std-dir [path]         directory where zig standard library resides\n"
        "  -L[dir]                      alias for --library-path\n"
        "  -dirafter [dir]              same as -isystem but do it last\n"
        "  -framework [name]            (darwin only) link against framework\n"
        "  -isystem [dir]               add additional search path for other .h files\n"
        "  -mconsole                    (windows only) --subsystem console to the linker\n"
        "  -mios-version-min [ver]      (darwin only) set iOS deployment target\n"
        "  -mlinker-version [ver]       (darwin only) override linker version\n"
        "  -mmacosx-version-min [ver]   (darwin only) set Mac OS X deployment target\n"
        "  -municode                    (windows only) link with unicode\n"
        "  -mwindows                    (windows only) --subsystem windows to the linker\n"
        "  -rdynamic                    add all symbols to the dynamic symbol table\n"
        "  -rpath [path]                add directory to the runtime library search path\n"
        "Test Options:\n"
        "  --test-filter [text]         skip tests that do not match filter\n"
        "  --test-name-prefix [text]    add prefix to all tests\n"
    , arg0);
    return EXIT_FAILURE;
}

static int print_target_list(FILE *f) {
    ZigTarget native;
    get_native_target(&native);

    fprintf(f, "Architectures:\n");
    size_t arch_count = target_arch_count();
    for (size_t arch_i = 0; arch_i < arch_count; arch_i += 1) {
        const ArchType *arch = get_target_arch(arch_i);
        char arch_name[50];
        get_arch_name(arch_name, arch);
        const char *native_str = (native.arch.arch == arch->arch && native.arch.sub_arch == arch->sub_arch) ?
            " (native)" : "";
        fprintf(f, "  %s%s\n", arch_name, native_str);
    }

    fprintf(f, "\nOperating Systems:\n");
    size_t os_count = target_os_count();
    for (size_t i = 0; i < os_count; i += 1) {
        ZigLLVM_OSType os_type = get_target_os(i);
        const char *native_str = (native.os == os_type) ? " (native)" : "";
        fprintf(f, "  %s%s\n", get_target_os_name(os_type), native_str);
    }

    fprintf(f, "\nEnvironments:\n");
    size_t environ_count = target_environ_count();
    for (size_t i = 0; i < environ_count; i += 1) {
        ZigLLVM_EnvironmentType environ_type = get_target_environ(i);
        const char *native_str = (native.env_type == environ_type) ? " (native)" : "";
        fprintf(f, "  %s%s\n", ZigLLVMGetEnvironmentTypeName(environ_type), native_str);
    }

    return EXIT_SUCCESS;
}

enum Cmd {
    CmdInvalid,
    CmdBuild,
    CmdTest,
    CmdVersion,
    CmdParseH,
    CmdTargets,
    CmdAsm,
    CmdLink,
};

int main(int argc, char **argv) {
    os_init();

    char *arg0 = argv[0];
    Cmd cmd = CmdInvalid;
    const char *in_file = nullptr;
    const char *out_file = nullptr;
    bool is_release_build = false;
    bool strip = false;
    bool is_static = false;
    OutType out_type = OutTypeUnknown;
    const char *out_name = nullptr;
    bool verbose = false;
    ErrColor color = ErrColorAuto;
    const char *libc_lib_dir = nullptr;
    const char *libc_static_lib_dir = nullptr;
    const char *libc_include_dir = nullptr;
    const char *zig_std_dir = nullptr;
    const char *dynamic_linker = nullptr;
    ZigList<const char *> clang_argv = {0};
    ZigList<const char *> lib_dirs = {0};
    ZigList<const char *> link_libs = {0};
    ZigList<const char *> frameworks = {0};
    int err;
    const char *target_arch = nullptr;
    const char *target_os = nullptr;
    const char *target_environ = nullptr;
    bool mwindows = false;
    bool mconsole = false;
    bool municode = false;
    const char *mlinker_version = nullptr;
    bool rdynamic = false;
    const char *mmacosx_version_min = nullptr;
    const char *mios_version_min = nullptr;
    const char *linker_script = nullptr;
    ZigList<const char *> rpath_list = {0};
    bool each_lib_rpath = false;
    ZigList<const char *> objects = {0};
    const char *test_filter = nullptr;
    const char *test_name_prefix = nullptr;

    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        const char *zig_exe_path = arg0;
        const char *build_file = "build.zig";
        bool asked_for_help = false;

        init_all_targets();

        Buf *zig_std_dir = buf_create_from_str(ZIG_STD_DIR);
        Buf *special_dir = buf_alloc();
        os_path_join(zig_std_dir, buf_sprintf("special"), special_dir);

        Buf *build_runner_path = buf_alloc();
        os_path_join(special_dir, buf_create_from_str("build_runner.zig"), build_runner_path);

        ZigList<const char *> args = {0};
        args.append(zig_exe_path);
        args.append(NULL); // placeholder
        for (int i = 2; i < argc; i += 1) {
            if (strcmp(argv[i], "--debug-build-verbose") == 0) {
                verbose = true;
            } else if (strcmp(argv[i], "--help") == 0) {
                asked_for_help = true;
                args.append(argv[i]);
            } else if (i + 1 < argc && strcmp(argv[i], "--build-file") == 0) {
                build_file = argv[i + 1];
                i += 1;
            } else {
                args.append(argv[i]);
            }
        }


        Buf root_source_dir = BUF_INIT;
        Buf root_source_code = BUF_INIT;
        Buf root_source_name = BUF_INIT;
        os_path_split(build_runner_path, &root_source_dir, &root_source_name);
        if ((err = os_fetch_file_path(build_runner_path, &root_source_code))) {
            fprintf(stderr, "unable to open '%s': %s\n", buf_ptr(build_runner_path), err_str(err));
            return 1;
        }
        CodeGen *g = codegen_create(&root_source_dir, nullptr);
        codegen_set_out_name(g, buf_create_from_str("build"));
        codegen_set_out_type(g, OutTypeExe);
        codegen_set_verbose(g, verbose);

        Buf build_file_abs = BUF_INIT;
        os_path_resolve(buf_create_from_str("."), buf_create_from_str(build_file), &build_file_abs);
        Buf build_file_basename = BUF_INIT;
        Buf build_file_dirname = BUF_INIT;
        os_path_split(&build_file_abs, &build_file_dirname, &build_file_basename);

        args.items[1] = buf_ptr(&build_file_dirname);

        bool build_file_exists;
        if ((err = os_file_exists(&build_file_abs, &build_file_exists))) {
            fprintf(stderr, "unable to open '%s': %s\n", buf_ptr(&build_file_abs), err_str(err));
            return 1;
        }
        if (!build_file_exists) {
            if (asked_for_help) {
                // This usage text has to be synchronized with std/special/build_runner.zig
                fprintf(stdout,
                        "Usage: %s build [options]\n"
                        "\n"
                        "General Options:\n"
                        "  --help                 Print this help and exit\n"
                        "  --build-file [file]    Override path to build.zig\n"
                        "  --verbose              Print commands before executing them\n"
                        "  --debug-build-verbose  Print verbose debugging information for the build system itself\n"
                        "  --prefix [prefix]      Override default install prefix\n"
                        "\n"
                        "More options become available when the build file is found.\n"
                        "Run this command with no options to generate a build.zig template.\n"
                , zig_exe_path);
                return 0;
            }
            Buf *build_template_path = buf_alloc();
            os_path_join(special_dir, buf_create_from_str("build_file_template.zig"), build_template_path);

            if ((err = os_copy_file(build_template_path, &build_file_abs))) {
                fprintf(stderr, "Unable to write build.zig template: %s\n", err_str(err));
            } else {
                fprintf(stderr, "Wrote build.zig template\n");
            }
            return 1;
        }

        PackageTableEntry *build_pkg = new_package(buf_ptr(&build_file_dirname), buf_ptr(&build_file_basename));
        build_pkg->package_table.put(buf_create_from_str("std"), g->std_package);
        g->root_package->package_table.put(buf_create_from_str("@build"), build_pkg);
        codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
        codegen_link(g, "build");

        Termination term;
        os_spawn_process("./build", args, &term);
        if (term.how != TerminationIdClean || term.code != 0) {
            fprintf(stderr, "\nBuild failed. Use the following command to reproduce the failure:\n");
            fprintf(stderr, "./build");
            for (size_t i = 0; i < args.length; i += 1) {
                fprintf(stderr, " %s", args.at(i));
            }
            fprintf(stderr, "\n");
        }
        return (term.how == TerminationIdClean) ? term.code : -1;
    }

    for (int i = 1; i < argc; i += 1) {
        char *arg = argv[i];

        if (arg[0] == '-') {
            if (strcmp(arg, "--release") == 0) {
                is_release_build = true;
            } else if (strcmp(arg, "--strip") == 0) {
                strip = true;
            } else if (strcmp(arg, "--static") == 0) {
                is_static = true;
            } else if (strcmp(arg, "--verbose") == 0) {
                verbose = true;
            } else if (strcmp(arg, "-mwindows") == 0) {
                mwindows = true;
            } else if (strcmp(arg, "-mconsole") == 0) {
                mconsole = true;
            } else if (strcmp(arg, "-municode") == 0) {
                municode = true;
            } else if (strcmp(arg, "-rdynamic") == 0) {
                rdynamic = true;
            } else if (strcmp(arg, "--each-lib-rpath") == 0) {
                each_lib_rpath = true;
            } else if (arg[1] == 'L' && arg[2] != 0) {
                // alias for --library-path
                lib_dirs.append(&arg[2]);
            } else if (i + 1 >= argc) {
                return usage(arg0);
            } else {
                i += 1;
                if (i >= argc) {
                    return usage(arg0);
                } else if (strcmp(arg, "--output") == 0) {
                    out_file = argv[i];
                } else if (strcmp(arg, "--color") == 0) {
                    if (strcmp(argv[i], "auto") == 0) {
                        color = ErrColorAuto;
                    } else if (strcmp(argv[i], "on") == 0) {
                        color = ErrColorOn;
                    } else if (strcmp(argv[i], "off") == 0) {
                        color = ErrColorOff;
                    } else {
                        return usage(arg0);
                    }
                } else if (strcmp(arg, "--name") == 0) {
                    out_name = argv[i];
                } else if (strcmp(arg, "--libc-lib-dir") == 0) {
                    libc_lib_dir = argv[i];
                } else if (strcmp(arg, "--libc-static-lib-dir") == 0) {
                    libc_static_lib_dir = argv[i];
                } else if (strcmp(arg, "--libc-include-dir") == 0) {
                    libc_include_dir = argv[i];
                } else if (strcmp(arg, "--zig-std-dir") == 0) {
                    zig_std_dir = argv[i];
                } else if (strcmp(arg, "--dynamic-linker") == 0) {
                    dynamic_linker = argv[i];
                } else if (strcmp(arg, "-isystem") == 0) {
                    clang_argv.append("-isystem");
                    clang_argv.append(argv[i]);
                } else if (strcmp(arg, "-dirafter") == 0) {
                    clang_argv.append("-dirafter");
                    clang_argv.append(argv[i]);
                } else if (strcmp(arg, "--library-path") == 0 || strcmp(arg, "-L") == 0) {
                    lib_dirs.append(argv[i]);
                } else if (strcmp(arg, "--library") == 0) {
                    link_libs.append(argv[i]);
                } else if (strcmp(arg, "--target-arch") == 0) {
                    target_arch = argv[i];
                } else if (strcmp(arg, "--target-os") == 0) {
                    target_os = argv[i];
                } else if (strcmp(arg, "--target-environ") == 0) {
                    target_environ = argv[i];
                } else if (strcmp(arg, "-mlinker-version") == 0) {
                    mlinker_version = argv[i];
                } else if (strcmp(arg, "-mmacosx-version-min") == 0) {
                    mmacosx_version_min = argv[i];
                } else if (strcmp(arg, "-mios-version-min") == 0) {
                    mios_version_min = argv[i];
                } else if (strcmp(arg, "-framework") == 0) {
                    frameworks.append(argv[i]);
                } else if (strcmp(arg, "--linker-script") == 0) {
                    linker_script = argv[i];
                } else if (strcmp(arg, "-rpath") == 0) {
                    rpath_list.append(argv[i]);
                } else if (strcmp(arg, "--test-filter") == 0) {
                    test_filter = argv[i];
                } else if (strcmp(arg, "--test-name-prefix") == 0) {
                    test_name_prefix = argv[i];
                } else {
                    fprintf(stderr, "Invalid argument: %s\n", arg);
                    return usage(arg0);
                }
            }
        } else if (cmd == CmdInvalid) {
            if (strcmp(arg, "build_exe") == 0) {
                cmd = CmdBuild;
                out_type = OutTypeExe;
            } else if (strcmp(arg, "build_obj") == 0) {
                cmd = CmdBuild;
                out_type = OutTypeObj;
            } else if (strcmp(arg, "build_lib") == 0) {
                cmd = CmdBuild;
                out_type = OutTypeLib;
            } else if (strcmp(arg, "link_lib") == 0) {
                cmd = CmdLink;
                out_type = OutTypeLib;
            } else if (strcmp(arg, "link_exe") == 0) {
                cmd = CmdLink;
                out_type = OutTypeExe;
            } else if (strcmp(arg, "version") == 0) {
                cmd = CmdVersion;
            } else if (strcmp(arg, "parseh") == 0) {
                cmd = CmdParseH;
            } else if (strcmp(arg, "test") == 0) {
                cmd = CmdTest;
            } else if (strcmp(arg, "targets") == 0) {
                cmd = CmdTargets;
            } else if (strcmp(arg, "asm") == 0) {
                cmd = CmdAsm;
            } else {
                fprintf(stderr, "Unrecognized command: %s\n", arg);
                return usage(arg0);
            }
        } else {
            switch (cmd) {
                case CmdBuild:
                case CmdParseH:
                case CmdTest:
                case CmdAsm:
                    if (!in_file) {
                        in_file = arg;
                    } else {
                        return usage(arg0);
                    }
                    break;
                case CmdLink:
                    objects.append(arg);
                    break;
                case CmdVersion:
                case CmdTargets:
                    return usage(arg0);
                case CmdInvalid:
                    zig_unreachable();
            }
        }
    }

    switch (cmd) {
    case CmdBuild:
    case CmdParseH:
    case CmdTest:
    case CmdAsm:
    case CmdLink:
        {
            bool one_source_input = (cmd == CmdBuild || cmd == CmdParseH || cmd == CmdTest || cmd == CmdAsm);
            if (one_source_input) {
                if (!in_file) {
                    fprintf(stderr, "Expected source file argument.\n");
                    return usage(arg0);
                }
            } else if (cmd == CmdLink) {
                if (objects.length == 0) {
                    fprintf(stderr, "Expected one or more object arguments.\n");
                    return usage(arg0);
                }
            } else {
                zig_unreachable();
            }

            assert((cmd != CmdBuild && cmd != CmdLink) || out_type != OutTypeUnknown);

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
                        fprintf(stderr, "invalid --target-arch argument\n");
                        return usage(arg0);
                    }
                }
                if (target_os) {
                    if (parse_target_os(target_os, &target->os)) {
                        fprintf(stderr, "invalid --target-os argument\n");
                        return usage(arg0);
                    }
                }
                if (target_environ) {
                    if (parse_target_environ(target_environ, &target->env_type)) {
                        fprintf(stderr, "invalid --target-environ argument\n");
                        return usage(arg0);
                    }
                }
            }

            bool need_name = (cmd == CmdBuild || cmd == CmdAsm || cmd == CmdLink);

            Buf in_file_buf = BUF_INIT;

            Buf root_source_dir = BUF_INIT;
            Buf root_source_code = BUF_INIT;
            Buf root_source_name = BUF_INIT;

            Buf *buf_out_name = (cmd == CmdTest) ? buf_create_from_str("test") :
                (out_name == nullptr) ? nullptr : buf_create_from_str(out_name);

            if (one_source_input) {
                buf_init_from_str(&in_file_buf, in_file);

                if (buf_eql_str(&in_file_buf, "-")) {
                    os_get_cwd(&root_source_dir);
                    if ((err = os_fetch_file(stdin, &root_source_code))) {
                        fprintf(stderr, "unable to read stdin: %s\n", err_str(err));
                        return 1;
                    }
                    buf_init_from_str(&root_source_name, "");

                } else {
                    os_path_split(&in_file_buf, &root_source_dir, &root_source_name);
                    if ((err = os_fetch_file_path(buf_create_from_str(in_file), &root_source_code))) {
                        fprintf(stderr, "unable to open '%s': %s\n", in_file, err_str(err));
                        return 1;
                    }

                    if (need_name && buf_out_name == nullptr) {
                        buf_out_name = buf_alloc();
                        Buf ext_name = BUF_INIT;
                        os_path_extname(&root_source_name, buf_out_name, &ext_name);
                    }
                }
            } else if (cmd == CmdLink) {
                os_get_cwd(&root_source_dir);
            } else {
                zig_unreachable();
            }

            if (need_name && buf_out_name == nullptr) {
                fprintf(stderr, "--name [name] not provided and unable to infer\n\n");
                return usage(arg0);
            }

            CodeGen *g = codegen_create(&root_source_dir, target);
            codegen_set_is_release(g, is_release_build);
            codegen_set_is_test(g, cmd == CmdTest);
            codegen_set_linker_script(g, linker_script);
            if (each_lib_rpath)
                codegen_set_each_lib_rpath(g, each_lib_rpath);

            codegen_set_clang_argv(g, clang_argv.items, clang_argv.length);
            codegen_set_strip(g, strip);
            codegen_set_is_static(g, is_static);
            if (cmd == CmdAsm) {
                codegen_set_out_type(g, OutTypeObj);
            } else if (out_type != OutTypeUnknown) {
                codegen_set_out_type(g, out_type);
            } else if (cmd == CmdTest) {
                codegen_set_out_type(g, OutTypeExe);
            }
            codegen_set_out_name(g, buf_out_name);
            if (libc_lib_dir)
                codegen_set_libc_lib_dir(g, buf_create_from_str(libc_lib_dir));
            if (libc_static_lib_dir)
                codegen_set_libc_static_lib_dir(g, buf_create_from_str(libc_static_lib_dir));
            if (libc_include_dir)
                codegen_set_libc_include_dir(g, buf_create_from_str(libc_include_dir));
            if (zig_std_dir)
                codegen_set_zig_std_dir(g, buf_create_from_str(zig_std_dir));
            if (dynamic_linker)
                codegen_set_dynamic_linker(g, buf_create_from_str(dynamic_linker));
            codegen_set_verbose(g, verbose);
            codegen_set_errmsg_color(g, color);

            for (size_t i = 0; i < lib_dirs.length; i += 1) {
                codegen_add_lib_dir(g, lib_dirs.at(i));
            }
            for (size_t i = 0; i < link_libs.length; i += 1) {
                codegen_add_link_lib(g, link_libs.at(i));
            }
            for (size_t i = 0; i < frameworks.length; i += 1) {
                codegen_add_framework(g, frameworks.at(i));
            }
            for (size_t i = 0; i < rpath_list.length; i += 1) {
                codegen_add_rpath(g, rpath_list.at(i));
            }

            codegen_set_windows_subsystem(g, mwindows, mconsole);
            codegen_set_windows_unicode(g, municode);
            codegen_set_rdynamic(g, rdynamic);
            if (mlinker_version) {
                codegen_set_mlinker_version(g, buf_create_from_str(mlinker_version));
            }
            if (mmacosx_version_min && mios_version_min) {
                fprintf(stderr, "-mmacosx-version-min and -mios-version-min options not allowed together\n");
                return EXIT_FAILURE;
            }

            if (mmacosx_version_min) {
                codegen_set_mmacosx_version_min(g, buf_create_from_str(mmacosx_version_min));
            }

            if (mios_version_min) {
                codegen_set_mios_version_min(g, buf_create_from_str(mios_version_min));
            }

            if (test_filter) {
                codegen_set_test_filter(g, buf_create_from_str(test_filter));
            }

            if (test_name_prefix) {
                codegen_set_test_name_prefix(g, buf_create_from_str(test_name_prefix));
            }

            if (cmd == CmdBuild) {
                codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_link(g, out_file);
                return EXIT_SUCCESS;
            } else if (cmd == CmdLink) {
                for (size_t i = 0; i < objects.length; i += 1) {
                    codegen_add_object(g, buf_create_from_str(objects.at(i)));
                }
                codegen_link(g, out_file);
                return EXIT_SUCCESS;
            } else if (cmd == CmdAsm) {
                codegen_add_root_assembly(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_link(g, out_file);
                return EXIT_SUCCESS;
            } else if (cmd == CmdParseH) {
                codegen_parseh(g, &root_source_dir, &root_source_name, &root_source_code);
                ast_render_decls(stdout, 4, g->root_import);
                return EXIT_SUCCESS;
            } else if (cmd == CmdTest) {
                codegen_add_root_code(g, &root_source_dir, &root_source_name, &root_source_code);
                codegen_link(g, "./test");
                ZigList<const char *> args = {0};
                Termination term;
                os_spawn_process("./test", args, &term);
                if (term.how != TerminationIdClean || term.code != 0) {
                    fprintf(stderr, "\nTests failed. Use the following command to reproduce the failure:\n");
                    fprintf(stderr, "./test\n");
                }
                return (term.how == TerminationIdClean) ? term.code : -1;
            } else {
                zig_unreachable();
            }
        }
    case CmdVersion:
        printf("%s\n", ZIG_VERSION_STRING);
        return EXIT_SUCCESS;
    case CmdTargets:
        return print_target_list(stdout);
    case CmdInvalid:
        return usage(arg0);
    }
}
