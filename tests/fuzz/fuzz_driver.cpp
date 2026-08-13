/*
 * ---------------------------------------------------------------------------
 * Standalone fuzz driver
 * ---------------------------------------------------------------------------
 *
 * Provides a main() for the shared LLVMFuzzerTestOneInput harness so the
 * same fuzz target can be built with compilers that do not ship libFuzzer
 * (notably GCC).
 *
 * Usage:
 *
 *     hevc_fuzz_driver <file> [file...]
 *     hevc_fuzz_driver          (reads stdin until EOF)
 *     hevc_fuzz_driver -        (reads stdin until EOF)
 *
 * Each input file is parsed once; a non-zero exit status reports that one
 * or more inputs could not be opened.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int
LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size);


namespace {

int run_bytes(
    const std::vector<std::uint8_t>& data)
{
    if (data.empty()) {
        return 0;
    }

    LLVMFuzzerTestOneInput(
        data.data(),
        data.size());

    return 0;
}


int run_file(
    const char* path)
{
    std::ifstream in(
        path,
        std::ios::binary);

    if (!in) {
        std::cerr << "hevc_fuzz_driver: cannot open: "
                  << path << "\n";
        return 1;
    }

    const std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    return run_bytes(data);
}


int run_stdin()
{
    const std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(std::cin)),
        std::istreambuf_iterator<char>());

    return run_bytes(data);
}

}  // namespace


int main(
    int argc,
    char** argv)
{
    if (argc < 2) {
        return run_stdin();
    }

    int failed = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-") {
            failed += run_stdin();
        } else {
            failed += run_file(argv[i]);
        }
    }

    return failed == 0 ? 0 : 1;
}