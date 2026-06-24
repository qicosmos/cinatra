#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// HPACK - Header Compression for HTTP/2 (RFC 7541)
#include "hpack_huffman.hpp"

// static table + dynamic table + Huffman string coding.
namespace cinatra::http2 {

struct header_field {
  std::string name;
  std::string value;
};

struct header_field_view {
  std::string_view name;
  std::string_view value;
};

inline bool header_list_size_within_limit(std::span<const header_field> headers,
                                          uint32_t max_header_list_size) {
  uint64_t total = 0;
  for (auto& hf : headers) {
    total += static_cast<uint64_t>(hf.name.size()) +
             static_cast<uint64_t>(hf.value.size()) + 32;
    if (total > max_header_list_size)
      return false;
  }
  return true;
}

inline bool header_list_size_within_limit(
    std::span<const header_field_view> headers, uint32_t max_header_list_size) {
  uint64_t total = 0;
  for (auto& hf : headers) {
    total += static_cast<uint64_t>(hf.name.size()) +
             static_cast<uint64_t>(hf.value.size()) + 32;
    if (total > max_header_list_size)
      return false;
  }
  return true;
}

// --- Static table (RFC 7541 Appendix A) ---
// Index starts at 1; index 0 is unused (placeholder).
struct static_entry {
  std::string_view name;
  std::string_view value;
};

inline constexpr std::array<static_entry, 62> HPACK_STATIC_TABLE = {{
    {"", ""},                              // 0 - unused
    {":authority", ""},                    // 1
    {":method", "GET"},                    // 2
    {":method", "POST"},                   // 3
    {":path", "/"},                        // 4
    {":path", "/index.html"},              // 5
    {":scheme", "http"},                   // 6
    {":scheme", "https"},                  // 7
    {":status", "200"},                    // 8
    {":status", "204"},                    // 9
    {":status", "206"},                    // 10
    {":status", "304"},                    // 11
    {":status", "400"},                    // 12
    {":status", "404"},                    // 13
    {":status", "500"},                    // 14
    {"accept-charset", ""},                // 15
    {"accept-encoding", "gzip, deflate"},  // 16
    {"accept-language", ""},               // 17
    {"accept-ranges", ""},                 // 18
    {"accept", ""},                        // 19
    {"access-control-allow-origin", ""},   // 20
    {"age", ""},                           // 21
    {"allow", ""},                         // 22
    {"authorization", ""},                 // 23
    {"cache-control", ""},                 // 24
    {"content-disposition", ""},           // 25
    {"content-encoding", ""},              // 26
    {"content-language", ""},              // 27
    {"content-length", ""},                // 28
    {"content-location", ""},              // 29
    {"content-range", ""},                 // 30
    {"content-type", ""},                  // 31
    {"cookie", ""},                        // 32
    {"date", ""},                          // 33
    {"etag", ""},                          // 34
    {"expect", ""},                        // 35
    {"expires", ""},                       // 36
    {"from", ""},                          // 37
    {"host", ""},                          // 38
    {"if-match", ""},                      // 39
    {"if-modified-since", ""},             // 40
    {"if-none-match", ""},                 // 41
    {"if-range", ""},                      // 42
    {"if-unmodified-since", ""},           // 43
    {"last-modified", ""},                 // 44
    {"link", ""},                          // 45
    {"location", ""},                      // 46
    {"max-forwards", ""},                  // 47
    {"proxy-authenticate", ""},            // 48
    {"proxy-authorization", ""},           // 49
    {"range", ""},                         // 50
    {"referer", ""},                       // 51
    {"refresh", ""},                       // 52
    {"retry-after", ""},                   // 53
    {"server", ""},                        // 54
    {"set-cookie", ""},                    // 55
    {"strict-transport-security", ""},     // 56
    {"transfer-encoding", ""},             // 57
    {"user-agent", ""},                    // 58
    {"vary", ""},                          // 59
    {"via", ""},                           // 60
    {"www-authenticate", ""},              // 61
}};

constexpr size_t STATIC_TABLE_SIZE = 61;  // indices 1..61

// --- Dynamic table ---

class dynamic_table {
 public:
  explicit dynamic_table(size_t max_size = 4096) : max_size_(max_size) {}

  // Add entry at front (index 62 after static table)
  void add(std::string name, std::string value) {
    size_t entry_size =
        name.size() + value.size() + 32;  // RFC 7541 section 4.1
    // Evict oldest entries until there is room
    while (!entries_.empty() && current_size_ + entry_size > max_size_) {
      evict();
    }
    if (entry_size <= max_size_) {
      current_size_ += entry_size;
      entries_.push_front({std::move(name), std::move(value)});
    }
  }

  // Lookup by dynamic-table-local index (1-based within dynamic table)
  const header_field& at(size_t local_idx) const {
    return entries_.at(local_idx - 1);
  }

  size_t size() const { return entries_.size(); }

  // Search for a matching entry. Returns {1-based local index, value_matches}.
  // Returns {0, false} if no name match found.
  std::pair<size_t, bool> find(std::string_view name,
                               std::string_view value) const {
    size_t name_only = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].name == name) {
        if (entries_[i].value == value)
          return {i + 1, true};
        if (name_only == 0)
          name_only = i + 1;
      }
    }
    return {name_only, false};
  }

  void add(std::string_view name, std::string_view value) {
    add(std::string(name), std::string(value));
  }

  void set_max_size(size_t s) {
    max_size_ = s;
    while (!entries_.empty() && current_size_ > max_size_) {
      evict();
    }
  }

 private:
  void evict() {
    auto& back = entries_.back();
    current_size_ -= back.name.size() + back.value.size() + 32;
    entries_.pop_back();
  }

  std::deque<header_field> entries_;
  size_t max_size_;
  size_t current_size_ = 0;
};

// --- Integer coding (RFC 7541 section 5.1) ---

// Decode a variable-length integer with `prefix_bits` prefix bits.
// Advances `buf` past the consumed bytes.
// Returns UINT32_MAX on overflow/error.
inline uint32_t decode_integer(std::span<const uint8_t>& buf,
                               uint8_t prefix_bits) {
  if (buf.empty())
    return UINT32_MAX;
  uint8_t mask = uint8_t((1u << prefix_bits) - 1u);
  uint32_t value = buf[0] & mask;
  buf = buf.subspan(1);
  if (value < mask)
    return value;  // fits in prefix

  uint32_t m = 0;
  while (true) {
    if (buf.empty())
      return UINT32_MAX;
    uint8_t b = buf[0];
    buf = buf.subspan(1);
    value += uint32_t(b & 0x7f) << m;
    m += 7;
    if (!(b & 0x80))
      break;
    if (m > 28)
      return UINT32_MAX;  // overflow guard
  }
  return value;
}

// Encode integer with `prefix_bits` prefix bits into `out`.
// `prefix_high_bits` are the already-set high bits of the first byte.
inline void encode_integer(std::vector<uint8_t>& out, uint32_t value,
                           uint8_t prefix_bits, uint8_t prefix_high_bits = 0) {
  uint8_t mask = uint8_t((1u << prefix_bits) - 1u);
  if (value < mask) {
    out.push_back(uint8_t(prefix_high_bits | value));
    return;
  }
  out.push_back(uint8_t(prefix_high_bits | mask));
  value -= mask;
  while (value >= 0x80) {
    out.push_back(uint8_t((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(uint8_t(value));
}

// --- String coding (RFC 7541 section 5.2) ---

inline std::string decode_string(std::span<const uint8_t>& buf) {
  if (buf.empty())
    throw std::runtime_error("hpack: truncated string");
  bool huffman = (buf[0] & 0x80) != 0;
  uint32_t len = decode_integer(buf, 7);
  if (len > buf.size())
    throw std::runtime_error("hpack: string overrun");
  std::string s =
      huffman ? huffman_decode(buf.first(len))
              : std::string(reinterpret_cast<const char*>(buf.data()), len);
  buf = buf.subspan(len);
  return s;
}

inline void encode_string(std::vector<uint8_t>& out, std::string_view s) {
  auto encoded_size = huffman_encoded_size(s);
  if (encoded_size < s.size()) {
    encode_integer(out, uint32_t(encoded_size), 7, 0x80);
    out.reserve(out.size() + encoded_size);
    huffman_encode_append(out, s);
    return;
  }

  encode_integer(out, uint32_t(s.size()), 7, 0x00);
  if (!s.empty()) {
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(s.data()),
               reinterpret_cast<const uint8_t*>(s.data()) + s.size());
  }
}

// --- HPACK decoder ---

class hpack_decoder {
 public:
  explicit hpack_decoder(size_t max_dynamic_size = 4096)
      : dyn_(max_dynamic_size), max_dynamic_size_(max_dynamic_size) {}

  void set_max_dynamic_table_size(size_t max_dynamic_size) {
    max_dynamic_size_ = max_dynamic_size;
    dyn_.set_max_size(max_dynamic_size);
  }

  // Decode a complete header block fragment -> list of header fields.
  std::vector<header_field> decode(std::span<const uint8_t> block) {
    std::vector<header_field> headers;
    bool at_beginning = true;
    while (!block.empty()) {
      uint8_t first = block[0];

      if (first & 0x80) {
        // --- Indexed header field (RFC 7541 section 6.1) ---
        // 0b1xxxxxxx
        at_beginning = false;
        uint32_t idx = decode_integer(block, 7);
        headers.push_back(lookup(idx));
      }
      else if (first & 0x40) {
        // --- Literal with incremental indexing (RFC 7541 section 6.2.1) ---
        // 0b01xxxxxx
        at_beginning = false;
        uint32_t idx = decode_integer(block, 6);
        std::string name =
            idx ? std::string(lookup(idx).name) : decode_string(block);
        std::string value = decode_string(block);
        dyn_.add(name, value);
        headers.push_back({std::move(name), std::move(value)});
      }
      else if ((first & 0xe0) == 0x20) {
        // --- Dynamic table size update (RFC 7541 section 6.3) ---
        // 0b001xxxxx
        uint32_t new_size = decode_integer(block, 5);
        if (!at_beginning)
          throw std::runtime_error("hpack: table size update after headers");
        if (new_size > max_dynamic_size_)
          throw std::runtime_error("hpack: table size update exceeds limit");
        dyn_.set_max_size(new_size);
      }
      else {
        // --- Literal without indexing / never indexed (RFC 7541
        // section 6.2.2/6.2.3) 0b0000xxxx / 0b0001xxxx
        at_beginning = false;
        uint32_t idx = decode_integer(block, 4);
        std::string name =
            idx ? std::string(lookup(idx).name) : decode_string(block);
        std::string value = decode_string(block);
        headers.push_back({std::move(name), std::move(value)});
      }
    }
    return headers;
  }

 private:
  header_field lookup(uint32_t idx) const {
    if (idx == 0)
      throw std::runtime_error("hpack: index 0 is invalid");
    if (idx <= STATIC_TABLE_SIZE) {
      auto& e = HPACK_STATIC_TABLE[idx];
      return {std::string(e.name), std::string(e.value)};
    }
    size_t local = idx - STATIC_TABLE_SIZE;
    if (local > dyn_.size())
      throw std::runtime_error("hpack: dynamic table index out of range");
    return dyn_.at(local);
  }

  dynamic_table dyn_;
  size_t max_dynamic_size_ = 4096;
};

// --- HPACK encoder ---

class hpack_encoder {
 public:
  void set_max_dynamic_table_size(size_t max_dynamic_size) {
    pending_table_size_update_ = true;
    max_dynamic_size_ = max_dynamic_size;
    dyn_.set_max_size(max_dynamic_size);
  }

  // Encode headers -> header block bytes.
  // Strategy: indexed if found in static or dynamic table (name+value match),
  //           literal with incremental indexing otherwise.
  std::vector<uint8_t> encode(std::span<const header_field> headers) {
    return encode_impl(headers);
  }

  std::vector<uint8_t> encode(std::span<const header_field_view> headers) {
    return encode_impl(headers);
  }

 private:
  template <typename HeaderSpan>
  std::vector<uint8_t> encode_impl(HeaderSpan headers) {
    std::vector<uint8_t> out;
    out.reserve(estimate_header_block_size(headers) +
                (pending_table_size_update_ ? 8 : 0));
    if (pending_table_size_update_) {
      encode_integer(out, static_cast<uint32_t>(max_dynamic_size_), 5, 0x20);
      pending_table_size_update_ = false;
    }
    for (auto& hf : headers) {
      auto [idx, full_match] = table_lookup(hf.name, hf.value);
      if (full_match) {
        // Indexed header field (RFC 7541 section 6.1): 0b1xxxxxxx
        encode_integer(out, idx, 7, 0x80);
      }
      else if (idx != 0) {
        // Literal with incremental indexing, indexed name (section 6.2.1):
        // 0b01xxxxxx
        encode_integer(out, idx, 6, 0x40);
        encode_string(out, hf.value);
        dyn_.add(hf.name, hf.value);
      }
      else {
        // Literal with incremental indexing, new name (section 6.2.1):
        // 0b01000000
        out.push_back(0x40);
        encode_string(out, hf.name);
        encode_string(out, hf.value);
        dyn_.add(hf.name, hf.value);
      }
    }
    return out;
  }

  template <typename HeaderSpan>
  static size_t estimate_header_block_size(HeaderSpan headers) {
    size_t size = headers.size() * 3;
    for (auto& hf : headers) {
      size += hf.name.size() + hf.value.size();
    }
    return size;
  }

  // Returns {index, value_also_matches}. Searches both static and dynamic
  // tables (RFC 7541 section 2.3.3). Prefers a full match over a name-only
  // match.
  std::pair<uint32_t, bool> table_lookup(std::string_view name,
                                         std::string_view value) const {
    uint32_t name_only_idx = 0;
    // Static table: indices 1..61
    for (uint32_t i = 1; i <= STATIC_TABLE_SIZE; ++i) {
      auto& e = HPACK_STATIC_TABLE[i];
      if (e.name == name) {
        if (e.value == value)
          return {i, true};
        if (name_only_idx == 0)
          name_only_idx = i;
      }
    }
    // Dynamic table: indices STATIC_TABLE_SIZE+1 ..
    auto [dyn_local, dyn_full] = dyn_.find(name, value);
    if (dyn_full) {
      return {uint32_t(STATIC_TABLE_SIZE + dyn_local), true};
    }
    if (dyn_local != 0 && name_only_idx == 0) {
      name_only_idx = uint32_t(STATIC_TABLE_SIZE + dyn_local);
    }
    return {name_only_idx, false};
  }

  dynamic_table dyn_;
  size_t max_dynamic_size_ = 4096;
  bool pending_table_size_update_ = false;
};

}  // namespace cinatra::http2
