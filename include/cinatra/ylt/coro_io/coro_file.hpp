/*
 * Copyright (c) 2023, Alibaba Group Holding Limited;
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <async_simple/Promise.h>
#include <async_simple/Traits.h>
#include <async_simple/coro/FutureAwaiter.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>

#include "async_simple/coro/SyncAwait.h"
#include "io_context_pool.hpp"
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
#include <asio/random_access_file.hpp>
#include <asio/stream_file.hpp>
#endif
#include <async_simple/coro/Lazy.h>

#include <asio/error.hpp>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro_io.hpp"

#if defined(ASIO_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif

namespace coro_io {

/*
              ┌─────────────┬───────────────────────────────┐
              │fopen() mode │ open() flags                  │
              ├─────────────┼───────────────────────────────┤
              │     r       │ O_RDONLY                      │
              ├─────────────┼───────────────────────────────┤
              │     w       │ O_WRONLY | O_CREAT | O_TRUNC  │
              ├─────────────┼───────────────────────────────┤
              │     a       │ O_WRONLY | O_CREAT | O_APPEND │
              ├─────────────┼───────────────────────────────┤
              │     r+      │ O_RDWR                        │
              ├─────────────┼───────────────────────────────┤
              │     w+      │ O_RDWR | O_CREAT | O_TRUNC    │
              ├─────────────┼───────────────────────────────┤
              │     a+      │ O_RDWR | O_CREAT | O_APPEND   │
              └─────────────┴───────────────────────────────┘
*/
enum flags {
#if defined(ASIO_WINDOWS)
  read_only = 1,
  write_only = 2,
  read_write = 4,
  append = 8,
  create = 16,
  exclusive = 32,
  truncate = 64,
  create_write = create | write_only,
  create_write_trunc = create | write_only | truncate,
  create_read_write_trunc = read_write | create | truncate,
  create_read_write_append = read_write | create | append,
  sync_all_on_write = 128
#else   // defined(ASIO_WINDOWS)
  read_only = O_RDONLY,
  write_only = O_WRONLY,
  read_write = O_RDWR,
  append = O_APPEND,
  create = O_CREAT,
  exclusive = O_EXCL,
  truncate = O_TRUNC,
  create_write = O_CREAT | O_WRONLY,
  create_write_trunc = O_WRONLY | O_CREAT | O_TRUNC,
  create_read_write_trunc = O_RDWR | O_CREAT | O_TRUNC,
  create_read_write_append = O_RDWR | O_CREAT | O_APPEND,
  sync_all_on_write = O_SYNC
#endif  // defined(ASIO_WINDOWS)
};

enum class read_type {
  init,
#if defined(ENABLE_FILE_IO_URING)
  uring,
  uring_random,
#else
  fread,
#endif
  pread,
};

enum class read_mode { seq, random };

enum class async_mode { native_async, thread_pool };

constexpr flags to_flags(std::ios::ios_base::openmode mode) {
  flags access = flags::read_write;

  if ((mode & (std::ios::app)) != 0)
    access = flags::append;

  if ((mode & (std::ios::trunc)) != 0)
    access = flags::truncate;

  if ((mode & (std::ios::in | std::ios::out)) != 0)
    access = (flags)(flags::create_write | flags::read_write);
  else if ((mode & std::ios::out) != 0)
    access = flags::create_write;
  else if ((mode & std::ios::in) != 0)
    access = flags::read_only;

  return access;
}

#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
template <bool seq, typename File, typename Executor>
inline bool open_native_async_file(File &file, Executor &executor,
                                   std::string_view filepath,
                                   flags open_flags) {
  if (file && file->is_open()) {
    return true;
  }

  try {
    if constexpr (seq) {
      file = std::make_shared<asio::stream_file>(
          executor.get_asio_executor(), std::string(filepath),
          static_cast<asio::file_base::flags>(open_flags));
    }
    else {
      file = std::make_shared<asio::random_access_file>(
          executor.get_asio_executor(), std::string(filepath),
          static_cast<asio::file_base::flags>(open_flags));
    }
  } catch (std::exception &ex) {
    std::cout << "line " << __LINE__ << " coro_file open failed" << ex.what()
              << "\n";
    return false;
  }

  return true;
}
#endif

enum class execution_type { none, native_async, thread_pool };

template <execution_type execute_type = execution_type::native_async>
class basic_seq_coro_file {
 public:
  basic_seq_coro_file(coro_io::ExecutorWrapper<> *executor =
                          coro_io::get_global_block_executor())
      : basic_seq_coro_file(executor->get_asio_executor()) {}

  basic_seq_coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}

  basic_seq_coro_file(std::string_view filepath,
                      std::ios::ios_base::openmode open_flags,
                      coro_io::ExecutorWrapper<> *executor =
                          coro_io::get_global_block_executor())
      : basic_seq_coro_file(filepath, open_flags,
                            executor->get_asio_executor()) {}

  basic_seq_coro_file(std::string_view filepath,
                      std::ios::ios_base::openmode open_flags,
                      asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {
    open(filepath, open_flags);
  }

  bool open(std::string_view filepath,
            std::ios::ios_base::openmode open_flags) {
    if constexpr (execute_type == execution_type::thread_pool) {
      return open_stream_file_in_pool(filepath, open_flags);
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      return open_native_async_file<true>(async_seq_file_, executor_wrapper_,
                                          filepath, to_flags(open_flags));
#else
      return open_stream_file_in_pool(filepath, open_flags);
#endif
    }
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      char *buf, size_t size) {
    if constexpr (execute_type == execution_type::thread_pool) {
      co_return co_await async_read_write({buf, size});
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      if (async_seq_file_ == nullptr) {
        co_return std::make_pair(
            std::make_error_code(std::errc::invalid_argument), 0);
      }
      auto [ec, read_size] = co_await coro_io::async_read(
          *async_seq_file_, asio::buffer(buf, size));
      if (ec == asio::error::eof) {
        eof_ = true;
        co_return std::make_pair(std::error_code{}, read_size);
      }

      co_return std::make_pair(ec, read_size);
#else
      co_return co_await async_read_write({buf, size});
#endif
    }
  }

  template <bool is_read = true>
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_write(
      std::span<char> buf) {
    auto result = co_await coro_io::post(
        [this, buf]() -> std::pair<std::error_code, size_t> {
          if constexpr (is_read) {
            if (frw_seq_file_.read(buf.data(), buf.size())) {
              return std::make_pair(std::error_code{}, frw_seq_file_.gcount());
            }
          }
          else {
            if (frw_seq_file_.write(buf.data(), buf.size())) {
              return std::make_pair(std::error_code{}, buf.size());
            }
          }

          if (frw_seq_file_.eof()) {
            eof_ = true;
            return std::make_pair(std::error_code{}, frw_seq_file_.gcount());
          }

          return std::make_pair(std::make_error_code(std::errc::io_error), 0);
        },
        &executor_wrapper_);

    co_return result.value();
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_write(
      std::string_view buf) {
    if constexpr (execute_type == execution_type::thread_pool) {
      co_return co_await async_read_write<false>(
          std::span(const_cast<char *>(buf.data()), buf.size()));
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      if (async_seq_file_ == nullptr) {
        co_return std::make_pair(
            std::make_error_code(std::errc::invalid_argument), 0);
      }
      auto [ec, size] =
          co_await coro_io::async_write(*async_seq_file_, asio::buffer(buf));
      co_return std::make_pair(ec, size);
#else
      co_return co_await async_read_write<false>(
          std::span(const_cast<char *>(buf.data()), buf.size()));
#endif
    }
  }

#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
  std::shared_ptr<asio::stream_file> get_async_stream_file() {
    return async_seq_file_;
  }
#endif

  std::fstream &get_stream_file() { return frw_seq_file_; }

  bool is_open() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    if (async_seq_file_ && async_seq_file_->is_open()) {
      return true;
    }
#endif
    return frw_seq_file_.is_open();
  }

  bool eof() { return eof_; }

  void close() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    if (async_seq_file_ && async_seq_file_->is_open()) {
      std::error_code ec;
      async_seq_file_->close(ec);
    }
#endif
    if (frw_seq_file_.is_open()) {
      frw_seq_file_.close();
    }
  }

  execution_type get_execution_type() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    if (async_seq_file_ && async_seq_file_->is_open()) {
      return execution_type::native_async;
    }
#endif
    if (frw_seq_file_.is_open()) {
      return execution_type::thread_pool;
    }

    return execution_type::none;
  }

 private:
  bool open_stream_file_in_pool(std::string_view filepath,
                                std::ios::ios_base::openmode flags) {
    if (frw_seq_file_.is_open()) {
      return true;
    }
    auto coro_func = coro_io::post(
        [this, flags, filepath] {
          frw_seq_file_.open(filepath.data(), flags);
          if (!frw_seq_file_.is_open()) {
            std::cout << "line " << __LINE__ << " coro_file open failed "
                      << filepath << "\n";
            return false;
          }
          return true;
        },
        &executor_wrapper_);
    auto result = async_simple::coro::syncAwait(coro_func);
    return result.value();
  }

  coro_io::ExecutorWrapper<> executor_wrapper_;
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
  std::shared_ptr<asio::stream_file> async_seq_file_;  // seq
#endif
  std::fstream frw_seq_file_;  // fread/fwrite seq file
  bool eof_ = false;
};

using coro_file0 = basic_seq_coro_file<>;

template <execution_type execute_type = execution_type::native_async>
class basic_random_coro_file {
 public:
  basic_random_coro_file(coro_io::ExecutorWrapper<> *executor =
                             coro_io::get_global_block_executor())
      : basic_random_coro_file(executor->get_asio_executor()) {}

  basic_random_coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}

  basic_random_coro_file(std::string_view filepath,
                         std::ios::ios_base::openmode open_flags,
                         coro_io::ExecutorWrapper<> *executor =
                             coro_io::get_global_block_executor())
      : basic_random_coro_file(filepath, open_flags,
                               executor->get_asio_executor()) {}

  basic_random_coro_file(std::string_view filepath,
                         std::ios::ios_base::openmode open_flags,
                         asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {
    open(filepath, open_flags);
  }

  bool open(std::string_view filepath,
            std::ios::ios_base::openmode open_flags) {
    if constexpr (execute_type == execution_type::thread_pool) {
      return open_fd(filepath, to_flags(open_flags));
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      return open_native_async_file<false>(async_random_file_,
                                           executor_wrapper_, filepath,
                                           to_flags(open_flags));
#else
      return open_fd(filepath, to_flags(open_flags));
#endif
    }
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_at(
      uint64_t offset, char *buf, size_t size) {
    if constexpr (execute_type == execution_type::thread_pool) {
      co_return co_await async_pread(offset, buf, size);
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      if (async_random_file_ == nullptr) {
        co_return std::make_pair(
            std::make_error_code(std::errc::invalid_argument), 0);
      }
      auto [ec, read_size] = co_await coro_io::async_read_at(
          offset, *async_random_file_, asio::buffer(buf, size));

      if (ec == asio::error::eof) {
        eof_ = true;
        co_return std::make_pair(std::error_code{}, read_size);
      }

      co_return std::make_pair(ec, read_size);
#else
      co_return co_await async_pread(offset, buf, size);
#endif
    }
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_write_at(
      uint64_t offset, std::string_view buf) {
    if constexpr (execute_type == execution_type::thread_pool) {
      co_return co_await async_pwrite(offset, buf.data(), buf.size());
    }
    else {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
      if (async_random_file_ == nullptr) {
        co_return std::make_pair(
            std::make_error_code(std::errc::invalid_argument), 0);
      }
      auto [ec, write_size] = co_await coro_io::async_write_at(
          offset, *async_random_file_, asio::buffer(buf));

      co_return std::make_pair(ec, write_size);
#else
      co_return co_await async_pwrite(offset, buf.data(), buf.size());
#endif
    }
  }

#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
  std::shared_ptr<asio::random_access_file> get_async_stream_file() {
    return async_random_file_;
  }
#endif

  std::shared_ptr<int> get_pread_file() { return prw_random_file_; }

  bool is_open() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    if (async_random_file_ && async_random_file_->is_open()) {
      return true;
    }
#endif
    return prw_random_file_ != nullptr;
  }

  bool eof() { return eof_; }

  execution_type get_execution_type() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    if (async_random_file_ && async_random_file_->is_open()) {
      return execution_type::native_async;
    }
#endif
    if (prw_random_file_ != nullptr) {
      return execution_type::thread_pool;
    }

    return execution_type::none;
  }

  void close() {
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
    std::error_code ec;
    if (async_random_file_) {
      async_random_file_->close(ec);
    }
#endif
    prw_random_file_ = nullptr;
  }

 private:
  bool open_fd(std::string_view filepath, int open_flags) {
    if (prw_random_file_) {
      return true;
    }

#if defined(ASIO_WINDOWS)
    int fd = _open(filepath.data(), adjust_flags(open_flags));
#else
    int fd = ::open(filepath.data(), open_flags);
#endif
    if (fd < 0) {
      return false;
    }

    prw_random_file_ = std::shared_ptr<int>(new int(fd), [](int *ptr) {
#if defined(ASIO_WINDOWS)
      _close(*ptr);
#else
      ::close(*ptr);
#endif
      delete ptr;
    });
    return true;
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_pread(
      size_t offset, char *data, size_t size) {
#if defined(ASIO_WINDOWS)
    auto pread = [](int fd, void *buf, uint64_t count,
                    uint64_t offset) -> int64_t {
      DWORD bytes_read = 0;
      OVERLAPPED overlapped;
      memset(&overlapped, 0, sizeof(OVERLAPPED));
      overlapped.Offset = offset & 0xFFFFFFFF;
      overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

      BOOL ok = ReadFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), buf,
                         count, &bytes_read, &overlapped);
      if (!ok && (errno = GetLastError()) != ERROR_HANDLE_EOF) {
        return -1;
      }

      return bytes_read;
    };
#endif
    co_return co_await async_prw(pread, true, offset, data, size);
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_pwrite(
      size_t offset, const char *data, size_t size) {
#if defined(ASIO_WINDOWS)
    auto pwrite = [](int fd, const void *buf, uint64_t count,
                     uint64_t offset) -> int64_t {
      DWORD bytes_write = 0;
      OVERLAPPED overlapped;
      memset(&overlapped, 0, sizeof(OVERLAPPED));
      overlapped.Offset = offset & 0xFFFFFFFF;
      overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

      BOOL ok = WriteFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), buf,
                          count, &bytes_write, &overlapped);
      if (!ok) {
        return -1;
      }

      return bytes_write;
    };
#endif
    co_return co_await async_prw(pwrite, false, offset, (char *)data, size);
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_prw(
      auto io_func, bool is_read, size_t offset, char *buf, size_t size) {
    std::function<int()> func = [=, this] {
      int fd = *prw_random_file_;
      return io_func(fd, buf, size, offset);
    };

    std::error_code ec{};
    size_t op_size = 0;

    auto len_val = co_await coro_io::post(std::move(func), &executor_wrapper_);
    int len = len_val.value();
    if (len == 0) {
      if (is_read) {
        eof_ = true;
      }
    }
    else if (len > 0) {
      op_size = len;
    }
    else {
      ec = std::make_error_code(std::errc::io_error);
      op_size = len;
    }

    co_return std::make_pair(ec, op_size);
  }

#if defined(ASIO_WINDOWS)
  static int adjust_flags(int open_mode) {
    switch (open_mode) {
      case flags::read_only:
        return _O_RDONLY;
      case flags::write_only:
        return _O_WRONLY;
      case flags::read_write:
        return _O_RDWR;
      case flags::append:
        return _O_APPEND;
      case flags::create:
        return _O_CREAT;
      case flags::exclusive:
        return _O_EXCL;
      case flags::truncate:
        return _O_TRUNC;
      case flags::create_write:
        return _O_CREAT | _O_WRONLY;
      case flags::create_write_trunc:
        return _O_CREAT | _O_WRONLY | _O_TRUNC;
      case flags::create_read_write_trunc:
        return _O_RDWR | _O_CREAT | _O_TRUNC;
      case flags::create_read_write_append:
        return _O_RDWR | _O_CREAT | _O_APPEND;
      case flags::sync_all_on_write:
      default:
        return open_mode;
        break;
    }
    return open_mode;
  }
#endif

  coro_io::ExecutorWrapper<> executor_wrapper_;
#if defined(ENABLE_FILE_IO_URING) || defined(ASIO_WINDOWS)
  std::shared_ptr<asio::random_access_file> async_random_file_;  // random file
#endif
  std::shared_ptr<int> prw_random_file_ = nullptr;  // pread/pwrite random file
  bool eof_ = false;
};

using random_coro_file = basic_random_coro_file<>;

class coro_file {
 public:
#if defined(ENABLE_FILE_IO_URING)
  coro_file(coro_io::ExecutorWrapper<> *executor =
                coro_io::get_global_block_executor())
      : coro_file(executor->get_asio_executor()) {}

  coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}
#else

  coro_file(coro_io::ExecutorWrapper<> *executor =
                coro_io::get_global_block_executor())
      : coro_file(executor->get_asio_executor()) {}

  coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}
#endif

  bool is_open() const {
    if (type_ == read_type::pread) {
      return fd_file_ != nullptr;
    }

    return stream_file_ != nullptr;
  }

  void flush() {
#if defined(ENABLE_FILE_IO_URING)

#else
    if (fd_file_) {
#if defined(__GNUC__)
      fsync(*fd_file_);
#endif
    }
    else if (stream_file_) {
      fflush(stream_file_.get());
    }
#endif
  }

  bool eof() const { return eof_; }

  void close() {
    if (stream_file_) {
      stream_file_.reset();
    }
    else if (fd_file_) {
      fd_file_.reset();
    }
  }

  size_t file_size(std::error_code ec) const noexcept {
    return std::filesystem::file_size(file_path_, ec);
  }

  size_t file_size() const { return std::filesystem::file_size(file_path_); }

  std::string_view file_path() const { return file_path_; }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_pread(
      size_t offset, char *data, size_t size) {
    if (type_ != read_type::pread) {
      co_return std::make_pair(
          std::make_error_code(std::errc::bad_file_descriptor), 0);
    }
#if defined(ASIO_WINDOWS)
    auto pread = [](int fd, void *buf, uint64_t count,
                    uint64_t offset) -> int64_t {
      DWORD bytes_read = 0;
      OVERLAPPED overlapped;
      memset(&overlapped, 0, sizeof(OVERLAPPED));
      overlapped.Offset = offset & 0xFFFFFFFF;
      overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

      BOOL ok = ReadFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), buf,
                         count, &bytes_read, &overlapped);
      if (!ok && (errno = GetLastError()) != ERROR_HANDLE_EOF) {
        return -1;
      }

      return bytes_read;
    };
#endif
    co_return co_await async_prw(pread, true, offset, data, size);
  }

  async_simple::coro::Lazy<std::error_code> async_pwrite(size_t offset,
                                                         const char *data,
                                                         size_t size) {
    if (type_ != read_type::pread) {
      co_return std::make_error_code(std::errc::bad_file_descriptor);
    }
#if defined(ASIO_WINDOWS)
    auto pwrite = [](int fd, const void *buf, uint64_t count,
                     uint64_t offset) -> int64_t {
      DWORD bytes_write = 0;
      OVERLAPPED overlapped;
      memset(&overlapped, 0, sizeof(OVERLAPPED));
      overlapped.Offset = offset & 0xFFFFFFFF;
      overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

      BOOL ok = WriteFile(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), buf,
                          count, &bytes_write, &overlapped);
      if (!ok) {
        return -1;
      }

      return bytes_write;
    };
#endif
    auto result = co_await async_prw(pwrite, false, offset, (char *)data, size);
    co_return result.first;
  }

#if defined(ENABLE_FILE_IO_URING)
  async_simple::coro::Lazy<bool> async_open(std::string_view filepath,
                                            int open_mode = flags::read_write,
                                            read_type type = read_type::uring) {
    type_ = type;
    if (type_ == read_type::pread) {
      co_return open_fd(filepath, open_mode);
    }

    try {
      if (type_ == read_type::uring) {
        stream_file_ = std::make_shared<asio::stream_file>(
            executor_wrapper_.get_asio_executor());
      }
      else {
        stream_file_ = std::make_shared<asio::random_access_file>(
            executor_wrapper_.get_asio_executor());
      }
    } catch (std::exception &ex) {
      stream_file_ = nullptr;
      std::cout << "line " << __LINE__ << " coro_file create failed"
                << ex.what() << "\n";
      co_return false;
    }

    std::error_code ec;
    stream_file_->open(filepath.data(),
                       static_cast<asio::file_base::flags>(open_mode), ec);

    if (ec) {
      stream_file_ = nullptr;
      std::cout << "line " << __LINE__ << " coro_file open failed"
                << ec.message() << "\n";
      co_return false;
    }

    co_return true;
  }

  bool seek(long offset, int whence) {
    if (type_ != read_type::uring) {
      return false;
    }

    std::error_code seek_ec;
    reinterpret_cast<asio::stream_file *>(stream_file_.get())
        ->seek(offset, static_cast<asio::file_base::seek_basis>(whence),
               seek_ec);
    if (seek_ec) {
      return false;
    }
    return true;
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read_at(
      uint64_t offset, char *data, size_t size) {
    if (type_ != read_type::uring_random) {
      co_return std::make_pair(
          std::make_error_code(std::errc::bad_file_descriptor), 0);
    }

    auto [ec, read_size] = co_await coro_io::async_read_at(
        offset,
        *reinterpret_cast<asio::random_access_file *>(stream_file_.get()),
        asio::buffer(data, size));

    if (ec == asio::error::eof) {
      eof_ = true;
      co_return std::make_pair(std::error_code{}, read_size);
    }

    co_return std::make_pair(std::error_code{}, read_size);
  }

  async_simple::coro::Lazy<std::error_code> async_write_at(uint64_t offset,
                                                           const char *data,
                                                           size_t size) {
    if (type_ != read_type::uring_random) {
      co_return std::make_error_code(std::errc::bad_file_descriptor);
    }

    auto [ec, write_size] = co_await coro_io::async_write_at(
        offset,
        *reinterpret_cast<asio::random_access_file *>(stream_file_.get()),
        asio::buffer(data, size));
    co_return ec;
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      char *data, size_t size) {
    if (type_ != read_type::uring) {
      co_return std::make_pair(
          std::make_error_code(std::errc::bad_file_descriptor), 0);
    }

    auto [ec, read_size] = co_await coro_io::async_read(
        *reinterpret_cast<asio::stream_file *>(stream_file_.get()),
        asio::buffer(data, size));
    if (ec == asio::error::eof) {
      eof_ = true;
      co_return std::make_pair(std::error_code{}, read_size);
    }

    co_return std::make_pair(std::error_code{}, read_size);
  }

  async_simple::coro::Lazy<std::error_code> async_write(const char *data,
                                                        size_t size) {
    if (type_ != read_type::uring) {
      co_return std::make_error_code(std::errc::bad_file_descriptor);
    }

    auto [ec, write_size] = co_await coro_io::async_write(
        *reinterpret_cast<asio::stream_file *>(stream_file_.get()),
        asio::buffer(data, size));

    co_return ec;
  }
#else
  std::string str_mode(int open_mode) {
    switch (open_mode) {
      case flags::read_only:
        return "rb";
      case flags::create_write:
      case flags::write_only:
        return "wb+";
      case flags::read_write:
        return "rb+";
      case flags::append:
        return "ab+";
      case flags::create_read_write_append:
        return "ab+";
      case flags::truncate:
        return "w+";
      default:
        return "rb+";
    }
  }

  bool seek(long offset, int whence) {
    if (stream_file_ == nullptr) {
      return false;
    }

    return fseek(stream_file_.get(), offset, whence) == 0;
  }

  async_simple::coro::Lazy<bool> async_open(std::string filepath,
                                            int open_mode = flags::read_write,
                                            read_type type = read_type::fread) {
    file_path_ = std::move(filepath);
    type_ = type;
    if (type_ == read_type::pread) {
      co_return open_fd(file_path_, open_mode);
    }

    if (stream_file_ != nullptr) {
      co_return true;
    }

    auto result = co_await coro_io::post(
        [this, open_mode] {
          auto fptr =
              fopen(this->file_path_.data(), str_mode(open_mode).data());
          if (fptr == nullptr) {
            std::cout << "line " << __LINE__ << " coro_file open failed "
                      << this->file_path_ << "\n";
            return false;
          }
          stream_file_ = std::shared_ptr<FILE>(fptr, [](FILE *ptr) {
            fclose(ptr);
          });
          return true;
        },
        &executor_wrapper_);
    co_return result.value();
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      char *data, size_t size) {
    if (type_ != read_type::fread) {
      co_return std::make_pair(
          std::make_error_code(std::errc::bad_file_descriptor), 0);
    }
    auto result = co_await coro_io::post(
        [this, data, size] {
          auto fptr = stream_file_.get();
          size_t read_size = fread(data, sizeof(char), size, fptr);
          if (ferror(fptr)) {
            return std::pair<std::error_code, size_t>(
                std::make_error_code(std::errc::io_error), 0);
          }
          eof_ = feof(fptr);
          return std::pair<std::error_code, size_t>(std::error_code{},
                                                    read_size);
        },
        &executor_wrapper_);

    co_return result.value();
  }

  async_simple::coro::Lazy<std::error_code> async_write(const char *data,
                                                        size_t size) {
    if (type_ != read_type::fread) {
      co_return std::make_error_code(std::errc::bad_file_descriptor);
    }
    auto result = co_await coro_io::post(
        [this, data, size] {
          auto fptr = stream_file_.get();
          fwrite(data, sizeof(char), size, fptr);
          if (ferror(fptr)) {
            return std::make_error_code(std::errc::io_error);
          }
          return std::error_code{};
        },
        &executor_wrapper_);

    co_return result.value();
  }
#endif

 private:
  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_prw(
      auto io_func, bool is_read, size_t offset, char *buf, size_t size) {
    std::function<int()> func = [=, this] {
      int fd = *fd_file_;
      return io_func(fd, buf, size, offset);
    };

    std::error_code ec{};
    size_t op_size = 0;

    auto len_val = co_await coro_io::post(std::move(func), &executor_wrapper_);
    int len = len_val.value();
    if (len == 0) {
      if (is_read) {
        eof_ = true;
      }
    }
    else if (len > 0) {
      op_size = len;
    }
    else {
      ec = std::make_error_code(std::errc::io_error);
    }

    co_return std::make_pair(ec, op_size);
  }

  bool open_fd(std::string_view filepath, int open_mode = flags::read_write) {
    if (fd_file_) {
      return true;
    }

#if defined(ASIO_WINDOWS)
    int fd = _open(filepath.data(), adjust_open_mode(open_mode));
#else
    int fd = open(filepath.data(), open_mode);
#endif
    if (fd < 0) {
      return false;
    }

    fd_file_ = std::shared_ptr<int>(new int(fd), [](int *ptr) {
#if defined(ASIO_WINDOWS)
      _close(*ptr);
#else
      ::close(*ptr);
#endif
      delete ptr;
    });
    return true;
  }

#if defined(ASIO_WINDOWS)
  static int adjust_open_mode(int open_mode) {
    switch (open_mode) {
      case flags::read_only:
        return _O_RDONLY;
      case flags::write_only:
        return _O_WRONLY;
      case flags::read_write:
        return _O_RDWR;
      case flags::append:
        return _O_APPEND;
      case flags::create:
        return _O_CREAT;
      case flags::exclusive:
        return _O_EXCL;
      case flags::truncate:
        return _O_TRUNC;
      case flags::create_write:
        return _O_CREAT | _O_WRONLY;
      case flags::create_write_trunc:
        return _O_CREAT | _O_WRONLY | _O_TRUNC;
      case flags::create_read_write_trunc:
        return _O_RDWR | _O_CREAT | _O_TRUNC;
      case flags::create_read_write_append:
        return _O_RDWR | _O_CREAT | _O_APPEND;
      case flags::sync_all_on_write:
      default:
        return open_mode;
        break;
    }
    return open_mode;
  }
#endif
 private:
  read_type type_ = read_type::init;
#if defined(ENABLE_FILE_IO_URING)
  std::shared_ptr<asio::basic_file<>> stream_file_;
#else
  std::shared_ptr<FILE> stream_file_;
#endif
  coro_io::ExecutorWrapper<> executor_wrapper_;
  std::shared_ptr<int> fd_file_;
  std::string file_path_;
  std::atomic<bool> eof_ = false;
};
}  // namespace coro_io
