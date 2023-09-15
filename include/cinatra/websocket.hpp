#pragma once
#include "request.hpp"
#include "sha1.hpp"
#include "utils.hpp"
#include "ws_define.h"

namespace cinatra {
class websocket {
public:
  bool is_upgrade(const request &req) {
    if (req.get_method() != "GET"sv)
      return false;

    auto h = req.get_header_value("connection");
    if (h.empty())
      return false;

    auto u = req.get_header_value("upgrade");
    if (u.empty())
      return false;

    if (!find_strIC(h, UPGRADE))
      return false;

    if (!iequal(u.data(), u.length(), WEBSOCKET.data()))
      return false;

    sec_ws_key_ = req.get_header_value("sec-websocket-key");
    if (sec_ws_key_.empty() || sec_ws_key_.size() != 24)
      return false;

    return true;
  }

  void upgrade_to_websocket(const request &req, response &res) {
    uint8_t sha1buf[20], key_src[60];
    char accept_key[29];

    std::memcpy(key_src, sec_ws_key_.data(), 24);
    std::memcpy(key_src + 24, ws_guid, 36);
    SHA1(key_src, sizeof(key_src), sha1buf);
    base64_encode(accept_key, sha1buf, sizeof(sha1buf), 0);

    res.set_status(status_type::switching_protocols);

    res.add_header("Upgrade", "WebSocket");
    res.add_header("Connection", "Upgrade");
    res.add_header("Sec-WebSocket-Accept", std::string(accept_key, 28));
    // res.add_header("content-length", "0");
    auto protocal_str = req.get_header_value("sec-websocket-protocol");
    if (!protocal_str.empty()) {
      res.add_header("Sec-WebSocket-Protocol",
                     {protocal_str.data(), protocal_str.length()});
    }
  }

  /*
  0               1               2               3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-------+-+-------------+-------------------------------+
  |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
  |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
  |N|V|V|V|       |S|             |   (if payload len==126/127)   |
  | |1|2|3|       |K|             |                               |
  +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
  |     Extended payload length continued, if payload len == 127  |
  + - - - - - - - - - - - - - - - +-------------------------------+
  |                               |Masking-key, if MASK set to 1  |
  +-------------------------------+-------------------------------+
  | Masking-key (continued)       |          Payload Data         |
  +-------------------------------- - - - - - - - - - - - - - - - +
  :                     Payload Data continued ...                :
  + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
  |                     Payload Data continued ...                |
  +---------------------------------------------------------------+
  opcode:
  *  %x0 denotes a continuation frame
  *  %x1 denotes a text frame
  *  %x2 denotes a binary frame
  *  %x3-7 are reserved for further non-control frames
  *  %x8 denotes a connection close
  *  %x9 denotes a ping
  *  %xA denotes a pong
  *  %xB-F are reserved for further control frames
  Payload length:  7 bits, 7+16 bits, or 7+64 bits
  Masking-key:  0 or 4 bytes
  */
  int parse_header(const char *buf, size_t size) {
    const unsigned char *inp = (const unsigned char *)(buf);

    msg_opcode_ = inp[0] & 0x0F;
    msg_fin_ = (inp[0] >> 7) & 0x01;
    unsigned char msg_masked = (inp[1] >> 7) & 0x01;

    int pos = 2;
    int length_field = inp[1] & (~0x80);

    left_header_len_ = 0;
    if (length_field <= 125) {
      payload_length_ = length_field;
    } else if (length_field == 126) // msglen is 16bit!
    {
      payload_length_ = ntohs(*(uint16_t *)&inp[2]); // (inp[2] << 8) + inp[3];
      pos += 2;
      left_header_len_ = MEDIUM_HEADER - size;
    } else if (length_field == 127) // msglen is 64bit!
    {
      payload_length_ = (size_t)be64toh(*(uint64_t *)&inp[2]);
      pos += 8;
      left_header_len_ = LONG_HEADER - size;
    } else {
      return -1;
    }

    if (msg_masked) {
      mask_ = *((unsigned int *)(inp + pos));
    }

    return left_header_len_ == 0 ? 0 : -2;
  }

  ws_frame_type parse_payload(const char *buf, size_t size,
                              std::string &outbuf) {
    const unsigned char *inp = (const unsigned char *)(buf);
    if (payload_length_ > size)
      return ws_frame_type::WS_INCOMPLETE_FRAME;

    if (payload_length_ > outbuf.size()) {
      outbuf.resize((size_t)payload_length_);
    }

    if (mask_ == 0) {
      memcpy(&outbuf[0], (void *)(inp), payload_length_);
    }else{
      // unmask data:
      for (size_t i = 0; i < payload_length_; i++) {
        outbuf[i] = inp[i] ^ ((unsigned char *)(&mask_))[i % 4];
      }
    }

    if (msg_opcode_ == 0x0)
      return (msg_fin_)
                 ? ws_frame_type::WS_TEXT_FRAME
                 : ws_frame_type::WS_INCOMPLETE_TEXT_FRAME; // continuation
                                                            // frame ?
    if (msg_opcode_ == 0x1)
      return (msg_fin_) ? ws_frame_type::WS_TEXT_FRAME
                        : ws_frame_type::WS_INCOMPLETE_TEXT_FRAME;
    if (msg_opcode_ == 0x2)
      return (msg_fin_) ? ws_frame_type::WS_BINARY_FRAME
                        : ws_frame_type::WS_INCOMPLETE_BINARY_FRAME;
    if (msg_opcode_ == 0x8)
      return ws_frame_type::WS_CLOSE_FRAME;
    if (msg_opcode_ == 0x9)
      return ws_frame_type::WS_PING_FRAME;
    if (msg_opcode_ == 0xA)
      return ws_frame_type::WS_PONG_FRAME;
    return ws_frame_type::WS_BINARY_FRAME;
  }

  std::string format_header(size_t length, opcode code) {
    size_t header_length = encode_header(length, code);
    return {msg_header_, header_length};
  }

  std::vector<asio_ns::const_buffer>
  format_message(const char *src, size_t length, opcode code) {
    size_t header_length = encode_header(length, code);
    return {asio_ns::buffer(msg_header_, header_length),
            asio_ns::buffer(src, length)};
  }

  close_frame parse_close_payload(char *src, size_t length) {
    close_frame cf = {};
    if (length >= 2) {
      std::memcpy(&cf.code, src, 2);
      cf = {ntohs(cf.code), src + 2, length - 2};
      if (cf.code < 1000 || cf.code > 4999 ||
          (cf.code > 1011 && cf.code < 4000) ||
          (cf.code >= 1004 && cf.code <= 1006) ||
          !is_valid_utf8((unsigned char *)cf.message, cf.length)) {
        return {};
      }
    }
    return cf;
  }

  std::string format_close_payload(uint16_t code, char *message,
                                   size_t length) {
    std::string close_payload;
    if (code) {
      close_payload.resize(length + 2);
      code = htons(code);
      std::memcpy(close_payload.data(), &code, 2);
      std::memcpy(close_payload.data() + 2, message, length);
    }
    return close_payload;
  }

  size_t left_header_len() const { return left_header_len_; }

  size_t payload_length() const { return payload_length_; }

  opcode get_opcode() { return (opcode)msg_opcode_; }

private:
  size_t encode_header(size_t length, opcode code) {
    size_t header_length;

    if (length < 126) {
      header_length = 2;
      msg_header_[1] = static_cast<char>(length);
    } else if (length <= UINT16_MAX) {
      header_length = 4;
      msg_header_[1] = 126;
      *((uint16_t *)&msg_header_[2]) = htons(static_cast<uint16_t>(length));
    } else {
      header_length = 10;
      msg_header_[1] = 127;
      *((uint64_t *)&msg_header_[2]) = htobe64(length);
    }

    int flags = 0;
    msg_header_[0] = (flags & SND_NO_FIN ? 0 : 128);
    if (!(flags & SND_CONTINUATION)) {
      msg_header_[0] |= code;
    }

    return header_length;
  }

  void SHA1(uint8_t *key_src, size_t size, uint8_t *sha1buf) {
    sha1_context ctx;
    init(ctx);
    update(ctx, key_src, size);
    finish(ctx, sha1buf);
  }

  std::string_view sec_ws_key_;

  size_t payload_length_ = 0;
  size_t left_payload_length_ = 0;

  size_t left_header_len_ = 0;
  unsigned int mask_ = 0;
  unsigned char msg_opcode_ = 0;
  unsigned char msg_fin_ = 0;

  char msg_header_[10];
};

} // namespace cinatra
