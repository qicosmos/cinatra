#ifndef _MULTIPART_READER_H_
#define _MULTIPART_READER_H_

#include "multipart_parser.hpp"
#include <map>
#include <string>
#include <utility>

namespace cinatra {
using multipart_headers = std::multimap<std::string, std::string>;

class multipart_reader {
public:
  using PartBeginCallback = std::function<void(const multipart_headers &)>;
  using PartDataCallback = std::function<void(const char *, size_t)>;
  using PartDataEnd = std::function<void()>;
  // typedef void (*PartBeginCallback)(const multipart_headers &headers);
  // typedef void (*PartDataCallback)(const char *buffer, size_t size);
  // typedef void (*Callback)();

  PartBeginCallback on_part_begin;
  PartDataCallback on_part_data;
  PartDataEnd on_part_end;
  PartDataEnd on_end;

  multipart_reader() {
    resetReaderCallbacks();
    setParserCallbacks();
  }

  void reset() { parser.reset(); }

  void set_boundary(std::string &&boundary) {
    parser.set_boundary(std::move(boundary)); // should add \r\n-- ?
  }

  size_t feed(const char *buffer, size_t len) {
    return parser.feed(buffer, len);
  }

  bool succeeded() const { return parser.succeeded(); }

  bool has_error() const { return parser.has_error(); }

  bool stopped() const { return parser.stopped(); }

  const char *get_error_message() const { return parser.get_error_message(); }

private:
  void resetReaderCallbacks() {
    on_part_begin = nullptr;
    on_part_data = nullptr;
    on_part_end = nullptr;
    on_end = nullptr;
  }

  void setParserCallbacks() {
    parser.onPartBegin = cbPartBegin;
    parser.onHeaderField = cbHeaderField;
    parser.onHeaderValue = cbHeaderValue;
    parser.onHeaderEnd = cbHeaderEnd;
    parser.onHeadersEnd = cbHeadersEnd;
    parser.onPartData = cbPartData;
    parser.onPartEnd = cbPartEnd;
    parser.onEnd = cbEnd;
    parser.userData = this;
  }

  static void cbPartBegin(const char *, size_t, size_t, void *) {
    // multipart_reader *self = (multipart_reader *)userData;
    // self->currentHeaders.clear();
  }

  static void cbHeaderField(const char *buffer, size_t start, size_t end,
                            void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    self->currentHeaderName += {buffer + start, end - start};
  }

  static void cbHeaderValue(const char *buffer, size_t start, size_t end,
                            void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    self->currentHeaderValue += {buffer + start, end - start};
  }

  static void cbHeaderEnd(const char *, size_t, size_t, void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    self->currentHeaders.emplace(self->currentHeaderName,
                                 self->currentHeaderValue);
    self->currentHeaderName.clear();
    self->currentHeaderValue.clear();
    // self->currentHeaders.emplace(std::string{ self->currentHeaderName.data(),
    // self->currentHeaderName.length() }, 	std::string{
    // self->currentHeaderValue.data(), self->currentHeaderValue.length() });
  }

  static void cbHeadersEnd(const char *, size_t, size_t, void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    if (self->on_part_begin != nullptr) {
      self->on_part_begin(self->currentHeaders);
    }
    self->currentHeaders.clear();
  }

  static void cbPartData(const char *buffer, size_t start, size_t end,
                         void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    if (self->on_part_data != nullptr) {
      self->on_part_data(buffer + start, end - start);
    }
  }

  static void cbPartEnd(const char *, size_t, size_t, void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    if (self->on_part_end != nullptr) {
      self->on_part_end();
    }
  }

  static void cbEnd(const char *, size_t, size_t, void *userData) {
    multipart_reader *self = (multipart_reader *)userData;
    if (self->on_end != nullptr) {
      self->on_end();
    }
  }

private:
  multipart_parser parser;
  multipart_headers currentHeaders;
  std::string currentHeaderName, currentHeaderValue;
  void *userData;
};
} // namespace cinatra
#endif /* _MULTIPART_READER_H_ */
