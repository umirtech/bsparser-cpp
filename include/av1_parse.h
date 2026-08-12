#pragma once
#include <cstddef>
#include <bitreader.h>

namespace bsparser {
    
   std::vector<Header> parse_av1(const Bytes& d, uint64_t off);
   
}