#include "bsparser/bsparser.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: bsparse <ivf|vp8|vp9|av1|avc|hevc|vvc> <input>\n";
    return 2;
  }
  std::ifstream file(argv[2], std::ios::binary);
  if (!file) {
    std::cerr << "Unable to open input file\n";
    return 2;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), {});
  try {
    std::vector<bsparser::Header> headers;
    if (std::string(argv[1]) == "ivf") {
      bsparser::IvfParser parser;
      headers = parser.feed(data);
    } else {
      bsparser::StreamParser parser(bsparser::codec_from_name(argv[1]));
      headers = parser.feed(data);
      auto final = parser.finish();
      headers.insert(headers.end(), final.begin(), final.end());
    }
    for (const auto& h : headers) std::cout << bsparser::to_json(h) << '\n';
  } catch (const std::exception& e) {
    std::cerr << "Parse error: " << e.what() << '\n';
    return 1;
  }
}
