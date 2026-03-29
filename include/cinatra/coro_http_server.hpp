#pragma once

#include "cinatra/coro_http_client.hpp"
#include "cinatra/coro_http_response.hpp"
#include "cinatra/coro_http_router.hpp"
#include "cinatra/define.h"
#include "cinatra/mime_types.hpp"
#include "cinatra_log_wrapper.hpp"
#include "coro_http_connection.hpp"
#include "ylt/coro_io/coro_file.hpp"
#include "ylt/coro_io/coro_io.hpp"
#include "ylt/coro_io/io_context_pool.hpp"
#include "ylt/coro_io/load_blancer.hpp"

namespace cinatra {
enum class file_resp_format_type {
  chunked,
  range,
};
class coro_http_server {
 public:
  coro_http_server(asio::io_context &ctx, unsigned short port,
                   std::string address = "0.0.0.0")
      : out_ctx_(&ctx),
        port_(port),
        acceptor_(ctx),
        check_timer_(ctx),
        cache_refresh_timer_(ctx) {
    init_address(std::move(address));
  }

  coro_http_server(asio::io_context &ctx,
                   std::string address /* = "0.0.0.0:9001" */)
      : out_ctx_(&ctx),
        acceptor_(ctx),
        check_timer_(ctx),
        cache_refresh_timer_(ctx) {
    init_address(std::move(address));
  }

  coro_http_server(size_t thread_num, unsigned short port,
                   std::string address = "0.0.0.0", bool cpu_affinity = false)
      : pool_(std::make_unique<coro_io::io_context_pool>(thread_num,
                                                         cpu_affinity)),
        port_(port),
        acceptor_(pool_->get_executor()->get_asio_executor()),
        check_timer_(pool_->get_executor()->get_asio_executor()),
        cache_refresh_timer_(pool_->get_executor()->get_asio_executor()) {
    init_address(std::move(address));
  }

  coro_http_server(size_t thread_num,
                   std::string address /* = "0.0.0.0:9001" */,
                   bool cpu_affinity = false)
      : pool_(std::make_unique<coro_io::io_context_pool>(thread_num,
                                                         cpu_affinity)),
        acceptor_(pool_->get_executor()->get_asio_executor()),
        check_timer_(pool_->get_executor()->get_asio_executor()),
        cache_refresh_timer_(pool_->get_executor()->get_asio_executor()) {
    init_address(std::move(address));
  }

  ~coro_http_server() {
    CINATRA_LOG_INFO << "coro_http_server will quit";
    stop();
  }

  void set_no_delay(bool r) { no_delay_ = r; }

  void set_max_http_body_size(int64_t max_size) {
    max_http_body_len_ = max_size;
  }

#ifdef CINATRA_ENABLE_SSL
  void init_ssl(const std::string &cert_file, const std::string &key_file,
                const std::string &passwd = "") {
    cert_file_ = cert_file;
    key_file_ = key_file;
    passwd_ = passwd;
    use_ssl_ = true;
  }
#endif

  // only call once, not thread safe.
  std::error_code sync_start() noexcept {
    auto ret = async_start();
    ret.wait();
    return ret.value();
  }

  // only call once, not thread safe.
  async_simple::Future<std::error_code> async_start() {
    errc_ = listen();

    async_simple::Promise<std::error_code> promise;
    auto future = promise.getFuture();

    if (!errc_) {
      if (out_ctx_ == nullptr) {
        thd_ = std::thread([this] {
          pool_->run();
        });
      }

      accept().start([p = std::move(promise), this](auto &&res) mutable {
        if (res.hasError()) {
          errc_ = std::make_error_code(std::errc::io_error);
          p.setValue(errc_);
        }
        else {
          p.setValue(res.value());
        }
      });
    }
    else {
      promise.setValue(errc_);
    }

    return future;
  }

  // only call once, not thread safe.
  void stop() {
    if (out_ctx_ == nullptr && !thd_.joinable()) {
      return;
    }

    stop_timer_ = true;
    std::error_code ec;
    check_timer_.cancel(ec);
    // Wake up the sleeping cache refresh coroutine so it can exit cleanly.
    cache_refresh_timer_.cancel(ec);
    // Wait for the coroutine to fully exit before stopping the pool.
    // Must happen here while the pool is still running: the coroutine may
    // need the pool executor to resume after a coro_io::post completes.
    if (cache_refresh_done_.valid()) {
      cache_refresh_done_.wait();
    }

    close_acceptor();

    // close current connections.
    {
      std::scoped_lock lock(conn_mtx_);
      for (auto &conn : connections_) {
        conn.second->close(false);
      }
      connections_.clear();
    }

    if (out_ctx_ == nullptr) {
      CINATRA_LOG_INFO << "wait for server's thread-pool finish all work.";
      pool_->stop();

      CINATRA_LOG_INFO << "server's thread-pool finished.";
      thd_.join();
      CINATRA_LOG_INFO << "stop coro_http_server ok";
    }
    else {
      out_ctx_ = nullptr;
    }
  }

  // call it after server async_start or sync_start.
  uint16_t port() const { return port_; }

  template <http_method... method, typename Func, typename... Aspects>
  void set_http_handler(std::string key, Func handler, Aspects &&...asps) {
    static_assert(sizeof...(method) >= 1, "must set http_method");
    if constexpr (sizeof...(method) == 1) {
      (router_.set_http_handler<method>(std::move(key), std::move(handler),
                                        std::forward<Aspects>(asps)...),
       ...);
    }
    else {
      (router_.set_http_handler<method>(key, handler,
                                        std::forward<Aspects>(asps)...),
       ...);
    }
  }

  template <http_method... method, typename Func, typename... Aspects>
  void set_http_handler(std::string key, Func handler,
                        util::class_type_t<Func> &owner, Aspects &&...asps) {
    static_assert(std::is_member_function_pointer_v<Func>,
                  "must be member function");
    using return_type = typename util::function_traits<Func>::return_type;
    if constexpr (coro_io::is_lazy_v<return_type>) {
      std::function<async_simple::coro::Lazy<void>(coro_http_request & req,
                                                   coro_http_response & resp)>
          f = std::bind(handler, &owner, std::placeholders::_1,
                        std::placeholders::_2);
      set_http_handler<method...>(std::move(key), std::move(f),
                                  std::forward<Aspects>(asps)...);
    }
    else {
      std::function<void(coro_http_request & req, coro_http_response & resp)>
          f = std::bind(handler, &owner, std::placeholders::_1,
                        std::placeholders::_2);
      set_http_handler<method...>(std::move(key), std::move(f),
                                  std::forward<Aspects>(asps)...);
    }
  }

  template <http_method... method, typename... Aspects>
  void set_http_proxy_handler(std::string url_path,
                              std::vector<std::string_view> hosts,
                              coro_io::load_blance_algorithm type =
                                  coro_io::load_blance_algorithm::random,
                              std::vector<int> weights = {},
                              Aspects &&...aspects) {
    if (hosts.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }

    auto load_blancer =
        std::make_shared<coro_io::load_blancer<coro_http_client>>(
            coro_io::load_blancer<coro_http_client>::create(
                hosts, {.lba = type}, weights));
    auto handler =
        [this, load_blancer, type](
            coro_http_request &req,
            coro_http_response &response) -> async_simple::coro::Lazy<void> {
      co_await load_blancer->send_request(
          [this, &req, &response](
              coro_http_client &client,
              std::string_view host) -> async_simple::coro::Lazy<void> {
            co_await reply(client, host, req, response);
          });
    };

    if constexpr (sizeof...(method) == 0) {
      set_http_handler<http_method::GET, http_method::POST, http_method::DEL,
                       http_method::HEAD, http_method::PUT, http_method::PATCH,
                       http_method::CONNECT, http_method::TRACE,
                       http_method::OPTIONS>(url_path, std::move(handler),
                                             std::forward<Aspects>(aspects)...);
    }
    else {
      set_http_handler<method...>(url_path, std::move(handler),
                                  std::forward<Aspects>(aspects)...);
    }
  }

  template <http_method... method, typename... Aspects>
  void set_websocket_proxy_handler(std::string url_path,
                                   std::vector<std::string_view> hosts,
                                   coro_io::load_blance_algorithm type =
                                       coro_io::load_blance_algorithm::random,
                                   std::vector<int> weights = {},
                                   Aspects &&...aspects) {
    if (hosts.empty()) {
      throw std::invalid_argument("not config hosts yet!");
    }

    auto load_blancer =
        std::make_shared<coro_io::load_blancer<coro_http_client>>(
            coro_io::load_blancer<coro_http_client>::create(
                hosts, {.lba = type}, weights));

    set_http_handler<cinatra::GET>(
        url_path,
        [load_blancer](coro_http_request &req, coro_http_response &resp)
            -> async_simple::coro::Lazy<void> {
          websocket_result result{};
          while (true) {
            result = co_await req.get_conn()->read_websocket();
            if (result.ec) {
              break;
            }

            if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
              CINATRA_LOG_INFO << "close frame";
              break;
            }

            auto ret = co_await load_blancer->send_request(
                [&req, result](coro_http_client &client, std::string_view host)
                    -> async_simple::coro::Lazy<std::error_code> {
                  auto r =
                      co_await client.write_websocket(std::string(result.data));
                  if (r.net_err) {
                    co_return r.net_err;
                  }
                  auto data = co_await client.read_websocket();
                  if (data.net_err) {
                    co_return data.net_err;
                  }
                  auto ec = co_await req.get_conn()->write_websocket(
                      std::string(result.data));
                  if (ec) {
                    co_return ec;
                  }
                  co_return std::error_code{};
                });
            if (!ret.has_value()) {
              req.get_conn()->close();
              break;
            }
          }
        },
        std::forward<Aspects>(aspects)...);
  }

  void set_max_size_of_cache_files(size_t max_size = 3 * 1024 * 1024) {
    std::error_code ec;
    auto new_cache =
        std::make_shared<std::unordered_map<std::string, std::string>>();
    for (const auto &file :
         std::filesystem::recursive_directory_iterator(static_dir_, ec)) {
      if (ec) {
        continue;
      }

      if (!file.is_directory()) {
        size_t filesize = fs::file_size(file, ec);
        if (ec || filesize > max_size) {
          continue;
        }

        std::ifstream ifs(file.path(), std::ios::binary);
        if (ifs.is_open()) {
          std::string content;
          detail::resize(content, filesize);
          ifs.read(content.data(), content.size());
          new_cache->emplace(file.path().string(), std::move(content));
        }
      }
    }
    std::atomic_store(&file_cache_, new_cache);
  }

  const coro_http_router &get_router() const { return router_; }

  void set_file_resp_format_type(file_resp_format_type type) {
    format_type_ = type;
  }

  void set_transfer_chunked_size(size_t size) { chunked_size_ = size; }

  // Enable background cache refresh for the static resource directory.
  // The timer checks the directory mtime every `interval`; only when the
  // directory has changed (file added/removed) is the cache rebuilt.
  void set_cache_refresh_interval(
      std::chrono::steady_clock::duration interval = std::chrono::seconds(30),
      size_t max_file_size = 3 * 1024 * 1024) {
    cache_refresh_interval_ = interval;
    max_cache_file_size_ = max_file_size;
    cache_refresh_stopped_ = std::promise<void>{};
    cache_refresh_done_ = cache_refresh_stopped_.get_future();
    cache_refresh_loop().start([](auto &&) {});
  }

#ifdef INJECT_FOR_HTTP_SEVER_TEST
  void set_write_failed_forever(bool r) { write_failed_forever_ = r; }

  void set_read_failed_forever(bool r) { read_failed_forever_ = r; }
#endif

  template <typename... Aspects>
  void set_static_res_dir(std::string_view uri_suffix = "",
                          std::string file_path = "www", Aspects &&...aspects) {
    bool has_double_dot = (file_path.find("..") != std::string::npos) ||
                          (uri_suffix.find("..") != std::string::npos);
    if (std::filesystem::path(file_path).has_root_path() ||
        std::filesystem::path(uri_suffix).has_root_path() || has_double_dot) {
      CINATRA_LOG_ERROR << "invalid file path: " << file_path;
      std::exit(1);
    }

    if (!uri_suffix.empty()) {
      static_dir_router_path_ =
          std::filesystem::path(uri_suffix).make_preferred().string();
    }

    // Track whether a named subdirectory was given, which determines the URI
    // prefix when uri_suffix is also empty.
    bool has_named_dir = false;
    if (!file_path.empty()) {
      file_path = std::filesystem::path(file_path).filename().string();
      if (file_path.empty()) {
        static_dir_ = fs::absolute(fs::current_path().string()).string();
      }
      else {
        static_dir_ =
            std::filesystem::path(file_path).make_preferred().string();
        has_named_dir = true;
      }
    }
    else {
      static_dir_ = fs::absolute(fs::current_path().string()).string();
    }

    // Derive URI prefix internally, preserving original routing semantics:
    // - uri_suffix given       → use it as prefix  (e.g. "assets" → /assets)
    // - named subdir, no suffix → use dirname       (e.g. "www"   → /www)
    // - current dir, no suffix  → empty prefix      (e.g. ""      → /)
    //   The last case keeps the original behaviour where files are served
    //   at the root, and avoids a bare /(.+) catch-all by falling through
    //   to the exact-match / prefix pattern /(.+) only for that dir.
    std::string uri_prefix;
    if (!static_dir_router_path_.empty()) {
      uri_prefix = "/" + static_dir_router_path_;
    }
    else if (has_named_dir) {
      uri_prefix = "/" + fs::path(static_dir_).filename().string();
    }
    // else: current directory → uri_prefix stays empty, pattern is "/(.+)"
    std::string pattern = uri_prefix + "/(.+)";
    set_http_handler<cinatra::GET>(
        pattern,
        [this, base_dir = static_dir_](
            coro_http_request &req,
            coro_http_response &resp) -> async_simple::coro::Lazy<void> {
          std::string rel = req.matches_.str(1);
          replace_all(rel, "\\", "/");
          std::string file_name =
              (fs::path(base_dir) / rel).make_preferred().string();

          std::error_code path_ec;
          auto abs = fs::weakly_canonical(file_name, path_ec);
          auto base = fs::weakly_canonical(base_dir, path_ec);
          if (path_ec || abs.string().find(base.string()) != 0) {
            resp.set_status(status_type::bad_request);
            co_return;
          }

          co_await serve_static_file_(file_name, req, resp);
        },
        std::forward<Aspects>(aspects)...);
  }

 public:
  async_simple::coro::Lazy<void> serve_static_file_(
      const std::string &file_name, coro_http_request &req,
      coro_http_response &resp) {
    std::string_view extension = get_extension(file_name);
    std::string_view mime = get_mime_type(extension);
    auto range_str = req.get_header_value("Range");

    auto cache = std::atomic_load(&file_cache_);
    if (cache) {
      if (auto it = cache->find(file_name); it != cache->end()) {
        auto range_header = build_range_header(
            mime, file_name, std::to_string(fs::file_size(file_name)));
        resp.set_delay(true);
        const std::string &body = it->second;
        std::array<asio::const_buffer, 2> arr{asio::buffer(range_header),
                                              asio::buffer(body)};
        co_await req.get_conn()->async_write(arr);
        co_return;
      }
    }

    std::string content;
    detail::resize(content, chunked_size_);

    coro_io::coro_file in_file{};
    in_file.open(file_name, std::ios::in);
    if (!in_file.is_open()) {
#ifndef NDEBUG
      resp.set_status_and_content(status_type::not_found,
                                  file_name + " not found");
#else
      resp.set_status(status_type::not_found);
#endif
      co_return;
    }

    size_t file_size = fs::file_size(file_name);

    if (format_type_ == file_resp_format_type::chunked && range_str.empty()) {
      resp.add_header("Content-Type", std::string{mime});
      resp.set_format_type(format_type::chunked);
      bool ok;
      if (ok = co_await resp.get_conn()->begin_chunked(); !ok) {
        co_return;
      }
      while (true) {
        auto [ec, size] =
            co_await in_file.async_read(content.data(), content.size());
        if (ec) {
          resp.set_status(status_type::no_content);
          co_await resp.get_conn()->reply();
          co_return;
        }
        bool r = co_await resp.get_conn()->write_chunked(
            std::string_view(content.data(), size));
        if (!r) {
          co_return;
        }
        if (in_file.eof()) {
          co_await resp.get_conn()->end_chunked();
          break;
        }
      }
      co_return;
    }

    auto pos = range_str.find('=');
    if (pos != std::string_view::npos) {
      range_str = range_str.substr(pos + 1);
      bool is_valid = true;
      auto ranges = parse_ranges(range_str, file_size, is_valid);
      if (!is_valid) {
        resp.set_status(status_type::range_not_satisfiable);
        co_return;
      }
      assert(!ranges.empty());

      if (ranges.size() == 1) {
        auto [start, end] = ranges[0];
        in_file.seek(start, std::ios::beg);
        size_t part_size = end + 1 - start;
        int status = (part_size == file_size) ? 200 : 206;
        std::string content_range = "Content-Range: bytes ";
        content_range.append(std::to_string(start))
            .append("-")
            .append(std::to_string(end))
            .append("/")
            .append(std::to_string(file_size))
            .append(CRCF);
        auto range_header = build_range_header(
            mime, file_name, std::to_string(part_size), status, content_range);
        resp.set_delay(true);
        bool r = co_await req.get_conn()->write_data(range_header);
        if (!r) {
          co_return;
        }
        co_await send_single_part(in_file, content, req, resp, part_size);
      }
      else {
        resp.set_delay(true);
        std::string file_size_str = std::to_string(file_size);
        size_t content_len = 0;
        std::vector<std::string> multi_heads =
            build_part_heads(ranges, mime, file_size_str, content_len);
        auto range_header = build_multiple_range_header(content_len);
        bool r = co_await req.get_conn()->write_data(range_header);
        if (!r) {
          co_return;
        }
        for (int i = 0; i < ranges.size(); i++) {
          r = co_await req.get_conn()->write_data(multi_heads[i]);
          if (!r) {
            co_return;
          }
          auto [start, end] = ranges[i];
          bool ok = in_file.seek(start, std::ios::beg);
          if (!ok) {
            resp.set_status_and_content(status_type::bad_request,
                                        "invalid range");
            co_await resp.get_conn()->reply();
            co_return;
          }
          size_t part_size = end + 1 - start;
          std::string_view more =
              (i == (int)ranges.size() - 1) ? MULTIPART_END : CRCF;
          r = co_await send_single_part(in_file, content, req, resp, part_size,
                                        more);
          if (!r) {
            co_return;
          }
        }
      }
      co_return;
    }

    auto range_header =
        build_range_header(mime, file_name, std::to_string(file_size));
    resp.set_delay(true);
    bool r = co_await req.get_conn()->write_data(range_header);
    if (!r) {
      co_return;
    }
    while (true) {
      auto [ec, size] =
          co_await in_file.async_read(content.data(), content.size());
      if (ec) {
        resp.set_status(status_type::no_content);
        co_await resp.get_conn()->reply();
        co_return;
      }
      r = co_await req.get_conn()->write_data(
          std::string_view(content.data(), size));
      if (!r) {
        co_return;
      }
      if (in_file.eof()) {
        break;
      }
    }
  }

  void set_check_duration(auto duration) { check_duration_ = duration; }

  void set_timeout_duration(
      std::chrono::steady_clock::duration timeout_duration) {
    if (timeout_duration > std::chrono::steady_clock::duration::zero()) {
      need_check_ = true;
      timeout_duration_ = timeout_duration;
      start_check_timer();
    }
  }

  void set_shrink_to_fit(bool r) { need_shrink_every_time_ = r; }

  void set_default_handler(std::function<async_simple::coro::Lazy<void>(
                               coro_http_request &, coro_http_response &)>
                               handler) {
    default_handler_ = std::move(handler);
  }

  size_t connection_count() {
    std::scoped_lock lock(conn_mtx_);
    return connections_.size();
  }

  std::string_view address() { return address_; }
  std::error_code get_errc() { return errc_; }

 private:
  std::error_code listen() {
    CINATRA_LOG_INFO << "begin to listen " << port_;
    using asio::ip::tcp;
    asio::error_code ec;

    asio::ip::tcp::resolver::query query(address_, std::to_string(port_));
    asio::ip::tcp::resolver resolver(acceptor_.get_executor());
    asio::ip::tcp::resolver::iterator it = resolver.resolve(query, ec);

    asio::ip::tcp::resolver::iterator it_end;
    if (ec || it == it_end) {
      CINATRA_LOG_ERROR << "bad address: " << address_
                        << " error: " << ec.message();
      if (ec) {
        return ec;
      }
      return std::make_error_code(std::errc::address_not_available);
    }

    auto endpoint = it->endpoint();
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      CINATRA_LOG_ERROR << "acceptor open failed"
                        << " error: " << ec.message();
      return ec;
    }
#ifdef __GNUC__
    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
#endif
    acceptor_.bind(endpoint, ec);
    if (ec) {
      CINATRA_LOG_ERROR << "bind port: " << port_ << " error: " << ec.message();
      std::error_code ignore_ec;
      acceptor_.cancel(ignore_ec);
      acceptor_.close(ignore_ec);
      return ec;
    }
#ifdef _MSC_VER
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
#endif
    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      CINATRA_LOG_ERROR << "get local endpoint port: " << port_
                        << " listen error: " << ec.message();
      return ec;
    }

    auto end_point = acceptor_.local_endpoint(ec);
    if (ec) {
      CINATRA_LOG_ERROR << "get local endpoint port: " << port_
                        << " error: " << ec.message();
      return ec;
    }
    port_ = end_point.port();

    CINATRA_LOG_INFO << "listen port " << port_ << " successfully";
    return {};
  }

  async_simple::coro::Lazy<std::error_code> accept() {
    for (;;) {
      coro_io::ExecutorWrapper<> *executor;
      if (out_ctx_ == nullptr) {
        executor = pool_->get_executor();
      }
      else {
        out_executor_ = std::make_unique<coro_io::ExecutorWrapper<>>(
            out_ctx_->get_executor());
        executor = out_executor_.get();
      }

      asio::ip::tcp::socket socket(executor->get_asio_executor());
      auto error = co_await coro_io::async_accept(acceptor_, socket);
      if (error) {
        CINATRA_LOG_INFO << "accept failed, error: " << error.message();
        if (error == asio::error::operation_aborted ||
            error == asio::error::bad_descriptor) {
          acceptor_close_waiter_.set_value();
          co_return error;
        }
        continue;
      }

      uint64_t conn_id = ++conn_id_;
      CINATRA_LOG_DEBUG << "new connection comming, id: " << conn_id;
      auto conn = std::make_shared<coro_http_connection>(
          executor, std::move(socket), router_);
      if (no_delay_) {
        conn->tcp_socket().set_option(asio::ip::tcp::no_delay(true));
      }
      conn->set_max_http_body_size(max_http_body_len_);
      if (need_shrink_every_time_) {
        conn->set_shrink_to_fit(true);
      }
      if (need_check_) {
        conn->set_check_timeout(true);
      }
      if (default_handler_) {
        conn->set_default_handler(default_handler_);
      }

#ifdef INJECT_FOR_HTTP_SEVER_TEST
      if (write_failed_forever_) {
        conn->set_write_failed_forever(write_failed_forever_);
      }
      if (read_failed_forever_) {
        conn->set_read_failed_forever(read_failed_forever_);
      }
#endif

#ifdef CINATRA_ENABLE_SSL
      if (use_ssl_) {
        conn->init_ssl(cert_file_, key_file_, passwd_);
      }
#endif

      conn->set_quit_callback(
          [this](const uint64_t &id) {
            std::scoped_lock lock(conn_mtx_);
            if (!connections_.empty())
              connections_.erase(id);
          },
          conn_id);

      {
        std::scoped_lock lock(conn_mtx_);
        connections_.emplace(conn_id, conn);
      }

      start_one(conn).via(conn->get_executor()).detach();
    }
  }

  async_simple::coro::Lazy<void> start_one(
      std::shared_ptr<coro_http_connection> conn) noexcept {
    co_await conn->start();
  }

  void close_acceptor() {
    asio::dispatch(acceptor_.get_executor(), [this]() {
      asio::error_code ec;
      acceptor_.cancel(ec);
      acceptor_.close(ec);
    });
    acceptor_close_waiter_.get_future().wait();
  }

  // Coroutine-based cache refresh loop.
  //
  // Sleep is done via period_timer::async_await() so stop() can wake it up
  // immediately by cancelling the timer. File reads are offloaded to the
  // global block executor via coro_io::post (non-blocking for the event loop),
  // with all data captured by value so 'this' is never accessed from that
  // thread. After the loop exits, the promise is fulfilled so stop() can
  // safely proceed to pool_->stop() without a use-after-free.
  async_simple::coro::Lazy<void> cache_refresh_loop() {
    while (true) {
      cache_refresh_timer_.expires_after(cache_refresh_interval_);
      bool timer_ok = co_await cache_refresh_timer_.async_await();
      if (!timer_ok || stop_timer_) {
        break;
      }

      std::error_code mtime_ec;
      auto dir_mtime = fs::last_write_time(static_dir_, mtime_ec);
      if (mtime_ec || dir_mtime == last_dir_mtime_) {
        continue;
      }
      last_dir_mtime_ = dir_mtime;

      // Capture by value: file reads run on the global block executor,
      // 'this' must not be touched from that thread.
      std::string static_dir = static_dir_;
      size_t max_size = max_cache_file_size_;
      auto result = co_await coro_io::post(
          [static_dir = std::move(static_dir), max_size]() {
            using FileMap =
                std::unordered_map<std::string, std::string>;
            auto new_cache = std::make_shared<FileMap>();
            std::error_code iter_ec;
            for (const auto &file :
                 fs::recursive_directory_iterator(static_dir, iter_ec)) {
              if (iter_ec || file.is_directory()) {
                continue;
              }
              size_t filesize = fs::file_size(file, iter_ec);
              if (iter_ec || filesize > max_size) {
                continue;
              }
              std::ifstream ifs(file.path(), std::ios::binary);
              if (ifs.is_open()) {
                std::string content(filesize, '\0');
                ifs.read(content.data(), content.size());
                new_cache->emplace(file.path().string(),
                                   std::move(content));
              }
            }
            return new_cache;
          });

      // Re-check stop flag: stop() may have been called while we were
      // doing file reads on the block executor.
      if (stop_timer_) {
        break;
      }

      if (!result.hasError()) {
        std::atomic_store(&file_cache_, result.value());
      }
    }

    // Signal stop() that this coroutine has fully exited and will no
    // longer access any member of 'this'.
    cache_refresh_stopped_.set_value();
  }

  void start_check_timer() {
    check_timer_.expires_after(check_duration_);
    check_timer_.async_wait([this](auto ec) {
      if (ec || stop_timer_) {
        return;
      }

      check_timeout();
      start_check_timer();
    });
  }

  void check_timeout() {
    auto cur_time = std::chrono::system_clock::now();

    std::unordered_map<uint64_t, std::shared_ptr<coro_http_connection>> conns;

    {
      std::scoped_lock lock(conn_mtx_);
      for (auto it = connections_.begin();
           it != connections_.end();)  // no "++"!
      {
        if (cur_time - it->second->get_last_rwtime() > timeout_duration_) {
          it->second->close(false);
          connections_.erase(it++);
        }
        else {
          ++it;
        }
      }
    }
  }

  std::string build_multiple_range_header(size_t content_len) {
    std::string header_str = "HTTP/1.1 206 Partial Content\r\n";
    header_str.append("Content-Length: ");
    header_str.append(std::to_string(content_len)).append(CRCF);
    header_str.append("Content-Type: multipart/byteranges; boundary=");
    header_str.append(BOUNDARY).append(TWO_CRCF);
    return header_str;
  }

  std::vector<std::string> build_part_heads(auto &ranges, std::string_view mime,
                                            std::string_view file_size_str,
                                            size_t &content_len) {
    std::vector<std::string> multi_heads;
    for (auto [start, end] : ranges) {
      std::string part_header = "--";
      part_header.append(BOUNDARY).append(CRCF);
      part_header.append("Content-Type: ").append(mime).append(CRCF);
      part_header.append("Content-Range: ").append("bytes ");
      part_header.append(std::to_string(start))
          .append("-")
          .append(std::to_string(end))
          .append("/")
          .append(file_size_str)
          .append(TWO_CRCF);
      content_len += part_header.size();
      multi_heads.push_back(std::move(part_header));
      size_t part_size = end + 1 - start + CRCF.size();
      content_len += part_size;
    }
    content_len += (BOUNDARY.size() + 4);
    return multi_heads;
  }

  std::string build_range_header(std::string_view mime,
                                 std::string_view filename,
                                 std::string_view file_size_str,
                                 int status = 200,
                                 std::string_view content_range = "") {
    std::string header_str = "HTTP/1.1 ";
    header_str.append(std::to_string(status));
    header_str.append(" OK\r\nAccept-Ranges: bytes\r\n");
    if (!content_range.empty()) {
      header_str.append(content_range);
    }
    header_str.append("Content-Disposition: attachment;filename=");
    std::string short_name =
        std::filesystem::path(filename).filename().string();
    header_str.append(short_name).append("\r\n");
    header_str.append("Connection: keep-alive\r\n");
    header_str.append("Content-Type: ").append(mime).append("\r\n");
    header_str.append("Content-Length: ");
    header_str.append(file_size_str).append("\r\n\r\n");
    return header_str;
  }

  async_simple::coro::Lazy<bool> send_single_part(auto &in_file, auto &content,
                                                  auto &req, auto &resp,
                                                  size_t part_size,
                                                  std::string_view more = "") {
    while (true) {
      size_t read_size = (std::min)(part_size, chunked_size_);
      if (read_size == 0) {
        break;
      }
      auto [ec, size] = co_await in_file.async_read(content.data(), read_size);
      if (ec) {
        resp.set_status(status_type::no_content);
        co_await resp.get_conn()->reply();
        co_return false;
      }

      part_size -= read_size;

      bool r = true;
      if (more.empty()) {
        r = co_await req.get_conn()->write_data(
            std::string_view(content.data(), size));
      }
      else {
        std::array<asio::const_buffer, 2> arr{
            asio::buffer(content.data(), size), asio::buffer(more)};
        auto [ec, _] = co_await req.get_conn()->async_write(arr);
        if (ec) {
          r = false;
        }
      }

      if (!r) {
        co_return false;
      }
    }

    co_return true;
  }

  template <class T, class Pred>
  size_t erase_if(std::span<T> &sp, Pred p) {
    auto it = std::remove_if(sp.begin(), sp.end(), p);
    size_t count = sp.end() - it;
    sp = std::span<T>(sp.data(), sp.data() + count);
    return count;
  }

  int remove_result_headers(resp_data &result, std::string_view value) {
    bool r = false;
    return erase_if(result.resp_headers, [&](http_header &header) {
      if (r) {
        return false;
      }

      r = (header.value.find(value) != std::string_view::npos);

      return r;
    });
  }

  void handle_response_header(resp_data &result, std::string &length) {
    int r = remove_result_headers(result, "chunked");
    if (r == 0) {
      r = remove_result_headers(result, "multipart/form-data");
      if (r) {
        length = std::to_string(result.resp_body.size());
        for (auto &[key, val] : result.resp_headers) {
          if (key == "Content-Length") {
            val = length;
            break;
          }
        }
      }
    }
  }

  async_simple::coro::Lazy<void> reply(coro_http_client &client,
                                       std::string_view host,
                                       coro_http_request &req,
                                       coro_http_response &response) {
    uri_t uri;
    std::string proxy_host;

    if (host.find("//") == std::string_view::npos) {
      proxy_host.append("http://").append(host);
      uri.parse_from(proxy_host.data());
    }
    else {
      uri.parse_from(host.data());
    }
    std::unordered_map<std::string, std::string> req_headers;
    for (auto &[k, v] : req.get_headers()) {
      req_headers.emplace(k, v);
    }
    req_headers["Host"] = uri.host;

    auto ctx = req_context<std::string_view>{.content = req.get_body()};
    auto result = co_await client.async_request(
        req.full_url(), method_type(req.get_method()), std::move(ctx),
        std::move(req_headers));

    std::string length;
    handle_response_header(result, length);
    response.add_header_span(result.resp_headers);

    response.set_status_and_content_view(
        static_cast<status_type>(result.status), result.resp_body);
    co_await response.get_conn()->reply();
    response.set_delay(true);
  }

  void init_address(std::string address) {
#if __has_include(<ylt/easylog.hpp>)
    easylog::logger<>::instance();  // init easylog singleton to make sure
                                    // server destruct before easylog.
#endif

    if (size_t pos = address.find(':'); pos != std::string::npos) {
      auto port_sv = std::string_view(address).substr(pos + 1);

      uint16_t port;
      auto [ptr, ec] = std::from_chars(
          port_sv.data(), port_sv.data() + port_sv.size(), port, 10);
      if (ec != std::errc{}) {
        address_ = std::move(address);
        return;
      }

      port_ = port;
      address = address.substr(0, pos);
    }

    address_ = std::move(address);
  }

 private:
  std::unique_ptr<coro_io::io_context_pool> pool_;
  asio::io_context *out_ctx_ = nullptr;
  std::unique_ptr<coro_io::ExecutorWrapper<>> out_executor_ = nullptr;
  uint16_t port_;
  std::string address_;
  std::error_code errc_ = {};
  asio::ip::tcp::acceptor acceptor_;
  std::thread thd_;
  std::promise<void> acceptor_close_waiter_;
  bool no_delay_ = true;

  uint64_t conn_id_ = 0;
  std::unordered_map<uint64_t, std::shared_ptr<coro_http_connection>>
      connections_;
  std::mutex conn_mtx_;
  std::chrono::steady_clock::duration check_duration_ =
      std::chrono::seconds(15);
  std::chrono::steady_clock::duration timeout_duration_{};
  asio::steady_timer check_timer_;
  bool need_check_ = false;
  std::atomic<bool> stop_timer_ = false;

  std::string static_dir_router_path_ = "";
  std::string static_dir_ = "";
  size_t chunked_size_ = 1024 * 10;

  std::shared_ptr<std::unordered_map<std::string, std::string>> file_cache_;
  coro_io::period_timer cache_refresh_timer_;
  std::chrono::steady_clock::duration cache_refresh_interval_ =
      std::chrono::seconds(5);
  size_t max_cache_file_size_ = 3 * 1024 * 1024;
  fs::file_time_type last_dir_mtime_{};
  std::promise<void> cache_refresh_stopped_;
  std::future<void> cache_refresh_done_;
  file_resp_format_type format_type_ = file_resp_format_type::range;
#ifdef CINATRA_ENABLE_SSL
  std::string cert_file_;
  std::string key_file_;
  std::string passwd_;
  bool use_ssl_ = false;
#endif
  coro_http_router router_;
  bool need_shrink_every_time_ = false;
  std::function<async_simple::coro::Lazy<void>(coro_http_request &,
                                               coro_http_response &)>
      default_handler_ = nullptr;
  int64_t max_http_body_len_ = MAX_HTTP_BODY_SIZE;
#ifdef INJECT_FOR_HTTP_SEVER_TEST
  bool write_failed_forever_ = false;
  bool read_failed_forever_ = false;
#endif
};

using http_server = coro_http_server;
using request = coro_http_request;
using response = coro_http_response;
}  // namespace cinatra