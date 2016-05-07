# Copyright (c) 2016 Andrew Kelley
# This file is MIT licensed.
# See http://opensource.org/licenses/MIT

# ZIG_FOUND
# ZIG_INCLUDE_DIR
# ZIG_BINARY

find_program(ZIG_BINARY zig)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ZIG DEFAULT_MSG ZIG_BINARY)

mark_as_advanced(ZIG_BINARY)
