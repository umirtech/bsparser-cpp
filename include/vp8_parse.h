#pragma once
#include <cstddef>
#include <bitreader.h>

namespace bsparser {
    
   Header parse_vp8(const Bytes& d, uint64_t off);

}