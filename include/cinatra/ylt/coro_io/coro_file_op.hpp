#pragma once
#include <cstdio>

#include "coro_io.hpp"
#ifdef __GNUC__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace coro_file_io {
struct file_result {
  int err = 0;
  bool eof = false;
  size_t size = 0;
};

inline FILE* fopen(const char* filename, const char* mode) {
  return std::fopen(filename, mode);
}

inline FILE* fopen(std::string_view filename, std::string_view mode) {
  return fopen(filename.data(), mode.data());
}

inline int fclose(FILE* file) { return std::fclose(file); }

inline std::shared_ptr<FILE> fopen_shared(const char* filename,
                                          const char* mode) {
  FILE* file_ptr = fopen(filename, mode);
  if (file_ptr == nullptr) {
    return nullptr;
  }

  return std::shared_ptr<FILE>(file_ptr, fclose);
}

inline std::shared_ptr<FILE> fopen_shared(std::string_view filename,
                                          std::string_view mode) {
  return fopen_shared(filename.data(), mode.data());
}

inline async_simple::coro::Lazy<file_result> async_op(auto io_func,
                                                      bool is_read,
                                                      FILE* stream, char* buf,
                                                      size_t size,
                                                      auto executor) {
  file_result result{};
  std::function<int()> func = [=] {
    return io_func(buf, sizeof(char), size, stream);
  };

  auto len_val = co_await coro_io::post(std::move(func), executor);
  result.size = len_val.value();
  result.err = ferror(stream);
  if (is_read) {
    result.eof = feof(stream);
  }
  else {
    result.eof = true;
  }

  co_return result;
}

inline async_simple::coro::Lazy<file_result> async_read(
    FILE* stream, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_op(fread, true, stream, buf, size, executor);
}

inline async_simple::coro::Lazy<file_result> async_op_at(auto io_func,
                                                         FILE* stream,
                                                         size_t offset,
                                                         char* buf, size_t size,
                                                         auto executor) {
  file_result result{};
  int ret = std::fseek(stream, offset, SEEK_CUR);
  if (ret != 0) {
    result.err = ret;
    co_return result;
  }

  co_return co_await io_func(stream, buf, size, executor);
}

inline async_simple::coro::Lazy<file_result> async_read_at(
    FILE* stream, size_t offset, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_op_at(async_read, stream, offset, buf, size,
                                 executor);
}

inline async_simple::coro::Lazy<file_result> async_write(
    FILE* stream, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_op(fwrite, false, stream, buf, size, executor);
}

inline async_simple::coro::Lazy<file_result> async_write_at(
    FILE* stream, size_t offset, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_op_at(async_write, stream, offset, buf, size,
                                 executor);
}

#ifdef __GNUC__
inline int open(const char* file, int mode) { return ::open(file, mode); }

inline int close(int fd) { return ::close(fd); }

inline int open(std::string_view file, int mode) {
  return ::open(file.data(), mode);
}

inline int fd_is_valid(int fd) {
  return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

inline std::shared_ptr<int> open_shared(const char* file, int mode) {
  int fd = open(file, mode);
  if (fd < 0) {
    return nullptr;
  }

  return std::shared_ptr<int>(new int(fd), [](int* ptr) {
    ::close(*ptr);
    delete ptr;
  });
}

inline std::shared_ptr<int> open_shared(std::string_view file, int mode) {
  return open_shared(file.data(), mode);
}

inline async_simple::coro::Lazy<file_result> async_prw(auto io_func,
                                                       bool is_read, int fd,
                                                       size_t offset, char* buf,
                                                       size_t size,
                                                       auto executor) {
  file_result result{};
  std::function<int()> func = [=] {
    return io_func(fd, buf, size, offset);
  };

  auto len_val = co_await coro_io::post(std::move(func), executor);
  int len = len_val.value();
  if (len == 0) {
    if (is_read) {
      result.eof = true;
    }
  }
  else if (len > 0) {
    result.size = len;
  }
  else {
    result.err = len;
  }

  co_return result;
}

inline async_simple::coro::Lazy<file_result> async_pread(
    int fd, size_t offset, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_prw(pread, true, fd, offset, buf, size, executor);
}

inline async_simple::coro::Lazy<file_result> async_pwrite(
    int fd, size_t offset, char* buf, size_t size,
    coro_io::ExecutorWrapper<>* executor =
        coro_io::get_global_block_executor()) {
  co_return co_await async_prw(pwrite, true, fd, offset, buf, size, executor);
}
#endif
}  // namespace coro_file_io