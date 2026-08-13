cmake_minimum_required(VERSION 3.20)

project(bsparser
    VERSION 0.1.0
    LANGUAGES C CXX
)

# Detect whether the building compiler actually supports C++20.  This gates
# which headers get installed: the C++20 template headers (bsparser.hpp and
# the core bitstream/parser/syntax/logging tree) are only shipped when the
# compiler can compile them.  Otherwise only the C-API headers are installed,
# which is the only path a non-C++20 consumer can use.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
#include <version>
#include <span>
#if __cplusplus < 202002L
#error requires C++20
#endif
int main() { std::span<int> s; (void)s; return 0; }
" BS_CXX20_SUPPORTED)

add_library(bs
    INTERFACE
)

target_include_directories(bs
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/bitstream/
        ${CMAKE_CURRENT_SOURCE_DIR}/logging/
        ${CMAKE_CURRENT_SOURCE_DIR}/parser/
        ${CMAKE_CURRENT_SOURCE_DIR}/syntax/
)

# ---------------------------------------------------------------------------
# Compiler selection
# ---------------------------------------------------------------------------
#
# The project is designed to build with both GCC and Clang (and MSVC).
# Compiler warning flags are attached to the interface library so every
# consumer inherits them.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")

    target_compile_options(bs
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
    )

elseif(MSVC)

    target_compile_options(bs
        INTERFACE
            /W4
            /permissive-
    )

endif()


# ---------------------------------------------------------------------------
# Trace logging
# ---------------------------------------------------------------------------
#
# OFF (default): Debug/Trace logging statements are compiled out.
#                The library is silent.
# ON:            Debug/Trace statements are compiled in.  The default
#                runtime threshold becomes Trace; use
#                bs::log::set_level() to filter.
option(BS_ENABLE_TRACE
    "Compile in verbose parser trace logging"
    OFF
)

target_compile_definitions(bs
    INTERFACE
        $<$<BOOL:${BS_ENABLE_TRACE}>:BS_ENABLE_TRACE=1>
)

target_compile_features(bs
    INTERFACE
        cxx_std_20
)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(
    bs_unified_test
    tests/unified_test.cpp
)

target_link_libraries(
    bs_unified_test
    PRIVATE
        bs
)

add_test(
    NAME bs_unified_test
    COMMAND bs_unified_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)


# ---------------------------------------------------------------------------
# Command-line parser / report exporter
# ---------------------------------------------------------------------------

add_executable(
    bs_cli
    cli/bs_cli.cpp
)

target_link_libraries(
    bs_cli
    PRIVATE
        bs
)

add_test(
    NAME bs_cli_hevc
    COMMAND bs_cli
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/stream.hevc
        --out ${CMAKE_CURRENT_BINARY_DIR}/cli_hevc.json
)

add_test(
    NAME bs_cli_avc
    COMMAND bs_cli
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_main.h264
        --codec avc
        --out ${CMAKE_CURRENT_BINARY_DIR}/cli_avc.html
)


# ---------------------------------------------------------------------------
# C public API (stable ABI → compiled STATIC / SHARED library)
# ---------------------------------------------------------------------------
#
# The C++20 core stays header-only; this single translation unit is the only
# thing that gets compiled, giving C / older-toolchain / FFI consumers a
# stable, versionable ABI they can link against.
#
#   cmake -DBS_CAPI_SHARED=ON ..   → build a shared library

option(BS_CAPI_SHARED
    "Build the C API as a shared library (default: static)"
    OFF
)

if(BS_CAPI_SHARED)
    set(BS_CAPI_LIB_TYPE SHARED)
else()
    set(BS_CAPI_LIB_TYPE STATIC)
endif()

add_library(bs_capi
    ${BS_CAPI_LIB_TYPE}
    capi/bs_capi.cpp
)

target_link_libraries(bs_capi
    PUBLIC
        bs
)

target_include_directories(bs_capi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/capi
)

set_target_properties(bs_capi
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
)

install(
    TARGETS bs_capi
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# C-API headers are always installed: the umbrella selects them for any
# non-C++20 (or C) consumer, and they back the bs_capi library.
install(
    FILES bsparser.h
    DESTINATION include
)

install(
    FILES capi/bs_capi.h
    DESTINATION include
)

# The C++20 template headers are only shipped when the building compiler
# supports C++20.  A pre-C++20 (or C) consumer cannot use them header-only,
# so installing them would be misleading; the C-API library covers that case.
if(BS_CXX20_SUPPORTED)
    install(
        FILES bsparser.hpp
        DESTINATION include
    )

    install(
        DIRECTORY bitstream parser syntax logging
        DESTINATION include
        FILES_MATCHING PATTERN "*.hpp"
    )
endif()


# ---------------------------------------------------------------------------
# C API test (compiled as C, linked against the C++ runtime)
# ---------------------------------------------------------------------------

add_executable(
    bs_capi_test
    tests/capi_test.c
)

target_link_libraries(
    bs_capi_test
    PRIVATE
        bs_capi
)

# The only translation unit is C, but it links a C++ static lib, so link
# with the C++ driver to pull in the C++ runtime.
set_target_properties(
    bs_capi_test
    PROPERTIES
        LINKER_LANGUAGE CXX
)

add_test(
    NAME bs_capi_test
    COMMAND bs_capi_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/stream.hevc
)



add_executable(
    bs_hevc_test
    tests/hevc_test.cpp
)

target_link_libraries(
    bs_hevc_test
    PRIVATE
        bs
)

add_test(
    NAME bs_hevc_test
    COMMAND bs_hevc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/stream.hevc
)


# ---------------------------------------------------------------------------
# AVC / H.264 parser demo
# ---------------------------------------------------------------------------

add_executable(
    bs_avc_test
    tests/avc_test.cpp
)

target_link_libraries(
    bs_avc_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_test
    COMMAND bs_avc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_main.h264
)

add_test(
    NAME bs_avc_test_high444
    COMMAND bs_avc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_high444.h264
)

add_executable(
    bs_avc_hdr_test
    tests/avc_hdr_test.cpp
)

target_link_libraries(
    bs_avc_hdr_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_hdr_test
    COMMAND bs_avc_hdr_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_hdr.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_hdr.txt
)



add_executable(
    bs_hevc_hdr_test
    tests/hevc_hdr_test.cpp
)

target_link_libraries(
    bs_hevc_hdr_test
    PRIVATE
        bs
)

add_test(
    NAME bs_hevc_hdr_test
    COMMAND bs_hevc_hdr_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/hevc_hdr10.hevc
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/hevc_hdr10.txt
)


# ---------------------------------------------------------------------------
# Fuzzing
# ---------------------------------------------------------------------------
#
# bs_fuzz            libFuzzer target.  Clang only (GCC does not ship
#                    the -fsanitize=fuzzer runtime).
#
# bs_fuzz_driver     Standalone file-based runner over the same
#                    LLVMFuzzerTestOneInput harness.  Builds with any
#                    compiler (GCC, Clang, MSVC) and is the vehicle for
#                    ASan/UBSan fuzzing on GCC.
#
# Usage:
#
#     # libFuzzer (Clang)
#     cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++
#     cmake --build build-fuzz --target bs_fuzz
#     ./build-fuzz/bs_fuzz tests/fuzz/corpus -max_len=8192
#
#     # Standalone driver (GCC or Clang)
#     cmake --build build --target bs_fuzz_driver
#     ./build/bs_fuzz_driver tests/fuzz/corpus/*.hevc
#
option(BS_ENABLE_FUZZING
    "Build fuzz harnesses"
    ON
)

option(BS_ENABLE_SANITIZERS
    "Enable ASan/UBSan on the standalone fuzz driver"
    ON
)

if(BS_ENABLE_FUZZING)

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")

        include(CheckCXXCompilerFlag)

        # libFuzzer is not shipped by every Clang distribution (notably
        # MinGW-targeting builds).  Probe for it so the build does not
        # fail on such toolchains.
        check_cxx_compiler_flag(
            "-fsanitize=fuzzer"
            BS_HAVE_LIBFUZZER
        )

    else()

        set(BS_HAVE_LIBFUZZER FALSE)

    endif()

    if(BS_HAVE_LIBFUZZER)

        add_executable(
            bs_fuzz
            tests/fuzz/fuzz_hevc.cpp
        )

        target_compile_features(
            bs_fuzz
            PRIVATE
                cxx_std_20
        )

        target_link_libraries(
            bs_fuzz
            PRIVATE
                bs
        )

        target_compile_options(
            bs_fuzz
            PRIVATE
                -g
                -O1
                -fno-omit-frame-pointer
                -fsanitize=fuzzer,address,undefined
        )

        target_link_options(
            bs_fuzz
            PRIVATE
                -fsanitize=fuzzer,address,undefined
        )

    else()

        message(STATUS
            "libFuzzer (-fsanitize=fuzzer) not available; "
            "skipping the bs_fuzz target")

    endif()

    add_executable(
        bs_fuzz_driver
        tests/fuzz/fuzz_hevc.cpp
        tests/fuzz/fuzz_driver.cpp
    )

    target_compile_features(
        bs_fuzz_driver
        PRIVATE
            cxx_std_20
    )

    target_link_libraries(
        bs_fuzz_driver
        PRIVATE
            bs
    )

    if(BS_ENABLE_SANITIZERS AND
       CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")

        target_compile_options(
            bs_fuzz_driver
            PRIVATE
                -g
                -O1
                -fno-omit-frame-pointer
                -fsanitize=address,undefined
        )

        target_link_options(
            bs_fuzz_driver
            PRIVATE
                -fsanitize=address,undefined
        )

        if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "GNU")

            # MinGW GCC needs the sanitizer runtimes statically linked to
            # avoid a runtime dependency on libasan.dll / libubsan.dll.
            target_compile_options(
                bs_fuzz_driver
                PRIVATE
                    -static-libasan
                    -static-libubsan
            )

            target_link_options(
                bs_fuzz_driver
                PRIVATE
                    -static-libasan
                    -static-libubsan
            )

        endif()

    endif()

    add_test(
        NAME bs_fuzz_smoke
        COMMAND bs_fuzz_driver
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/stream.hevc
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/ext_stream.hevc
    )

endif()
