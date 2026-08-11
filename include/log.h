#include <cstdio>

#define logE(tag, fmt, ...) \
    std::fprintf(stderr, "[ERROR] [%s] " fmt "\n", tag, ##__VA_ARGS__)

#define logI(tag, fmt, ...) \
    std::fprintf(stdout, "[INFO ] [%s] " fmt "\n", tag, ##__VA_ARGS__)

#define logD(tag, fmt, ...) \
    std::fprintf(stdout, "[DEBUG] [%s] " fmt "\n", tag, ##__VA_ARGS__)

#define logW(tag, fmt, ...) \
    std::fprintf(stderr, "[WARN ] [%s] " fmt "\n", tag, ##__VA_ARGS__)