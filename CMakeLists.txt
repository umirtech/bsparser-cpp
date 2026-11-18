cmake_minimum_required(VERSION 3.20)

project(bsparser
    VERSION 0.1.0
    LANGUAGES C CXX
)

#Default to an optimized build when the user did not pick a build type.
#Without an optimization level the template - heavy parsers are ~100x slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

#Detect whether the building compiler actually supports C++ 20. This gates
#which headers get installed : the C++ 20 template headers(bsparser.hpp and
#the core bitstream / parser / syntax / logging tree) are only shipped when the
#compiler can compile them.Otherwise only the C - API headers are installed,
#which is the only path a non - C++ 20 consumer can use.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
#include <version>
#include <span>
#if __cplusplus < 202002L
#error requires C++20
#endif
int main() {
    std::span<int> s;
    (void)s;
    return 0; }
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Compiler selection
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#The project is designed to build with both GCC and Clang(and MSVC).
#Compiler warning flags are attached to the interface library so every
#consumer inherits them.
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Trace logging
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#OFF(default) : Debug / Trace logging statements are compiled out.
#The library is silent.
#ON : Debug / Trace statements are compiled in.The default
#runtime threshold becomes Trace; use
#bs::log::set_level() to filter.
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Tests
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Picture order count derivation (H.265 §8.3.1 / H.264 §8.2.1 / H.266 §8.3.1)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_poc_test
    tests/poc_test.cpp
)

target_link_libraries(
    bs_poc_test
    PRIVATE
        bs
)

add_test(
    NAME bs_poc_test
    COMMAND bs_poc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/stream.hevc
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_main.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/av1_sample.obu
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/vp9_sample.ivf
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/vp8_sample.ivf
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/vvc_sample.266
)

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Command - line parser / report exporter
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#C public API(stable ABI → compiled STATIC / SHARED library)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#The C++ 20 core stays header - only; this single translation unit is the only
#thing that gets compiled, giving C / older - toolchain / FFI consumers a
#stable, versionable ABI they can link against.
#
#cmake - DBS_CAPI_SHARED = ON..   → build a shared library

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

#C - API headers are always installed : the umbrella selects them for any
#non - C++ 20(or C) consumer, and they back the bs_capi library.
install(
    FILES bsparser.h
    DESTINATION include
)

install(
    FILES capi/bs_capi.h
    DESTINATION include
)

#The C++ 20 template headers are only shipped when the building compiler
#supports C++ 20. A pre - C++ 20(or C) consumer cannot use them header - only,
#so installing them would be misleading; the C - API library covers that case.
if(BS_CXX20_SUPPORTED)
    install(
        FILES bsparser.hpp
        DESTINATION include
    )

    install(
        DIRECTORY bitstream parser syntax logging utils demux mux
        DESTINATION include
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
    )
endif()

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#C API test(compiled as C, linked against the C++ runtime)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_capi_test
    tests/capi_test.c
)

target_link_libraries(
    bs_capi_test
    PRIVATE
        bs_capi
)

#The only translation unit is C, but it links a C++ static lib, so link
#with the C++ driver to pull in the C++ runtime.
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

#The same C API smoke test runs against the other codecs; the explicit codec
#avoids ambiguity in VCL-only / OBU samples.
foreach(pair IN ITEMS
    "avc_main.h264 avc"
    "vvc_sample.266 vvc"
    "av1_sample.obu av1"
    "vp9_sample.ivf vp9"
    "vp8_sample.ivf vp8"
)

    string(REPLACE " " ";" parts "${pair}")
    list(GET parts 0 sample)
    list(GET parts 1 codec)

    add_test(
        NAME bs_capi_${codec}
        COMMAND bs_capi_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${sample}
            ${codec}
    )

endforeach()


#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#C API accuracy verifier (verify_c vs pre-generated reference files)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#Built through CMake (not a hand-rolled link line) so the library name and
#location are resolved by the build system on every platform / toolchain
#(libbs_capi.a on GCC, bs_capi.lib on MSVC) instead of being hardcoded.

add_executable(
    bs_verify_c
    tools/verify_c.c
)

target_link_libraries(
    bs_verify_c
    PRIVATE
        bs_capi
)

#The only translation unit is C, but it links a C++ static lib, so link
#with the C++ driver to pull in the C++ runtime.
set_target_properties(
    bs_verify_c
    PROPERTIES
        LINKER_LANGUAGE CXX
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Parser-local object pool and zero-copy motion-buffer pool
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_object_pool_test
    tests/object_pool_test.cpp
)

target_link_libraries(
    bs_object_pool_test
    PRIVATE
        bs
)

add_test(
    NAME bs_object_pool_test
    COMMAND bs_object_pool_test
)

add_executable(
    bs_motion_buffer_test
    tests/motion_buffer_test.cpp
)

target_link_libraries(
    bs_motion_buffer_test
    PRIVATE
        bs
)

add_test(
    NAME bs_motion_buffer_test
    COMMAND bs_motion_buffer_test
)

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Portable SIMD kernels for the 4x4 motion-cell grid
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_avc_simd_test
    tests/avc_cell_simd_test.cpp
)

target_link_libraries(
    bs_avc_simd_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_simd_test
    COMMAND bs_avc_simd_test
)

#Run the same suite with AVX2 disabled so the SSE2 baseline kernel is
#exercised even on a machine whose runtime dispatch would pick AVX2.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")

    add_executable(
        bs_avc_simd_sse2_test
        tests/avc_cell_simd_test.cpp
    )

    target_compile_definitions(
        bs_avc_simd_sse2_test
        PRIVATE
            BS_AVC_CELL_SIMD_DISABLE_AVX2=1
    )

    target_link_libraries(
        bs_avc_simd_sse2_test
        PRIVATE
            bs
    )

    add_test(
        NAME bs_avc_simd_sse2_test
        COMMAND bs_avc_simd_sse2_test
    )

endif()

add_executable(
    bs_avc_cavlc_test
    tests/avc_cavlc_test.cpp
)

target_link_libraries(
    bs_avc_cavlc_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_cavlc_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_cavlc.h264
)

add_test(
    NAME bs_avc_residual_cavlc_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc.h264
)

add_test(
    NAME bs_avc_residual_cavlc_x264_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc_x264.h264
)

add_test(
    NAME bs_avc_residual_cavlc_8x8_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc_8x8.h264
)

add_test(
    NAME bs_avc_residual_cavlc_intra4x4_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc_intra4x4.h264
)

add_test(
    NAME bs_avc_residual_cavlc_b_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc_b.h264
)


add_test(
    NAME bs_avc_bpyramid_cavlc_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_bpyramid_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_bpyramid_cavlc.mv.txt
        15
)

add_test(
    NAME bs_avc_residual_cavlc_lossless_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_cavlc_lossless.h264
)

add_test(
    NAME bs_avc_multislice_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_multislice_cabac.h264
)

add_test(
    NAME bs_avc_422_static_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_422_cabac.h264
)

add_test(
    NAME bs_avc_444_static_test
    COMMAND bs_avc_cavlc_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_444_cabac.h264
)

add_executable(
    bs_avc_cabac_test
    tests/avc_cabac_test.cpp
)

target_link_libraries(
    bs_avc_cabac_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_cabac_test
    COMMAND bs_avc_cabac_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_422_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_444_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_10bit_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_420.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_444.h264
)

add_executable(
    bs_avc_mv_test
    tests/avc_mv_test.cpp
)

target_link_libraries(
    bs_avc_mv_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_test
    COMMAND bs_avc_mv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_static_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_move_b_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mv_static.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_main.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mv_main.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_sdr_hd.h264
)

add_executable(
    bs_avc_mv_field_test
    tests/avc_mv_field_test.cpp
)

target_link_libraries(
    bs_avc_mv_field_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_field_test
    COMMAND bs_avc_mv_field_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_paff_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_cavlc.h264
)

add_executable(
    bs_avc_mv_dpb_test
    tests/avc_mv_dpb_test.cpp
)

target_link_libraries(
    bs_avc_mv_dpb_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_dpb_test
    COMMAND bs_avc_mv_dpb_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mv_multiref_b.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mv_multiref_b.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mmco_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_field_reorder_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_field_mmco_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_field_b_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_frame_num_wrap_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_frame_num_gap_cavlc.h264
)

add_executable(
    bs_avc_mv_ext_test
    tests/avc_mv_ext_test.cpp
)

target_link_libraries(
    bs_avc_mv_ext_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_ext_test
    COMMAND bs_avc_mv_ext_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_frame_num_gap_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_frame_num_gap_field_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_frame_num_gap_poc0_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_multislice_refs_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_no_output_prior_cavlc.h264
)

add_executable(
    bs_avc_mv_chroma_test
    tests/avc_mv_chroma_test.cpp
)

target_link_libraries(
    bs_avc_mv_chroma_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_chroma_test
    COMMAND bs_avc_mv_chroma_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_residual_422_cabac.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_residual_422_cavlc.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_residual_444_cabac.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_residual_444_cavlc.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_cavlc_8x8.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_residual_444_cavlc_8x8.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_10bit_cabac.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_10bit_qp60_cabac.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_pcm_cavlc.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm_cavlc.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_pcm10_cavlc.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm10_cavlc.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm_cabac.h264
        -
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_420.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_420.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_422.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_422.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_444.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_444.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_420_10bit.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_420_10bit.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_422_10bit.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_422_10bit.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_444_10bit.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cabac_444_10bit.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cavlc_420.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_pcm_cavlc_420.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mono_cabac.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mono_cavlc.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_pcm_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mono_pcm_cabac.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_pcm_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mono_pcm_cavlc.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_10bit_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mono_10bit_cabac.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cavlc_frame.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mbaff_422_cavlc_frame.mv.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cavlc_frame.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/reference/avc_mbaff_444_cavlc_frame.mv.txt
)

add_executable(
    bs_avc_mv_pcm_test
    tests/avc_mv_pcm_test.cpp
)

target_link_libraries(
    bs_avc_mv_pcm_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_mv_pcm_test
    COMMAND bs_avc_mv_pcm_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_420.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_422.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_444.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_420_10bit.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_422_10bit.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cabac_444_10bit.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_pcm_cavlc_420.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_pcm_cavlc.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm_cavlc.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_422_pcm10_cavlc.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm10_cavlc.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_residual_444_pcm_cabac.h264
        pcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_cabac.h264
        mono
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_cavlc.h264
        mono
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_pcm_cabac.h264
        mono
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_pcm_cavlc.h264
        mono
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mono_10bit_cabac.h264
        mono
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_sep_planes_cavlc.h264
        sep
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_b_pcm_cavlc.h264
        bpcm
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_b_pcm_cabac.h264
        bpcm-cabac
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_paff_422_cavlc.h264
        field-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_paff_444_cavlc.h264
        field-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_paff_422_10bit_cavlc.h264
        field-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_paff_444_10bit_cavlc.h264
        field-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cabac.h264
        mbaff-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cabac.h264
        mbaff-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cabac_10bit.h264
        mbaff-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cabac_10bit.h264
        mbaff-chroma
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cabac_noskip.h264
        mbaff-chroma-8
)

add_test(
    NAME bs_avc_mbaff_cavlc_test
    COMMAND bs_avc_mv_pcm_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cavlc.h264
        mbaff-chroma-cavlc
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cavlc.h264
        mbaff-chroma-cavlc
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cavlc_10bit.h264
        mbaff-chroma-cavlc-10bit
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cavlc_10bit.h264
        mbaff-chroma-cavlc-10bit
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_422_cavlc_frame.h264
        mbaff-chroma-cavlc-frame
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_mbaff_444_cavlc_frame.h264
        mbaff-chroma-cavlc-frame
)

add_executable(
    bs_avc_fmo_test
    tests/avc_fmo_test.cpp
)

target_link_libraries(
    bs_avc_fmo_test
    PRIVATE
        bs
)

add_test(
    NAME bs_avc_fmo_test
    COMMAND bs_avc_fmo_test
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map0_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map0_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map1_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map1_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map2_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map2_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map3_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map3_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map4_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map4_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map5_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map5_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map6_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_map6_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map0_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map0_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map1_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map1_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map2_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map2_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map3_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map3_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map4_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map4_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map5_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map5_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map6_cavlc.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_refs_map6_cabac.h264
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/avc_fmo_mixed_i_p_map0_cavlc.h264
)

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#AVC / H.264 parser demo
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Additional codecs : VVC / AV1 / VP9 / VP8
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

set(BS_NEW_CODEC_TESTS
    Vvc  vvc_sample.266
    Av1  av1_sample.obu
    Vp9  vp9_sample.ivf
    Vp8  vp8_sample.ivf
)

list(LENGTH BS_NEW_CODEC_TESTS BS_NEW_CODEC_TEST_COUNT)
math(EXPR BS_NEW_CODEC_TEST_MAX "${BS_NEW_CODEC_TEST_COUNT} - 1")

foreach(idx RANGE 0 ${BS_NEW_CODEC_TEST_MAX} 2)

    list(GET BS_NEW_CODEC_TESTS ${idx} codec)
    math(EXPR nxt "${idx} + 1")
    list(GET BS_NEW_CODEC_TESTS ${nxt} sample)

    string(TOLOWER ${codec} name)

    add_executable(
        bs_${name}_test
        tests/${name}_test.cpp
    )

    target_link_libraries(
        bs_${name}_test
        PRIVATE
            bs
    )

    add_test(
        NAME bs_${name}_test
        COMMAND bs_${name}_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${sample}
    )

endforeach()

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Container demux tests(muxed samples->elementary stream)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_demux_test
    tests/demux_test.cpp
)

target_link_libraries(
    bs_demux_test
    PRIVATE
        bs
)

foreach(pair IN ITEMS
    "hevc.mp4 hevc"
    "h264.mp4 avc"
    "vvc.mp4 vvc"
    "av1.mp4 av1"
    "vp9.mp4 vp9"
    "h264_enc.ts avc"
    "h264_enc.flv avc"
    "h264_enc.avi avc"
    "vp9.webm vp9"
    "hevc.mkv hevc"
    "h264.mkv avc"
    "av1.mkv av1"
    "multi.mkv hevc"
    "multi.mp4 hevc"
    "multi.ts avc"
    "multi.flv avc"
)

    string(REPLACE " " ";" parts "${pair}")
    list(GET parts 0 sample)
    list(GET parts 1 codec)

    add_test(
        NAME bs_demux_${sample}
        COMMAND bs_demux_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${sample}
            ${codec}
    )

endforeach()

add_executable(
    bs_demux_stream_test
    tests/demux_stream_test.cpp
)

target_link_libraries(
    bs_demux_stream_test
    PRIVATE
        bs
)

foreach(sample IN ITEMS
    hevc.mp4
    h264.mp4
    vvc.mp4
    av1.mp4
    vp9.mp4
    h264_enc.ts
    h264_enc.flv
    h264_enc.avi
    vp9.webm
    hevc.mkv
    h264.mkv
    av1.mkv
    multi.mkv
    multi.mp4
    multi.ts
    multi.flv
)

    add_test(
        NAME bs_demux_stream_${sample}
        COMMAND bs_demux_stream_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${sample}
    )

endforeach()

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Container mux tests(samples->muxed file->demux round trip)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -

add_executable(
    bs_mux_test
    tests/mux_test.cpp
)

target_link_libraries(
    bs_mux_test
    PRIVATE
        bs
)

add_executable(
    bs_mux_stream_test
    tests/mux_stream_test.cpp
)

target_link_libraries(
    bs_mux_stream_test
    PRIVATE
        bs
)

foreach(sample IN ITEMS
    h264.mp4
    hevc.mp4
    vvc.mp4
    av1.mp4
    vp9.mp4
    vp8_sample.ivf
)

    add_test(
        NAME bs_mux_${sample}
        COMMAND bs_mux_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${sample}
    )

endforeach()

add_executable(
    bs_mux_metadata_test
    tests/mux_metadata_test.cpp
)

target_link_libraries(
    bs_mux_metadata_test
    PRIVATE
        bs
)

add_executable(
    bs_malformed_test
    tests/malformed_test.cpp
)

target_link_libraries(
    bs_malformed_test
    PRIVATE
        bs
)

foreach(fixture IN ITEMS
    avi_bad_chunk.avi
    mp4_bad_fragsize.mp4
    mp4_bad_stsz.mp4
    ogg_bad_segments.ogg
)

    add_test(
        NAME bs_malformed_${fixture}
        COMMAND bs_malformed_test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/malicious/${fixture}
    )

endforeach()

add_executable(
    bs_mux_wav_test
    tests/mux_wav_test.cpp
)

target_link_libraries(
    bs_mux_wav_test
    PRIVATE
        bs
)

add_test(
    NAME bs_mux_wav_test
    COMMAND bs_mux_wav_test
)

add_executable(
    bs_mux_robustness_test
    tests/mux_robustness_test.cpp
)

target_link_libraries(
    bs_mux_robustness_test
    PRIVATE
        bs
)

add_test(
    NAME bs_mux_robustness_test
    COMMAND bs_mux_robustness_test
)

add_test(
    NAME bs_mux_metadata_test
    COMMAND bs_mux_metadata_test
)

add_test(
    NAME bs_mux_stream_test
    COMMAND bs_mux_stream_test
)

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Fuzzing
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#bs_fuzz libFuzzer target.Clang only(GCC does not ship
#the - fsanitize = fuzzer runtime).
#
#bs_fuzz_driver Standalone file - based runner over the same
#LLVMFuzzerTestOneInput harness.Builds with any
#compiler(GCC, Clang, MSVC) and is the vehicle for
#ASan / UBSan fuzzing on GCC.
#
#Usage:
#
# #libFuzzer(Clang)
#cmake - B build - fuzz - DCMAKE_CXX_COMPILER = clang++
#cmake-- build build - fuzz-- target bs_fuzz
#./ build - fuzz / bs_fuzz tests / fuzz / corpus - max_len = 8192
#
# #Standalone driver(GCC or Clang)
#cmake-- build build-- target bs_fuzz_driver
#./ build / bs_fuzz_driver tests / fuzz / corpus/*.hevc
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

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#Optional benchmark and deterministic-hash tools (development only)
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#
#These targets are not part of ctest and are never built by default:
#
#    cmake -B build-bench -DBS_BUILD_BENCHMARKS=ON
#    cmake --build build-bench --target the AVC benchmark tool hash_avc_mv
#    ./build-bench/the AVC benchmark tool stream.h264 5 150 1920 1080
#    ./build-bench/hash_avc_mv stream.h264
#
option(BS_BUILD_BENCHMARKS
    "Build the optional AVC benchmark and deterministic hash tools"
    OFF
)

if(BS_BUILD_BENCHMARKS)

    add_executable(
        the AVC benchmark tool
        tools/the AVC benchmark tool.cpp
    )

    target_link_libraries(
        the AVC benchmark tool
        PRIVATE
            bs
    )

    add_executable(
        hash_avc_mv
        tools/hash_avc_mv.cpp
    )

    target_link_libraries(
        hash_avc_mv
        PRIVATE
            bs
    )

endif()
