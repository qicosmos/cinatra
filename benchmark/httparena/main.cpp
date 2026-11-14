#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "cinatra/coro_http_server.hpp"
#include "iguana/json_reader.hpp"
#include "iguana/json_writer.hpp"

struct Rating {
  int64_t score;
  int64_t count;
};
REFLECTION(Rating, score, count);

struct Item {
  int64_t id;
  std::string name;
  std::string category;
  int64_t price;
  int64_t quantity;
  bool active;
  std::vector<std::string> tags;
  Rating rating;
};
REFLECTION(Item, id, name, category, price, quantity, active, tags, rating);

// The dataset outlives both servers. Borrow immutable fields while computing
// totals anew; the synchronous writer produces response-owned bytes.
struct ResponseItem {
  int64_t id;
  std::string_view name;
  std::string_view category;
  int64_t price;
  int64_t quantity;
  bool active;
  std::pmr::vector<std::string_view> tags;
  Rating rating;
  int64_t total;
};
REFLECTION(ResponseItem, id, name, category, price, quantity, active, tags,
           rating, total);

struct JsonResponse {
  std::pmr::vector<ResponseItem> items;
  size_t count;
};
REFLECTION(JsonResponse, items, count);

using namespace cinatra;
using async_simple::coro::Lazy;

namespace {
// Header spans are shared read-only. cinatra emits them in both buffer modes.
constinit std::array<http_header, 1> text_headers{
    {{"Content-Type", "text/plain; charset=UTF-8"}}};
constinit std::array<http_header, 2> json_headers{
    {{"Content-Type", "application/json; charset=UTF-8"},
     {"Vary", "Accept-Encoding"}}};

int64_t number(std::string_view value) {
  if (value.empty()) {
    return 0;
  }
  int64_t result = 0;
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}

size_t worker_count() {
  if (const auto *env = std::getenv("CINATRA_THREADS")) {
    if (auto count = number(env); count > 0) {
      return static_cast<size_t>(count);
    }
    throw std::runtime_error("CINATRA_THREADS must be positive");
  }
#ifdef __linux__
  cpu_set_t available;
  if (sched_getaffinity(0, sizeof(available), &available) == 0) {
    return static_cast<size_t>(CPU_COUNT(&available));
  }
#endif
  return std::max(1u, std::thread::hardware_concurrency());
}

int64_t query_sum(coro_http_request &request) {
  int64_t sum = 0;
  for (const auto &[name, value] : request.get_queries()) {
    sum += number(value);
  }
  return sum;
}

void text(coro_http_response &response, int64_t value) {
  response.add_header_span(text_headers);
  response.set_status_and_content(status_type::ok, std::to_string(value));
}

// cinatra exposes chunked bodies as a stream. Consume them through that API,
// owning each chunk before the next read invalidates its borrowed buffer.
Lazy<std::string> read_chunked_body(coro_http_request &request) {
  std::string body;
  while (true) {
    auto chunk = co_await request.get_conn()->read_chunked();
    if (chunk.ec) {
      throw std::system_error(chunk.ec);
    }
    if (chunk.eof) {
      break;
    }
    body.append(chunk.data);
  }
  co_return body;
}

void register_routes(coro_http_server &server, const std::vector<Item> &dataset,
                     bool multi_buffer = true) {
  server.set_default_response_headers(false);
  server.set_http_handler<GET>("/baseline11", [](coro_http_request &request,
                                                 coro_http_response &response) {
    request.get_conn()->set_multi_buf(false);
    text(response, query_sum(request));
  });
  server.set_http_handler<POST>("/baseline11",
                                [](coro_http_request &request,
                                   coro_http_response &response) -> Lazy<void> {
                                  request.get_conn()->set_multi_buf(false);
                                  auto sum = query_sum(request);
                                  if (request.is_chunked()) {
                                    auto body =
                                        co_await read_chunked_body(request);
                                    sum += number(body);
                                  }
                                  else {
                                    sum += number(request.get_body());
                                  }
                                  text(response, sum);
                                });
  server.set_http_handler<GET>(
      "/pipeline", [](coro_http_request &, coro_http_response &response) {
        response.add_header_span(text_headers);
        response.set_status_and_content_view(status_type::ok, "ok");
      });
  server.set_http_handler<GET>(
      "/delay/:ms",
      [](coro_http_request &request,
         coro_http_response &response) -> Lazy<void> {
        const auto ms = number(request.params_.at("ms"));
        if (ms > 0) {
          const auto duration = std::chrono::milliseconds(ms);
          co_await coro_io::sleep_for(duration,
                                      request.get_conn()->get_executor());
        }
        text(response, ms);
      });
  server.set_http_handler<GET>(
      "/json/:count", [&dataset, multi_buffer](coro_http_request &request,
                                               coro_http_response &response) {
        request.get_conn()->set_multi_buf(multi_buffer);
        const auto count = std::clamp<int64_t>(
            number(request.params_.at("count")), 0, dataset.size());
        const auto query = request.get_query_value("m");
        const auto multiplier = query.empty() ? 1 : number(query);
        // Rebuild the typed payload in request-local storage. This retains no
        // serialized data and has no shared reference counts between workers.
        std::array<std::byte, 16 * 1024> storage;
        std::pmr::monotonic_buffer_resource arena(storage.data(),
                                                  storage.size());
        JsonResponse result{std::pmr::vector<ResponseItem>(&arena),
                            static_cast<size_t>(count)};
        result.items.reserve(count);
        for (size_t i = 0; i < count; ++i) {
          const auto &item = dataset[i];
          std::pmr::vector<std::string_view> tags(&arena);
          tags.reserve(item.tags.size());
          for (const auto &tag : item.tags) {
            tags.push_back(tag);
          }
          result.items.push_back({item.id, item.name, item.category, item.price,
                                  item.quantity, item.active, std::move(tags),
                                  item.rating,
                                  item.price * item.quantity * multiplier});
        }
        response.add_header_span(json_headers);
        const auto accept = request.get_accept_encoding();
        // Empty client encoding permits forced gzip in the response API;
        // Arena requires an ordinary body when the request omits the header.
        const auto encoding =
            accept.empty() ? content_encoding::none : content_encoding::gzip;
        response.set_content(
            [&](auto &stream) {
              iguana::to_json(result, stream);
            },
            encoding, accept, 3);
      });
  server.set_http_handler<POST>(
      "/echo",
      [multi_buffer](coro_http_request &request,
                     coro_http_response &response) -> Lazy<void> {
        request.get_conn()->set_multi_buf(multi_buffer);
        response.add_header("Content-Type", "application/octet-stream");
        if (!request.is_chunked()) {
          // The connection retains the request body until its reply completes.
          response.set_status_and_content_view(status_type::ok,
                                               request.get_body());
          co_return;
        }
        auto body = co_await read_chunked_body(request);
        response.set_status_and_content(status_type::ok, std::move(body));
      });
  server.set_http_handler<GET>(
      "/ws",
      [](coro_http_request &request,
         coro_http_response &response) -> Lazy<void> {
        if (request.get_content_type() != content_type::websocket) {
          response.set_status_and_content(status_type::bad_request,
                                          "WebSocket upgrade required");
          co_return;
        }
        while (true) {
          auto message = co_await request.get_conn()->read_websocket();
          if (message.ec || message.type == ws_frame_type::WS_CLOSE_FRAME) {
            co_return;
          }
          if (message.type == ws_frame_type::WS_TEXT_FRAME ||
              message.type == ws_frame_type::WS_BINARY_FRAME) {
            const auto op = message.type == ws_frame_type::WS_BINARY_FRAME
                                ? opcode::binary
                                : opcode::text;
            if (co_await request.get_conn()->write_websocket(message.data,
                                                             op)) {
              co_return;
            }
          }
        }
      });
  server.set_max_http_body_size(30 * 1024 * 1024);
}
}  // namespace

int main() {
  try {
    const auto *path = std::getenv("DATASET_PATH");
    std::ifstream input(path ? path : "/data/dataset.json");
    if (!input) {
      throw std::runtime_error("Cannot open dataset.json");
    }
    std::string json((std::istreambuf_iterator<char>(input)), {});
    std::vector<Item> dataset;
    iguana::from_json(dataset, json);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (const auto error = pthread_sigmask(SIG_BLOCK, &signals, nullptr)) {
      throw std::system_error(error, std::generic_category());
    }
    const auto threads = worker_count();
    coro_http_server server(threads, 8080);
    register_routes(server, dataset);
    std::unique_ptr<coro_http_server> tls;
    const auto *cert_env = std::getenv("TLS_CERT");
    const auto *key_env = std::getenv("TLS_KEY");
    const auto *cert = cert_env ? cert_env : "/certs/server.crt";
    const auto *key = key_env ? key_env : "/certs/server.key";
    if (std::filesystem::exists(cert) && std::filesystem::exists(key)) {
      tls = std::make_unique<coro_http_server>(threads, 8081);
      const auto *multi = std::getenv("CINATRA_TLS_MULTI_BUFFER");
      register_routes(*tls, dataset, multi && number(multi) != 0);
      tls->init_ssl(cert, key);
      tls->async_start();
      if (const auto error = tls->get_errc()) {
        throw std::system_error(error);
      }
    }
    std::cout << "cinatra: " << threads << " workers per listener, "
              << dataset.size() << " dataset items" << std::endl;
    server.async_start();
    if (const auto error = server.get_errc()) {
      throw std::system_error(error);
    }
    int signal = 0;
    if (const auto error = sigwait(&signals, &signal)) {
      throw std::system_error(error, std::generic_category());
    }
    if (tls) {
      tls->stop();
    }
    server.stop();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
