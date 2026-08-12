#pragma once
#include <cstddef>
#include <bitreader.h>

namespace bsparser {
    
   Header parse_vvc(const Bytes& d, uint64_t off);

}