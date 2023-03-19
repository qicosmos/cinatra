#include <iostream>

#include "../include/cinatra.hpp"

using namespace cinatra;

struct log_t {
  bool before(request &, response &) {
    std::cout << "before log" << std::endl;
    return true;
  }

  bool after(request &, response &res) {
    std::cout << "after log" << std::endl;
    res.add_header("aaaa", "bbcc");
    return true;
  }
};

struct person {
  void foo(request &, response &res) {
    std::cout << i << std::endl;
    res.render_string("ok");
  }

  void foo1(request &, response &res) {
    std::cout << i << std::endl;
    res.render_string("ok");
  }

  int i = 0;
};

void test_ssl_server() {
#ifdef CINATRA_ENABLE_SSL
  // you should open macro CINATRA_ENABLE_SSL at first
  http_ssl_server server(2);

  server.set_ssl_conf({"server.crt", "server.key"});
  int r = server.listen("0.0.0.0", "9001");
  if (r < 0) {
    return;
  }

  server.set_http_handler<GET, POST>("/", [](request &req, response &res) {
    auto str = req.get_conn<cinatra::SSL>()->remote_address();
    res.set_status_and_content(status_type::ok, "hello world from 9001");
  });

  server.run();
#endif
}

#ifdef CINATRA_ENABLE_SSL
void test_coro_http_client() {
  using namespace cinatra;
  coro_http_client client{};
  client.init_ssl("../../include/cinatra", "server.crt");
  auto data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
  data = client.get("https://www.bing.com");
  std::cout << data.resp_body << "\n";
}
#endif

void test_sync_client() {
  {
    std::string uri = "http://www.baidu.com";
    coro_http_client client{};
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.status);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.status);
  }

  {
    coro_http_client client{};
    std::string uri = "http://cn.bing.com";
    auto result = client.get(uri);
    assert(!result.net_err);
    print(result.status);

    result = client.post(uri, "hello", req_content_type::json);
    print(result.status);
  }
}

async_simple::coro::Lazy<void> test_async_client(coro_http_client &client) {
  std::string uri = "http://www.baidu.com";
  auto data = co_await client.async_get(uri);
  print(data.status);

  data = co_await client.async_get(uri);
  print(data.status);

  data = co_await client.async_post(uri, "hello", req_content_type::string);
  print(data.status);
}

async_simple::coro::Lazy<void> test_async_ssl_client(coro_http_client &client) {
#ifdef CINATRA_ENABLE_SSL
  std::string uri2 = "https://www.baidu.com";
  std::string uri3 = "https://cn.bing.com";
  coro_http_client client{};
  client.init_ssl("../../include/cinatra", "server.crt");
  data = co_await client.async_get(uri2);
  print(data.status);

  data = co_await client.async_get(uri3);
  print(data.status);
#endif
  co_return;
}

async_simple::coro::Lazy<void> test_download() {
  coro_http_client client{};
  std::string uri =
      "http://www.httpwatch.com/httpgallery/chunked/chunkedimage.aspx";
  std::string filename = "test.jpg";

  std::error_code ec{};
  std::filesystem::remove(filename, ec);
  auto r = co_await client.async_download(uri, filename);
  assert(!r.net_err);
  assert(r.status == 200);
  std::cout << "download finished\n";
}

async_simple::coro::Lazy<void> test_upload() {
  std::string uri = "http://example.com/";
  coro_http_client client{};
  auto result = co_await client.async_upload(uri, "test", "yourfile.jpg");
  print(result.status);
  std::cout << "upload finished\n";

  client.add_str_part("hello", "coro_http_client");
  client.add_file_part("test", "yourfile.jpg");
  result = co_await client.async_upload(uri, "test", "yourfile.jpg");
  print(result.status);
  std::cout << "upload finished\n";
}

async_simple::coro::Lazy<void> test_websocket() {
  coro_http_client client{};
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
  });
  client.on_ws_msg([](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      return;
    }
    std::cout << data.resp_body << std::endl;
  });

  bool r = co_await client.async_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result =
      co_await client.async_send_ws("hello websocket");  // mask as default.
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */ false);
  std::cout << result.status << "\n";
  result = co_await client.async_send_ws_close("ws close");
  std::cout << result.status << "\n";
}

void test_smtp_client() {
  asio::io_context io_context;
#ifdef CINATRA_ENABLE_SSL
  auto client = cinatra::smtp::get_smtp_client<cinatra::SSL>(io_context);
#else
  auto client = cinatra::smtp::get_smtp_client<cinatra::NonSSL>(io_context);
#endif
  smtp::email_server server{};
  server.server = "smtp.163.com";
  server.port = client.IS_SSL ? "465" : "25";
  server.user = "your_email@163.com";
  server.password = "your_email_password";

  smtp::email_data data{};
  data.filepath = "";  // some file as attachment.
  data.from_email = "your_email@163.com";
  data.to_email.push_back("to_some_email@163.com");
  // data.to_email.push_back("to_more_email@example.com");
  data.subject = "it is a test from cinatra smtp";
  data.text = "Hello cinatra smtp client";

  client.set_email_server(server);
  client.set_email_data(data);

  client.start();

  std::error_code ec;
  io_context.run(ec);
}

class qps {
 public:
  void increase() { counter_.fetch_add(1, std::memory_order_release); }

  qps() : counter_(0) {
    thd_ = std::thread([this] {
      while (!stop_) {
        std::cout << "qps: " << counter_.load(std::memory_order_acquire)
                  << '\n';
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // counter_.store(0, std::memory_order_release);
      }
    });
  }

  ~qps() {
    stop_ = true;
    thd_.join();
  }

 private:
  bool stop_ = false;
  std::thread thd_;
  std::atomic<uint32_t> counter_;
};

int main() {
  // test_coro_http_client();
  // test_smtp_client();
  {
    test_sync_client();
    coro_http_client client{};
    async_simple::coro::syncAwait(test_async_client(client));

    coro_http_client ssl_client{};
    async_simple::coro::syncAwait(test_async_ssl_client(ssl_client));
  }

  // test_ssl_server();
  // test_download();
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    // LOG_INFO << "listen failed";
    return -1;
  }

  // server.on_connection([](auto conn) { return true; });
  server.set_http_handler<GET, POST>("/", [](request &, response &res) mutable {
    res.set_status_and_content(status_type::ok, "hello world");
    // res.set_status_and_content(status_type::ok, std::move(str));
  });

  server.set_http_handler<GET>("/plaintext", [](request &, response &res) {
    // res.set_status_and_content<status_type::ok,
    // res_content_type::string>("Hello, World!");
    res.set_status_and_content(status_type::ok, "Hello, World!",
                               req_content_type::string);
  });

  // server.set_http_handler<GET, POST>("/delay", [](request& req, response&
  // res) {
  //    auto conn = req.get_conn<NonSSL>();

  //    //monitor an async operation to test response delay
  //    std::thread thd([conn, &res] {
  //        std::this_thread::sleep_for(std::chrono::seconds(3));
  //        if (!conn->has_close()) {
  //            res.set_status_and_content(status_type::ok, "hello world");
  //            conn->response_now();
  //        }
  //        else {
  //            std::cout << "has closed\n";
  //        }
  //    });
  //    thd.detach();
  //    res.set_delay(true);
  //});

  //	person p{ 2 };
  //	server.set_http_handler<GET, POST>("/a", &person::foo, enable_cache{
  // false }, log_t{});
  ////	server.set_http_handler<GET, POST>("/b", &person::foo1, log_t{},
  /// enable_cache{ false });
  //
  //    server.set_http_handler<GET, POST>("/string", [](request& req, response&
  //    res) {
  //        res.render_string(std::to_string(std::time(nullptr)));
  //    },enable_cache{true});
  //
  //    server.set_http_handler<GET, POST>("/404", [](request& req, response&
  //    res) {
  //        res.render_404();
  //    },enable_cache{false});
  //
  //    server.set_http_handler<GET, POST>("/404_custom", [](request& req,
  //    response& res) {
  //        res.render_404("./404.html");
  //    },enable_cache{false});
  //
  //	server.set_http_handler<GET, POST>("/login", [](request& req, response&
  // res) { 		auto session = res.start_session();
  // session->set_data("userid", std::string("1"));
  // session->set_max_age(-1);
  // res.set_status_and_content(status_type::ok, "login");
  //	},enable_cache{false});
  //
  //	server.set_http_handler<GET, POST>("/islogin", [](request& req,
  // response& res) { 		auto ptr = req.get_session(); 		auto
  // session = ptr.lock(); 		if (session == nullptr ||
  // session->get_data<std::string>("userid") != "1") {
  //			res.set_status_and_content(status_type::ok, "没有登录",
  // res_content_type::string); 			return;
  //		}
  //		res.set_status_and_content(status_type::ok, "已经登录",
  // res_content_type::string);
  //	},enable_cache{false});
  //
  //	server.set_http_handler<GET, POST>("/html", [](request& req, response&
  // res) {
  //        res.set_attr("number",1024);
  //        res.set_attr("test_text","hello,world");
  //        res.set_attr("header_text","你好 cinatra");
  //		res.render_view("./www/test.html");
  //	});
  //
  //	server.set_http_handler<GET, POST,OPTIONS>("/json", [](request& req,
  // response& res) { 		nlohmann::json json;
  //        res.add_header("Access-Control-Allow-Origin","*");
  //		if(req.get_method()=="OPTIONS"){
  //            res.add_header("Access-Control-Allow-Headers","Authorization");
  //            res.render_string("");
  //		}else{
  //            json["abc"] = "abc";
  //            json["success"] = true;
  //            json["number"] = 100.005;
  //            json["name"] = "中文";
  //            json["time_stamp"] = std::time(nullptr);
  //            res.render_json(json);
  //		}
  //	});
  //
  //	server.set_http_handler<GET,POST>("/redirect",[](request& req, response&
  // res){ 		res.redirect("http://www.baidu.com"); //
  // res.redirect("/json");
  //	});
  //
  //	server.set_http_handler<GET, POST>("/pathinfo/*", [](request& req,
  // response& res) { 		auto s = req.get_query_value(0);
  //		res.render_string(std::string(s.data(), s.length()));
  //	});
  //
  //	server.set_http_handler<GET, POST>("/restype", [](request& req,
  // response& res) { 		auto type = req.get_query_value("type");
  // auto res_type = cinatra::res_content_type::string; 		if (type
  // == "html")
  //		{
  //			res_type = cinatra::res_content_type::html;
  //		}
  //		else if (type == "json") {
  //			res_type = cinatra::res_content_type::json;
  //		}
  //		else if (type == "string") {
  //			//do not anything;
  //		}
  //		res.set_status_and_content(status_type::ok, "<a
  // href='http://www.baidu.com'>hello world 百度</a>", res_type);
  //	});
  //
  //	server.set_http_handler<GET, POST>("/getzh", [](request& req, response&
  // res) { 		auto zh = req.get_query_value("zh");
  //		res.render_string(std::string(zh.data(),zh.size()));
  //	});
  //
  //	server.set_http_handler<GET, POST>("/gzip", [](request& req, response&
  // res) { 		auto body = req.body(); 		std::cout <<
  // body.data()
  // <<
  // std::endl; 		res.set_status_and_content(status_type::ok,
  // "hello world", req_content_type::none, content_encoding::gzip);
  // 	});
  //
  //
  //	server.set_http_handler<GET, POST>("/test", [](request& req, response&
  // res) { 		auto name = req.get_header_value("name"); if
  //(name.empty()) { 			res.render_string("no name");
  // return;
  //		}
  //
  //		auto id = req.get_query_value("id");
  //		if (id.empty()) {
  //			res.render_404();
  //			return;
  //		}
  //		res.render_string("hello world");
  //	});
  //
  //	//aspect
  //	server.set_http_handler<GET, POST>("/aspect", [](request& req, response&
  // res) { 		res.render_string("hello world");
  //	}, check{}, log_t{});
  //
  ////web socket
  // server.set_http_handler<GET, POST>("/ws", [](request& req, response& res) {
  //	assert(req.get_content_type() == content_type::websocket);

  //	req.on(ws_open, [](request& req){
  //		std::cout << "websocket start" << std::endl;
  //	});

  //	req.on(ws_message, [](request& req) {
  //		auto part_data = req.get_part_data();
  //		//echo
  //		std::string str = std::string(part_data.data(),
  // part_data.length()); req.get_conn()->send_ws_string(std::move(str));
  // std::cout << part_data.data() << std::endl;
  //	});

  //	req.on(ws_error, [](request& req) {
  //		std::cout << "websocket pack error or network error" <<
  // std::endl;
  //	});
  //});

  //	server.set_http_handler<GET, POST>("/vue_html", [](request& req,
  // response& res) { 		res.render_raw_view("./www/index.html");
  //	});
  //
  //	server.set_http_handler<GET, POST>("/vue_demo", [](request& req,
  // response& res) { 		res.render_raw_view("./www/dist/index.html");
  //	});
  //
  // http upload(multipart)
  server.set_http_handler<GET, POST>(
      "/upload_multipart", [](request &req, response &res) {
        assert(req.get_content_type() == content_type::multipart);
        auto &files = req.get_upload_files();
        for (auto &file : files) {
          std::cout << file.get_file_path() << " " << file.get_file_size()
                    << std::endl;
        }
        res.render_string("multipart finished");
      });
  //
  //	//http upload(octet-stream)
  //	server.set_http_handler<GET, POST>("/upload_octet_stream", [](request&
  // req, response& res) { 		assert(req.get_content_type() ==
  // content_type::octet_stream); 		auto& files =
  // req.get_upload_files();
  // for (auto& file : files) { 			std::cout <<
  // file.get_file_path()
  // << " " << file.get_file_size() << std::endl;
  //		}
  //		res.render_string("octet-stream finished");
  //	});

  // chunked download
  // http://127.0.0.1:8080/assets/show.jpg
  // cinatra will send you the file, if the file is big file(more than 5M) the
  // file will be downloaded by chunked

  std::thread stop_thd([&] {
    std::string str;
    std::cin >> str;
    if (str == "quit") {
      server.stop();
      std::cout << "quit server\n";
    }
  });
  server.run();
  stop_thd.join();
  return 0;
}