#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cinatra::http2 {

struct hpack_huffman_code {
  uint32_t bits;
  uint8_t bit_length;
};

inline constexpr std::array<hpack_huffman_code, 257> HPACK_HUFFMAN_CODES{{
    {0x1ff8u, 13},     {0x7fffd8u, 23},   {0xfffffe2u, 28},  {0xfffffe3u, 28},
    {0xfffffe4u, 28},  {0xfffffe5u, 28},  {0xfffffe6u, 28},  {0xfffffe7u, 28},
    {0xfffffe8u, 28},  {0xffffeau, 24},   {0x3ffffffcu, 30}, {0xfffffe9u, 28},
    {0xfffffeau, 28},  {0x3ffffffdu, 30}, {0xfffffebu, 28},  {0xfffffecu, 28},
    {0xfffffedu, 28},  {0xfffffeeu, 28},  {0xfffffefu, 28},  {0xffffff0u, 28},
    {0xffffff1u, 28},  {0xffffff2u, 28},  {0x3ffffffeu, 30}, {0xffffff3u, 28},
    {0xffffff4u, 28},  {0xffffff5u, 28},  {0xffffff6u, 28},  {0xffffff7u, 28},
    {0xffffff8u, 28},  {0xffffff9u, 28},  {0xffffffau, 28},  {0xffffffbu, 28},
    {0x14u, 6},        {0x3f8u, 10},      {0x3f9u, 10},      {0xffau, 12},
    {0x1ff9u, 13},     {0x15u, 6},        {0xf8u, 8},        {0x7fau, 11},
    {0x3fau, 10},      {0x3fbu, 10},      {0xf9u, 8},        {0x7fbu, 11},
    {0xfau, 8},        {0x16u, 6},        {0x17u, 6},        {0x18u, 6},
    {0x0u, 5},         {0x1u, 5},         {0x2u, 5},         {0x19u, 6},
    {0x1au, 6},        {0x1bu, 6},        {0x1cu, 6},        {0x1du, 6},
    {0x1eu, 6},        {0x1fu, 6},        {0x5cu, 7},        {0xfbu, 8},
    {0x7ffcu, 15},     {0x20u, 6},        {0xffbu, 12},      {0x3fcu, 10},
    {0x1ffau, 13},     {0x21u, 6},        {0x5du, 7},        {0x5eu, 7},
    {0x5fu, 7},        {0x60u, 7},        {0x61u, 7},        {0x62u, 7},
    {0x63u, 7},        {0x64u, 7},        {0x65u, 7},        {0x66u, 7},
    {0x67u, 7},        {0x68u, 7},        {0x69u, 7},        {0x6au, 7},
    {0x6bu, 7},        {0x6cu, 7},        {0x6du, 7},        {0x6eu, 7},
    {0x6fu, 7},        {0x70u, 7},        {0x71u, 7},        {0x72u, 7},
    {0xfcu, 8},        {0x73u, 7},        {0xfdu, 8},        {0x1ffbu, 13},
    {0x7fff0u, 19},    {0x1ffcu, 13},     {0x3ffcu, 14},     {0x22u, 6},
    {0x7ffdu, 15},     {0x3u, 5},         {0x23u, 6},        {0x4u, 5},
    {0x24u, 6},        {0x5u, 5},         {0x25u, 6},        {0x26u, 6},
    {0x27u, 6},        {0x6u, 5},         {0x74u, 7},        {0x75u, 7},
    {0x28u, 6},        {0x29u, 6},        {0x2au, 6},        {0x7u, 5},
    {0x2bu, 6},        {0x76u, 7},        {0x2cu, 6},        {0x8u, 5},
    {0x9u, 5},         {0x2du, 6},        {0x77u, 7},        {0x78u, 7},
    {0x79u, 7},        {0x7au, 7},        {0x7bu, 7},        {0x7ffeu, 15},
    {0x7fcu, 11},      {0x3ffdu, 14},     {0x1ffdu, 13},     {0xffffffcu, 28},
    {0xfffe6u, 20},    {0x3fffd2u, 22},   {0xfffe7u, 20},    {0xfffe8u, 20},
    {0x3fffd3u, 22},   {0x3fffd4u, 22},   {0x3fffd5u, 22},   {0x7fffd9u, 23},
    {0x3fffd6u, 22},   {0x7fffdau, 23},   {0x7fffdbu, 23},   {0x7fffdcu, 23},
    {0x7fffddu, 23},   {0x7fffdeu, 23},   {0xffffebu, 24},   {0x7fffdfu, 23},
    {0xffffecu, 24},   {0xffffedu, 24},   {0x3fffd7u, 22},   {0x7fffe0u, 23},
    {0xffffeeu, 24},   {0x7fffe1u, 23},   {0x7fffe2u, 23},   {0x7fffe3u, 23},
    {0x7fffe4u, 23},   {0x1fffdcu, 21},   {0x3fffd8u, 22},   {0x7fffe5u, 23},
    {0x3fffd9u, 22},   {0x7fffe6u, 23},   {0x7fffe7u, 23},   {0xffffefu, 24},
    {0x3fffdau, 22},   {0x1fffddu, 21},   {0xfffe9u, 20},    {0x3fffdbu, 22},
    {0x3fffdcu, 22},   {0x7fffe8u, 23},   {0x7fffe9u, 23},   {0x1fffdeu, 21},
    {0x7fffeau, 23},   {0x3fffddu, 22},   {0x3fffdeu, 22},   {0xfffff0u, 24},
    {0x1fffdfu, 21},   {0x3fffdfu, 22},   {0x7fffebu, 23},   {0x7fffecu, 23},
    {0x1fffe0u, 21},   {0x1fffe1u, 21},   {0x3fffe0u, 22},   {0x1fffe2u, 21},
    {0x7fffedu, 23},   {0x3fffe1u, 22},   {0x7fffeeu, 23},   {0x7fffefu, 23},
    {0xfffeau, 20},    {0x3fffe2u, 22},   {0x3fffe3u, 22},   {0x3fffe4u, 22},
    {0x7ffff0u, 23},   {0x3fffe5u, 22},   {0x3fffe6u, 22},   {0x7ffff1u, 23},
    {0x3ffffe0u, 26},  {0x3ffffe1u, 26},  {0xfffebu, 20},    {0x7fff1u, 19},
    {0x3fffe7u, 22},   {0x7ffff2u, 23},   {0x3fffe8u, 22},   {0x1ffffecu, 25},
    {0x3ffffe2u, 26},  {0x3ffffe3u, 26},  {0x3ffffe4u, 26},  {0x7ffffdeu, 27},
    {0x7ffffdfu, 27},  {0x3ffffe5u, 26},  {0xfffff1u, 24},   {0x1ffffedu, 25},
    {0x7fff2u, 19},    {0x1fffe3u, 21},   {0x3ffffe6u, 26},  {0x7ffffe0u, 27},
    {0x7ffffe1u, 27},  {0x3ffffe7u, 26},  {0x7ffffe2u, 27},  {0xfffff2u, 24},
    {0x1fffe4u, 21},   {0x1fffe5u, 21},   {0x3ffffe8u, 26},  {0x3ffffe9u, 26},
    {0xffffffdu, 28},  {0x7ffffe3u, 27},  {0x7ffffe4u, 27},  {0x7ffffe5u, 27},
    {0xfffecu, 20},    {0xfffff3u, 24},   {0xfffedu, 20},    {0x1fffe6u, 21},
    {0x3fffe9u, 22},   {0x1fffe7u, 21},   {0x1fffe8u, 21},   {0x7ffff3u, 23},
    {0x3fffeau, 22},   {0x3fffebu, 22},   {0x1ffffeeu, 25},  {0x1ffffefu, 25},
    {0xfffff4u, 24},   {0xfffff5u, 24},   {0x3ffffeau, 26},  {0x7ffff4u, 23},
    {0x3ffffebu, 26},  {0x7ffffe6u, 27},  {0x3ffffecu, 26},  {0x3ffffedu, 26},
    {0x7ffffe7u, 27},  {0x7ffffe8u, 27},  {0x7ffffe9u, 27},  {0x7ffffeau, 27},
    {0x7ffffebu, 27},  {0xffffffeu, 28},  {0x7ffffecu, 27},  {0x7ffffedu, 27},
    {0x7ffffeeu, 27},  {0x7ffffefu, 27},  {0x7fffff0u, 27},  {0x3ffffeeu, 26},
    {0x3fffffffu, 30},
}};

inline size_t huffman_encoded_size(std::string_view s) {
  size_t bit_count = 0;
  for (unsigned char ch : s) bit_count += HPACK_HUFFMAN_CODES[ch].bit_length;
  return (bit_count + 7) / 8;
}

inline void huffman_encode_append(std::vector<uint8_t>& out,
                                  std::string_view s) {
  uint64_t bit_buffer = 0;
  uint32_t bit_count = 0;

  for (unsigned char ch : s) {
    const auto& code = HPACK_HUFFMAN_CODES[ch];
    bit_buffer = (bit_buffer << code.bit_length) | code.bits;
    bit_count += code.bit_length;

    while (bit_count >= 8) {
      out.push_back(uint8_t(bit_buffer >> (bit_count - 8)));
      bit_count -= 8;
      if (bit_count == 0) {
        bit_buffer = 0;
      }
      else {
        bit_buffer &= ((uint64_t(1) << bit_count) - 1);
      }
    }
  }

  if (bit_count != 0) {
    bit_buffer = (bit_buffer << (8 - bit_count)) |
                 ((uint64_t(1) << (8 - bit_count)) - 1);
    out.push_back(uint8_t(bit_buffer));
  }
}

inline std::vector<uint8_t> huffman_encode(std::string_view s) {
  std::vector<uint8_t> out;
  out.reserve(huffman_encoded_size(s));
  huffman_encode_append(out, s);

  return out;
}

struct hpack_huffman_node {
  int16_t next[2]{-1, -1};
  int16_t sym{-1};
};

inline const std::vector<hpack_huffman_node>& huffman_decode_tree() {
  static const auto tree = [] {
    std::vector<hpack_huffman_node> nodes(1);

    for (size_t sym = 0; sym < HPACK_HUFFMAN_CODES.size(); ++sym) {
      const auto& code = HPACK_HUFFMAN_CODES[sym];
      int node = 0;
      for (uint8_t i = 0; i < code.bit_length; ++i) {
        uint32_t bit = (code.bits >> (code.bit_length - 1 - i)) & 1u;
        if (nodes[node].next[bit] == -1) {
          nodes[node].next[bit] = int16_t(nodes.size());
          nodes.push_back({});
        }
        node = nodes[node].next[bit];
      }
      nodes[node].sym = int16_t(sym);
    }

    return nodes;
  }();

  return tree;
}

inline std::string huffman_decode(std::span<const uint8_t> encoded) {
  const auto& tree = huffman_decode_tree();

  std::string out;
  out.reserve(encoded.size() * 2);

  int node = 0;
  uint32_t pending_bits = 0;
  uint32_t pending = 0;

  for (uint8_t byte : encoded) {
    for (int shift = 7; shift >= 0; --shift) {
      uint32_t bit = (byte >> shift) & 1u;
      pending = (pending << 1) | bit;
      ++pending_bits;

      node = tree[node].next[bit];
      if (node < 0)
        throw std::runtime_error("hpack: invalid Huffman code");

      if (tree[node].sym >= 0) {
        if (tree[node].sym == 256)
          throw std::runtime_error("hpack: EOS symbol is not allowed");
        out.push_back(static_cast<char>(tree[node].sym));
        node = 0;
        pending = 0;
        pending_bits = 0;
      }
    }
  }

  if (pending_bits != 0) {
    if (pending_bits > 7)
      throw std::runtime_error("hpack: invalid Huffman padding");
    if (pending != ((uint32_t(1) << pending_bits) - 1))
      throw std::runtime_error("hpack: invalid Huffman padding");
  }

  return out;
}

}  // namespace cinatra::http2
