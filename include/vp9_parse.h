#pragma once
#include <cstddef>
#include <bitreader.h>

namespace bsparser {
    
   Header parse_vp9(const Bytes& d, uint64_t off);

}