#pragma once
#include <string>
#include <string_view>

namespace cinatra {
class coro_http_response {
 public:
  void set_status(int status) { status_ = status; }
  void set_content(std::string content) { content_ = std::move(content); }
  std::string_view get_content() const { return content_; }
  int get_status() const { return status_; }

 private:
  int status_;
  std::string content_;
};
}  // namespace cinatra