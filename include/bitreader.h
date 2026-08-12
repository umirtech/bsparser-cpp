#pragma once
#include <common.h>

namespace bsparser {

  // A safe, MSB-first reader for video syntax elements. All methods throw
  // std::out_of_range when the input is truncated.
  class BitReader {
  public:
    explicit BitReader(const std::vector<uint8_t>& data);

    uint32_t u(unsigned bits);
    int32_t s(unsigned bits);
    uint32_t ue();
    int32_t se();
    uint64_t leb128();

    void rbsp_trailing_bits();
    [[nodiscard]] size_t bit_position() const noexcept;
    [[nodiscard]] size_t bits_left() const noexcept;

    [[nodiscard]] bool more_rbsp_data() const noexcept;

    void skip(size_t bits);

  private:
    uint32_t peek_bit(size_t offset) const noexcept;
    const std::vector<uint8_t>& data_;
    size_t bitpos_ = 0;
  };
  
}
