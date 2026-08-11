#include <arm_neon.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <arm_vector_types.h>

namespace
{

size_t find_start_code_scalar(const uint8_t* data, size_t size, size_t from, size_t& startCodeSize)
{
    startCodeSize = 0;

    if (size < 3 || from > size - 3)
    {
        return std::numeric_limits<size_t>::max();
    }

    for (size_t i = from; i + 3 <= size; ++i)
    {
        if (data[i] != 0 || data[i + 1] != 0) continue;

        // 00 00 00 01
        if (data[i + 2] == 0 && i + 3 < size && data[i + 3] == 1)
        {
            startCodeSize = 4;
            return i;
        }

        // 00 00 01
        if (data[i + 2] == 1)
        {
            startCodeSize = 3;
            return i;
        }
    }

    return std::numeric_limits<size_t>::max();
}
}  // namespace

extern "C" size_t bsparser_find_start_code_neon_armv7(
    const uint8_t* data, size_t size, size_t from, size_t& startCodeSize
)
{
    startCodeSize = 0;

    if (size < 3 || from > size - 3)
    {
        return std::numeric_limits<size_t>::max();
        ;
    }

    const uint8x16_t zero = vdupq_n_u8(0);
    size_t cursor = from;

    while (cursor + 16 <= size)
    {
        const uint8x16_t bytes = vld1q_u8(data + cursor);
        const uint8x16_t equal_zero = vceqq_u8(bytes, zero);

        uint8_t matches[16];
        vst1q_u8(matches, equal_zero);

        for (uint8_t match : matches)
        {
            if (match != 0)
            {
                return find_start_code_scalar(data, std::min(size, cursor + 16), cursor, startCodeSize);
            }
        }

        cursor += 16;
    }

    return find_start_code_scalar(data, size, cursor, startCodeSize);
}
