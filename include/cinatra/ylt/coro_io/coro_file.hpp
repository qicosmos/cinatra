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
#include <asio/io_context.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "async_simple/Promise.h"
#include "async_simple/Traits.h"
#include "async_simple/coro/FutureAwaiter.h"
#include "io_context_pool.hpp"
#if defined(ENABLE_FILE_IO_URING)
#include <asio/random_access_file.hpp>
#include <asio/stream_file.hpp>
#endif
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "asio/error.hpp"
#include "async_simple/coro/Lazy.h"
#include "coro_io.hpp"

namespace coro_io {

enum flags {
#if defined(ASIO_WINDOWS)
  read_only = 1,
  write_only = 2,
  read_write = 4,
  append = 8,
  create = 16,
  exclusive = 32,
  truncate = 64,
  sync_all_on_write = 128
#else   // defined(ASIO_WINDOWS)
  read_only = O_RDONLY,
  write_only = O_WRONLY,
  read_write = O_RDWR,
  append = O_APPEND,
  create = O_CREAT,
  exclusive = O_EXCL,
  truncate = O_TRUNC,
  sync_all_on_write = O_SYNC
#endif  // defined(ASIO_WINDOWS)
};

class coro_file {
 public:
#if defined(ENABLE_FILE_IO_URING)
  coro_file(
      coro_io::ExecutorWrapper<>* executor = coro_io::get_global_executor())
      : coro_file(executor->get_asio_executor()) {}

  coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}
#else

  coro_file(coro_io::ExecutorWrapper<>* executor =
                coro_io::get_global_block_executor())
      : coro_file(executor->get_asio_executor()) {}

  coro_file(asio::io_context::executor_type executor)
      : executor_wrapper_(executor) {}
#endif

  bool is_open() { return stream_file_ != nullptr; }

  void flush() {
#if defined(ENABLE_FILE_IO_URING)

#else
    if (stream_file_) {
      auto fptr = stream_file_.get();
      fflush(fptr);
    }
#endif
  }

  bool eof() { return eof_; }

  void close() { stream_file_.reset(); }

  static size_t file_size(std::string_view filepath) {
    std::error_code ec;
    size_t size = std::filesystem::file_size(filepath, ec);
    return size;
  }

#if defined(ENABLE_FILE_IO_URING)
  async_simple::coro::Lazy<bool> async_open(
      std::string_view filepath, flags open_mode = flags::read_write) {
    try {
      stream_file_ = std::make_unique<asio::stream_file>(
          executor_wrapper_.get_asio_executor());
    } catch (std::exception& ex) {
      std::cout << ex.what() << "\n";
      co_return false;
    }

    std::error_code ec;
    stream_file_->open(filepath.data(), open_mode, ec);
    if (ec) {
      std::cout << ec.message() << "\n";
      co_return false;
    }

    co_return true;
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      char* data, size_t size) {
    size_t left_size = size;
    size_t offset = 0;
    size_t read_total = 0;
    while (left_size) {
      auto [ec, read_size] = co_await coro_io::async_read_some(
          *stream_file_, asio::buffer(data + offset, size - offset));
      if (ec) {
        if (ec == asio::error::eof) {
          eof_ = true;
          co_return std::make_pair(std::error_code{}, read_total);
        }

        co_return std::make_pair(ec, 0);
      }

      if (read_size > size) {
        // if read_size is very large, it means the size if negative, and there
        // is an error occurred.
        co_return std::make_pair(
            std::make_error_code(std::errc::invalid_argument), 0);
      }

      read_total += read_size;

      left_size -= read_size;
      offset += read_size;
      seek_offset_ += read_size;
      std::error_code seek_ec;
      stream_file_->seek(seek_offset_, asio::file_base::seek_basis::seek_set,
                         seek_ec);
      if (seek_ec) {
        co_return std::make_pair(std::make_error_code(std::errc::invalid_seek),
                                 0);
      }
    }

    co_return std::make_pair(std::error_code{}, read_total);
  }

  async_simple::coro::Lazy<std::error_code> async_write(const char* data,
                                                        size_t size) {
    size_t left_size = size;
    size_t offset = 0;
    while (left_size) {
      auto [ec, write_size] = co_await coro_io::async_write_some(
          *stream_file_, asio::buffer(data, size));

      if (ec) {
        co_return ec;
      }

      left_size -= write_size;
      if (left_size == 0) {
        co_return ec;
      }
      offset += write_size;
      std::error_code seek_ec;
      stream_file_->seek(offset, asio::file_base::seek_basis::seek_set,
                         seek_ec);
      if (seek_ec) {
        co_return seek_ec;
      }
    }

    co_return std::error_code{};
  }
#else
  std::string str_mode(int open_mode) {
    switch (open_mode) {
      case O_RDONLY:
        return "r";
      case O_WRONLY:
        return "w";
      case O_RDWR:
        return "r+";
      case O_APPEND:
        return "a";
      case O_TRUNC:
        return "w+";
      default:
        return "r+";
    }
  }

  bool seek(long offset, int whence) {
    return fseek(stream_file_.get(), offset, whence) == 0;
  }

  async_simple::coro::Lazy<bool> async_open(
      std::string filepath, flags open_mode = flags::read_write) {
    if (stream_file_ != nullptr) {
      co_return true;
    }

    auto result = co_await coro_io::post(
        [this, &filepath, open_mode] {
          auto fptr = fopen(filepath.data(), str_mode(open_mode).data());
          if (fptr == nullptr) {
            std::cout << "open file " << filepath << " failed "
                      << "\n";
            return false;
          }
          stream_file_ = std::shared_ptr<FILE>(fptr, [](FILE* ptr) {
            fclose(ptr);
          });
          return true;
        },
        &executor_wrapper_);
    co_return result.value();
  }

  async_simple::coro::Lazy<std::pair<std::error_code, size_t>> async_read(
      char* data, size_t size) {
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

  async_simple::coro::Lazy<std::error_code> async_write(const char* data,
                                                        size_t size) {
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
#if defined(ENABLE_FILE_IO_URING)
  std::unique_ptr<asio::stream_file> stream_file_;
  std::atomic<size_t> seek_offset_ = 0;
#else
  std::shared_ptr<FILE> stream_file_;
#endif
  coro_io::ExecutorWrapper<> executor_wrapper_;

  std::atomic<bool> eof_ = false;
};
}  // namespace coro_io
