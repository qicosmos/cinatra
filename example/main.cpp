#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "metric_conf.hpp"

using namespace cinatra;
using namespace ylt::metric;
using namespace std::chrono_literals;

void create_file(std::string filename, size_t file_size = 64) {
  std::ofstream file(filename, std::ios::binary);
  if (file) {
    std::string str(file_size, 'A');
    file.write(str.data(), str.size());
  }
}

async_simple::coro::Lazy<void> byte_ranges_download() {
  create_file("test_multiple_range.txt", 64);
  coro_http_server server(1, 8090);
  server.set_static_res_dir("", "");
  server.async_start();
  std::this_thread::sleep_for(200ms);

  std::string uri = "http://127.0.0.1:8090/test_multiple_range.txt";
  {
    std::string filename = "test1.txt";
    std::error_code ec{};
    std::filesystem::remove(filename, ec);

    coro_http_client client{};
    resp_data result = co_await client.async_download(uri, filename, "1-10");
    assert(result.status == 206);
    assert(std::filesystem::file_size(filename) == 10);

    filename = "test2.txt";
    std::filesystem::remove(filename, ec);
    result = co_await client.async_download(uri, filename, "10-15");
    assert(result.status == 206);
    assert(std::filesystem::file_size(filename) == 6);
  }

  {
    coro_http_client client{};
    std::string uri = "http://127.0.0.1:8090/test_multiple_range.txt";

    client.add_header("Range", "bytes=1-10,20-30");
    auto result = co_await client.async_get(uri);
    assert(result.status == 206);
    assert(result.resp_body.size() == 21);

    std::string filename = "test_ranges.txt";
    client.add_header("Range", "bytes=0-10,21-30");
    result = co_await client.async_download(uri, filename);
    assert(result.status == 206);
    assert(fs::file_size(filename) == 21);
  }
}

async_simple::coro::Lazy<resp_data> chunked_upload1(coro_http_client &client) {
  std::string filename = "test.txt";
  create_file(filename, 1010);

  coro_io::coro_file file{};
  co_await file.async_open(filename, coro_io::flags::read_only);

  std::string buf;
  detail::resize(buf, 100);

  auto fn = [&file, &buf]() -> async_simple::coro::Lazy<read_result> {
    auto [ec, size] = co_await file.async_read(buf.data(), buf.size());
    co_return read_result{buf, file.eof(), ec};
  };

  auto result = co_await client.async_upload_chunked(
      "http://127.0.0.1:9001/chunked"sv, http_method::POST, std::move(fn));
  co_return result;
}

async_simple::coro::Lazy<void> chunked_upload_download() {
  cinatra::coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/chunked",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        assert(req.get_content_type() == content_type::chunked);
        chunked_result result{};
        std::string content;

        while (true) {
          result = co_await req.get_conn()->read_chunked();
          if (result.ec) {
            co_return;
          }
          if (result.eof) {
            break;
          }

          content.append(result.data);
        }

        std::cout << "content size: " << content.size() << "\n";
        std::cout << content << "\n";
        resp.set_format_type(format_type::chunked);
        resp.set_status_and_content(status_type::ok, "chunked ok");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/write_chunked",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        resp.set_format_type(format_type::chunked);
        bool ok;
        if (ok = co_await resp.get_conn()->begin_chunked(); !ok) {
          co_return;
        }

        std::vector<std::string> vec{"hello", " world", " ok"};

        for (auto &str : vec) {
          if (ok = co_await resp.get_conn()->write_chunked(str); !ok) {
            co_return;
          }
        }

        ok = co_await resp.get_conn()->end_chunked();
      });

  server.async_start();
  std::this_thread::sleep_for(200ms);

  coro_http_client client{};
  auto r = co_await chunked_upload1(client);
  assert(r.status == 200);
  assert(r.resp_body == "chunked ok");

  auto ss = std::make_shared<std::stringstream>();
  *ss << "hello world";
  auto result = co_await client.async_upload_chunked(
      "http://127.0.0.1:9001/chunked"sv, http_method::POST, ss);
  assert(result.status == 200);
  assert(result.resp_body == "chunked ok");

  result = co_await client.async_get("http://127.0.0.1:9001/write_chunked");
  assert(result.status == 200);
  assert(result.resp_body == "hello world ok");
}

async_simple::coro::Lazy<void> use_websocket() {
  coro_http_server server(1, 9001);
  server.set_http_handler<cinatra::GET>(
      "/ws_echo",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        assert(req.get_content_type() == content_type::websocket);
        websocket_result result{};
        while (true) {
          result = co_await req.get_conn()->read_websocket();
          if (result.ec) {
            break;
          }

          if (result.type == ws_frame_type::WS_CLOSE_FRAME) {
            std::cout << "close frame\n";
            break;
          }

          if (result.type == ws_frame_type::WS_TEXT_FRAME ||
              result.type == ws_frame_type::WS_BINARY_FRAME) {
            std::cout << result.data << "\n";
          }
          else if (result.type == ws_frame_type::WS_PING_FRAME ||
                   result.type == ws_frame_type::WS_PONG_FRAME) {
            // ping pong frame just need to continue, no need echo anything,
            // because framework has reply ping/pong msg to client
            // automatically.
            continue;
          }
          else {
            // error frame
            break;
          }

          auto ec = co_await req.get_conn()->write_websocket(result.data);
          if (ec) {
            break;
          }
        }
      });
  server.async_start();
  std::this_thread::sleep_for(300ms);  // wait for server start

  coro_http_client client{};
  auto r = co_await client.connect("ws://127.0.0.1:9001/ws_echo");
  if (r.net_err) {
    co_return;
  }

  auto result = co_await client.write_websocket("hello websocket");
  assert(!result.net_err);
  auto data = co_await client.read_websocket();
  assert(data.resp_body == "hello websocket");
  result = co_await client.write_websocket("test again");
  assert(!result.net_err);
  data = co_await client.read_websocket();
  assert(data.resp_body == "test again");
}

async_simple::coro::Lazy<void> static_file_server() {
  std::string filename = "temp.txt";
  create_file(filename, 64);

  coro_http_server server(1, 9001);

  std::string virtual_path = "download";
  std::string files_root_path = "";  // current path
  server.set_static_res_dir(
      virtual_path,
      files_root_path);  // set this before server start, if you add new files,
                         // you need restart the server.
  server.async_start();
  std::this_thread::sleep_for(300ms);  // wait for server start

  coro_http_client client{};
  auto result =
      co_await client.async_get("http://127.0.0.1:9001/download/temp.txt");
  assert(result.status == 200);
  assert(result.resp_body.size() == 64);
}

struct log_t {
  bool before(coro_http_request &, coro_http_response &) {
    std::cout << "before log" << std::endl;
    return true;
  }

  bool after(coro_http_request &, coro_http_response &res) {
    std::cout << "after log" << std::endl;
    res.add_header("aaaa", "bbcc");
    return true;
  }
};

struct get_data {
  bool before(coro_http_request &req, coro_http_response &res) {
    req.set_aspect_data("hello world");
    return true;
  }
};

async_simple::coro::Lazy<void> use_aspects() {
  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/get",
      [](coro_http_request &req, coro_http_response &resp) {
        auto &val = req.get_aspect_data();
        assert(val[0] == "hello world");
        resp.set_status_and_content(status_type::ok, "ok");
      },
      log_t{}, get_data{});

  server.async_start();
  std::this_thread::sleep_for(300ms);  // wait for server start

  coro_http_client client{};
  auto result = co_await client.async_get("http://127.0.0.1:9001/get");
  assert(result.status == 200);

  co_return;
}

struct person_t {
  void foo(coro_http_request &, coro_http_response &res) {
    res.set_status_and_content(status_type::ok, "ok");
  }
};

async_simple::coro::Lazy<void> basic_usage() {
  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/get", [](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<GET>(
      "/coro",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        resp.set_status_and_content(status_type::ok, "ok");
        co_return;
      });

  server.set_http_handler<POST>(
      "/form_data",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        assert(req.get_content_type() == content_type::multipart);
        auto boundary = req.get_boundary();
        multipart_reader_t multipart(req.get_conn());
        while (true) {
          auto part_head = co_await multipart.read_part_head(boundary);
          if (part_head.ec) {
            resp.set_status_and_content(status_type::bad_request,
                                        "bad_request");
            co_return;
          }

          std::cout << part_head.name << "\n";
          std::cout << part_head.filename << "\n";  // if form data, no filename

          auto part_body = co_await multipart.read_part_body(boundary);
          if (part_body.ec) {
            co_return;
          }

          std::cout << part_body.data << "\n";

          if (part_body.eof) {
            break;
          }
        }

        resp.set_status_and_content(status_type::ok, "multipart finished");
      });

  server.set_http_handler<GET>(
      "/in_thread_pool",
      [](coro_http_request &req,
         coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        // will respose in another thread.
        co_await coro_io::post([&] {
          // do your heavy work here when finished work, response.
          resp.set_status_and_content(status_type::ok, "ok");
        });
      });

  server.set_http_handler<POST, PUT>(
      "/post", [](coro_http_request &req, coro_http_response &resp) {
        auto req_body = req.get_body();
        resp.set_status_and_content(status_type::ok, std::string{req_body});
      });

  server.set_http_handler<GET>(
      "/headers", [](coro_http_request &req, coro_http_response &resp) {
        auto name = req.get_header_value("name");
        auto age = req.get_header_value("age");
        assert(name == "tom");
        assert(age == "20");
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<GET>(
      "/query", [](coro_http_request &req, coro_http_response &resp) {
        auto name = req.get_query_value("name");
        auto age = req.get_query_value("age");
        assert(name == "tom");
        assert(age == "20");
        resp.set_status_and_content(status_type::ok, "ok");
      });

  server.set_http_handler<cinatra::GET, cinatra::POST>(
      "/users/:userid/subscriptions/:subid",
      [](coro_http_request &req, coro_http_response &response) {
        assert(req.params_["userid"] == "ultramarines");
        assert(req.params_["subid"] == "guilliman");
        response.set_status_and_content(status_type::ok, "ok");
      });

  server.use_metrics();

  person_t person{};
  server.set_http_handler<GET>("/person", &person_t::foo, person);

  server.async_start();
  std::this_thread::sleep_for(300ms);  // wait for server start

  coro_http_client client{};
  auto result = co_await client.async_get("http://127.0.0.1:9001/get");
  assert(result.status == 200);
  assert(result.resp_body == "ok");
  for (auto [key, val] : result.resp_headers) {
    std::cout << key << ": " << val << "\n";
  }

  result = co_await client.async_get("/coro");
  assert(result.status == 200);

  client.add_str_part("hello", "form_data");
  client.add_str_part("test", "value");
  result =
      co_await client.async_upload_multipart("http://127.0.0.1:9001/form_data");
  assert(result.status == 200);
  assert(result.resp_body == "multipart finished");

  result = co_await client.async_get("/in_thread_pool");
  assert(result.status == 200);

  result = co_await client.async_post("/post", "post string",
                                      req_content_type::string);
  assert(result.status == 200);
  assert(result.resp_body == "post string");

  client.add_header("name", "tom");
  client.add_header("age", "20");
  result = co_await client.async_get("/headers");
  assert(result.status == 200);

  result = co_await client.async_get("/query?name=tom&age=20");
  assert(result.status == 200);

  result = co_await client.async_get(
      "http://127.0.0.1:9001/users/ultramarines/subscriptions/guilliman");
  assert(result.status == 200);

  // make sure you have install openssl and enable CINATRA_ENABLE_SSL
#ifdef CINATRA_ENABLE_SSL
  coro_http_client client2{};

  result = client2.post("https://baidu.com", "test", req_content_type::string);
  std::cout << result.resp_body << "\n";
  result.net_err.value() assert(result.status == 200);
#endif
}

void use_metric() {
  using namespace ylt::metric;
  auto c =
      std::make_shared<counter_t>("request_count", "request count",
                                  std::vector<std::string>{"method", "url"});
  auto failed = std::make_shared<gauge_t>(
      "not_found_request_count", "not found request count",
      std::vector<std::string>{"method", "code", "url"});
  auto total =
      std::make_shared<counter_t>("total_request_count", "total request count");

  auto h =
      std::make_shared<histogram_t>(std::string("test"), std::string("help"),
                                    std::vector{5.0, 10.0, 20.0, 50.0, 100.0});

  auto summary = std::make_shared<summary_t>(
      std::string("test_summary"), std::string("summary help"),
      summary_t::Quantiles{
          {0.5, 0.05}, {0.9, 0.01}, {0.95, 0.005}, {0.99, 0.001}});

  default_metric_manager::register_metric_dynamic(c);
  default_metric_manager::register_metric_dynamic(total);
  default_metric_manager::register_metric_dynamic(failed);
  default_metric_manager::register_metric_dynamic(h);
  default_metric_manager::register_metric_dynamic(summary);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> distr(1, 100);

  std::thread thd([&] {
    while (true) {
      c->inc({"GET", "/test"});
      total->inc();
      h->observe(distr(gen));
      summary->observe(distr(gen));
      std::this_thread::sleep_for(1s);
    }
  });
  thd.detach();

  coro_http_server server(1, 9001);
  server.set_default_handler(
      [&](coro_http_request &req,
          coro_http_response &resp) -> async_simple::coro::Lazy<void> {
        failed->inc({std::string(req.get_method()),
                     std::to_string((int)status_type::not_found),
                     std::string(req.get_url())});
        total->inc();
        resp.set_status_and_content(status_type::not_found, "not found");
        co_return;
      });

  server.set_http_handler<GET>(
      "/get", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ok");
        c->inc({std::string(req.get_method()), std::string(req.get_url())});
        total->inc();
      });

  server.set_http_handler<GET>(
      "/test", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ok");
        c->inc({std::string(req.get_method()), std::string(req.get_url())});
        total->inc();
      });

  server.set_http_handler<GET, POST>(
      "/", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "ok");
        total->inc();
      });

  server.set_http_handler<GET, POST>(
      "/metrics", [](coro_http_request &req, coro_http_response &resp) {
        resp.need_date_head(false);
        resp.set_status_and_content(status_type::ok, "");
      });
  server.sync_start();
}

void metrics_example() {
  auto get_req_counter = std::make_shared<counter_t>(
      "get_req_count", "get req count",
      std::map<std::string, std::string>{{"url", "/get"}});
  auto get_req_qps = std::make_shared<gauge_t>("get_req_qps", "get req qps");
  // default_metric_manager::register_metric_static(get_req_counter,
  // get_req_qps);
  int64_t last = 0;
  std::thread thd([&] {
    while (true) {
      std::this_thread::sleep_for(1s);
      auto value = get_req_counter->value({"/get"});
      get_req_qps->update(value - last);
      last = value;
    }
  });
  thd.detach();

  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/get", [&](coro_http_request &req, coro_http_response &resp) {
        // get_req_counter->inc({"/get"});
        resp.set_status_and_content(status_type::ok, "ok");
      });
  server.set_http_handler<GET>(
      "/", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "hello world");
      });
  server.use_metrics();
  server.sync_start();
}

async_simple::coro::Lazy<void> use_channel() {
  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "hello world");
      });
  server.use_metrics();
  server.async_start();
  std::this_thread::sleep_for(100ms);

  auto channel = std::make_shared<coro_io::channel<coro_http_client>>(
      coro_io::channel<coro_http_client>::create(
          {"127.0.0.1:9001"}, {.lba = coro_io::load_blance_algorithm::random}));
  std::string url = "http://127.0.0.1:9001/";
  co_await channel->send_request(
      [&url](coro_http_client &client,
             std::string_view host) -> async_simple::coro::Lazy<void> {
        auto data = co_await client.async_get(url);
        std::cout << data.net_err.message() << "\n";
        std::cout << data.resp_body << "\n";
      });
}

async_simple::coro::Lazy<void> use_pool() {
  coro_http_server server(1, 9001);
  server.set_http_handler<GET>(
      "/", [&](coro_http_request &req, coro_http_response &resp) {
        resp.set_status_and_content(status_type::ok, "hello world");
      });
  server.use_metrics();
  server.async_start();

  auto map = default_metric_manager::metric_map_static();
  for (auto &[k, m] : map) {
    std::cout << k << ", ";
    std::cout << m->help() << "\n";
  }

  std::string url = "http://127.0.0.1:9001/";

  auto pool = coro_io::client_pool<coro_http_client>::create(
      url, {std::thread::hardware_concurrency() * 2});

  std::atomic<size_t> count = 0;
  for (size_t i = 0; i < 10000; i++) {
    pool->send_request(
            [&](coro_http_client &client) -> async_simple::coro::Lazy<void> {
              auto data = co_await client.async_get(url);
              std::cout << data.resp_body << "\n";
            })
        .start([&](auto &&) {
          count++;
        });
  }

  while (count != 10000) {
    std::this_thread::sleep_for(5ms);
  }

  int size = pool->free_client_count();
  printf("current client count: %d, \n", size);
  co_return;
}

int main() {
  // use_metric();
  // metrics_example();
  async_simple::coro::syncAwait(use_channel());
  async_simple::coro::syncAwait(use_pool());
  async_simple::coro::syncAwait(basic_usage());
  async_simple::coro::syncAwait(use_aspects());
  async_simple::coro::syncAwait(static_file_server());
  async_simple::coro::syncAwait(use_websocket());
  async_simple::coro::syncAwait(chunked_upload_download());
  async_simple::coro::syncAwait(byte_ranges_download());
  return 0;
}