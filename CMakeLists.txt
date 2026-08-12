cmake_minimum_required(VERSION 3.16)
project(bsparser_cpp VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Output directories (everything stays inside the build folder)
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)   # Executables
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)   # Shared libraries
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)   # Static libraries

# Multi-config generators (Visual Studio, Xcode)
foreach(CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER ${CONFIG} CONFIG_UPPER)

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/bin/${CONFIG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/lib/${CONFIG})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER}
        ${CMAKE_BINARY_DIR}/lib/${CONFIG})
endforeach()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(
    bsparser
    STATIC
    src/bsparser.cpp
    src/bitreader.cpp
    src/hevc_parse.cpp
    src/vvc_parse.cpp
    src/avc_parse.cpp
    src/av1_parse.cpp
    src/vp9_parse.cpp
    src/vp8_parse.cpp
)

target_include_directories(bsparser
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(bsparser PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(bsparser PRIVATE /W4 /permissive-)
else()
    target_compile_options(bsparser PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Example executable
# ---------------------------------------------------------------------------
add_executable(bsparse src/main.cpp)
target_link_libraries(bsparse PRIVATE bsparser)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(bsparser_tests tests/tests.cpp)
target_link_libraries(bsparser_tests PRIVATE bsparser)
target_compile_definitions(
    bsparser_tests
    PRIVATE BSPARSER_TEST_FILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test-files"
)

# add_test(
#    NAME bsparser_tests
#    COMMAND bsparser_tests 
#)

add_test(
    NAME bsparse
    COMMAND bsparse hevc  ${CMAKE_CURRENT_SOURCE_DIR}/test-files/h265.h265 --html h265.html
)
