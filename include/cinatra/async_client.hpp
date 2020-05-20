#pragma once
#include <string>
#include <vector>
#include <tuple>
#include <future>
#include <atomic>
#include <fstream>
#include <mutex>
#include <deque>
#include <string_view>
#include "use_asio.hpp"
#include "uri.hpp"
//#include "util.hpp"
#include "http_parser.hpp"
#include "itoa_jeaiii.hpp"
//#include "common.h"

#ifdef CINATRA_ENABLE_SSL
#ifdef ASIO_STANDALONE
#include <asio/ssl.hpp>
#else
#include <boost/asio/ssl.hpp>
#endif
#endif

namespace cinatra {
    struct callback_data {
        boost::system::error_code ec;
        int status;
        std::string_view resp_body;
        std::pair<phr_header*, size_t> resp_headers;
    };
    using callback_t = std::function<void(callback_data)>;

    inline static std::string INVALID_URI = "invalid_uri";
    inline static std::string REQUEST_TIMEOUT = "request timeout";
    inline static std::string MULTIPLE_REQUEST = "last async request not finished";
    inline static std::string METHOD_ERROR = "method error";
    inline static std::string INVALID_FILE_PATH = "invalid file path";
    inline static std::string OPEN_FAILED = "open file failed";
    inline static std::string FILE_SIZE_ERROR = "filesize error";
    inline static std::string RESP_PARSE_ERROR = "http response parse error";
    inline static std::string INVALID_CHUNK_SIZE = "invalid chunk size";
    inline static std::string READ_TIMEOUT = "read timeout";

    class async_client : public std::enable_shared_from_this<async_client> {
    public:
        async_client(boost::asio::io_service& ios) :
            ios_(ios), resolver_(ios), socket_(ios), timer_(ios) {
            future_ = read_finished_.get_future();
        }

        ~async_client() {
            close();
        }

        callback_data get(std::string uri) {
            return request(http_method::GET, std::move(uri), res_content_type::json, timeout_seconds_);
        }

        callback_data get(std::string uri, size_t seconds) {
            return request(http_method::GET, std::move(uri), res_content_type::json, seconds);
        }

        callback_data get(std::string uri, res_content_type type) {
            return request(http_method::GET, std::move(uri), type, timeout_seconds_);
        }

        callback_data get(std::string uri, size_t seconds, res_content_type type) {
            return request(http_method::GET, std::move(uri), type, seconds);
        }

        callback_data get(std::string uri, res_content_type type, size_t seconds) {
            return request(http_method::GET, std::move(uri), type, seconds);
        }

        callback_data post(std::string uri, std::string body) {
            return request(http_method::POST, std::move(uri), res_content_type::json, timeout_seconds_, std::move(body));
        }

        callback_data post(std::string uri, std::string body, res_content_type type) {
            return request(http_method::POST, std::move(uri), type, timeout_seconds_, std::move(body));
        }

        callback_data post(std::string uri, std::string body, size_t seconds) {
            return request(http_method::POST, std::move(uri), res_content_type::json, seconds, std::move(body));
        }

        callback_data post(std::string uri, std::string body, res_content_type type, size_t seconds) {
            return request(http_method::POST, std::move(uri), type, seconds, std::move(body));
        }

        callback_data post(std::string uri, std::string body, size_t seconds, res_content_type type) {
            return request(http_method::POST, std::move(uri), type, seconds, std::move(body));
        }

        callback_data request(http_method method, std::string uri, res_content_type type = res_content_type::json, size_t seconds = 15, std::string body = "") {
            promise_ = std::make_shared<std::promise<callback_data>>();
            async_request(http_method::POST, std::move(uri), nullptr, type, seconds, std::move(body));
            auto future = promise_->get_future();
            auto status = future.wait_for(std::chrono::seconds(seconds));
            if (status == std::future_status::timeout) {
                promise_->set_value({ boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404, REQUEST_TIMEOUT });
            }
            auto result = future.get();
            promise_ = nullptr;
            return result;
        }

        void async_get(std::string uri, callback_t cb) {
            async_request(http_method::GET, std::move(uri), std::move(cb), res_content_type::json, timeout_seconds_);
        }

        void async_get(std::string uri, res_content_type type, callback_t cb) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, timeout_seconds_);
        }

        void async_get(std::string uri, callback_t cb, res_content_type type) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, timeout_seconds_);
        }

        void async_get(std::string uri, callback_t cb, size_t seconds) {
            async_request(http_method::GET, std::move(uri), std::move(cb), res_content_type::json, seconds);
        }

        void async_get(std::string uri, callback_t cb, res_content_type type, size_t seconds) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, seconds);
        }

        void async_get(std::string uri, callback_t cb, size_t seconds, res_content_type type) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, seconds);
        }

        void async_get(std::string uri, size_t seconds, callback_t cb) {
            async_request(http_method::GET, std::move(uri), std::move(cb), res_content_type::json, seconds);
        }

        void async_get(std::string uri, res_content_type type, size_t seconds, callback_t cb) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, seconds);
        }

        void async_get(std::string uri, size_t seconds, res_content_type type, callback_t cb) {
            async_request(http_method::GET, std::move(uri), std::move(cb), type, seconds);
        }

        void async_post(std::string uri, std::string body, callback_t cb) {
            async_request(http_method::POST, std::move(uri), std::move(cb), res_content_type::json, timeout_seconds_, std::move(body));
        }

        void async_post(std::string uri, std::string body, callback_t cb, res_content_type type) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, timeout_seconds_, std::move(body));
        }

        void async_post(std::string uri, std::string body, callback_t cb, size_t seconds) {
            async_request(http_method::POST, std::move(uri), std::move(cb), res_content_type::json, seconds, std::move(body));
        }

        void async_post(std::string uri, std::string body, callback_t cb, res_content_type type, size_t seconds) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, seconds, std::move(body));
        }

        void async_post(std::string uri, std::string body, callback_t cb, size_t seconds, res_content_type type) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, seconds, std::move(body));
        }

        void async_post(std::string uri, std::string body, res_content_type type, callback_t cb) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, timeout_seconds_, std::move(body));
        }

        void async_post(std::string uri, std::string body, size_t seconds, callback_t cb) {
            async_request(http_method::POST, std::move(uri), std::move(cb), res_content_type::json, seconds, std::move(body));
        }

        void async_post(std::string uri, std::string body, res_content_type type, size_t seconds, callback_t cb) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, seconds, std::move(body));
        }

        void async_post(std::string uri, std::string body, size_t seconds, res_content_type type, callback_t cb) {
            async_request(http_method::POST, std::move(uri), std::move(cb), type, seconds, std::move(body));
        }

        void async_request(http_method method, std::string uri, callback_t cb, res_content_type type = res_content_type::json, size_t seconds = 15, std::string body = "") {
            if (!promise_) {//just for async request, guard continuous async request, it's not allowed; async request must be after last one finished!
                if (in_progress_) {
                    if(cb)
                        cb({ boost::asio::error::make_error_code(boost::asio::error::basic_errors::in_progress), 404, MULTIPLE_REQUEST });
                    return;
                }
                else {
                    in_progress_ = true;
                }
            }            

            if (method != http_method::POST && !body.empty()) {
                set_error_value(cb, boost::asio::error::basic_errors::invalid_argument, METHOD_ERROR);
                return;
            }

            bool init = last_uri_.empty();
            bool need_reset = !init && (last_uri_ != uri);
            
            if (need_reset) {
                close(false);
                //wait read finish
                if (future_.valid()) {
                    future_.wait();
                }

                read_finished_ = {};
                reset_socket();
            }

            auto [r, u] = get_uri(uri);
            if (!r) {
                set_error_value(cb, boost::asio::error::basic_errors::invalid_argument, INVALID_URI);          
                return;
            }

            last_uri_ = std::move(uri);
            timeout_seconds_ = seconds;
            res_content_type_ = type;
            cb_ = std::move(cb);
            context ctx(u, method, std::move(body));
            if (promise_) {
                weak_ = promise_;
            }
            if (has_connected_) {
                do_write(std::move(ctx));
            }
            else {
                async_connect(std::move(ctx));
            }            
        }

        void download(std::string src_file, std::string dest_file, callback_t cb, size_t seconds = 60) {
            auto parant_path = fs::path(dest_file).parent_path();
            std::error_code code;
            fs::create_directories(parant_path, code);
            if (code) {
                cb_ = std::move(cb);
                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), INVALID_FILE_PATH);
                return;
            }

            download_file_ = std::make_shared<std::ofstream>(dest_file, std::ios::binary);
            if (!download_file_->is_open()) {
                cb_ = std::move(cb);
                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), OPEN_FAILED);
                return;
            }

            async_get(std::move(src_file), std::move(cb), res_content_type::none, seconds);
        }

        void download(std::string src_file, std::function<void(boost::system::error_code, std::string_view)> chunk, size_t seconds = 60) {
            on_chunk_ = std::move(chunk);
            async_get(std::move(src_file), nullptr, res_content_type::none, seconds);
        }

        void upload(std::string uri, std::string filename, callback_t cb, size_t seconds = 60) {
            upload(std::move(uri), std::move(filename), 0, std::move(cb), seconds);
        }

        void upload(std::string uri, std::string filename, size_t start, callback_t cb, size_t seconds = 60) {
            multipart_str_ = std::move(filename);
            start_ = start;
            async_request(http_method::POST, uri, std::move(cb), res_content_type::multipart, seconds, "");
        }

        void add_header(std::string key, std::string val) {
            if (key.empty())
                return;

            if (key == "Host")
                return;

            headers_.emplace_back(std::move(key), std::move(val));
        }

        void add_header_str(std::string pair_str) {
            if (pair_str.empty())
                return;

            if (pair_str.find("Host:") != std::string::npos)
                return;

            header_str_.append(pair_str);
        }

        void clear_headers() {
            if (!headers_.empty()) {
                headers_.clear();
            }

            if (!header_str_.empty()) {
                header_str_.clear();
            }
        }

        std::pair<phr_header*, size_t> get_resp_headers() {
            if (!copy_headers_.empty())
                parser_.set_headers(copy_headers_);

            return parser_.get_headers();
        }

        std::string_view get_header_value(std::string_view key) {
            return parser_.get_header_value(key);
        }

    private:
        void callback(const boost::system::error_code& ec) {
            callback(ec, 404, "");
        }

        void callback(const boost::system::error_code& ec, std::string error_msg) {
            callback(ec, 404, std::move(error_msg));
        }

        void callback(const boost::system::error_code& ec, int status) {
            callback(ec, status, "");
        }

        void callback(const boost::system::error_code& ec, int status, std::string_view result) {
            if (auto sp = weak_.lock(); sp) {
                sp->set_value({ ec, status, result, get_resp_headers() });
                return;
            }

            if (cb_) {
                cb_({ ec, status, result, get_resp_headers() });
                cb_ = nullptr;
            }

            if (on_chunk_) {
                on_chunk_(ec, result);
            }

            in_progress_ = false;
        }

        std::pair<bool, uri_t> get_uri(const std::string& uri) {
            uri_t u;
            if (!u.parse_from(uri.data())) {
                if (u.schema.empty())
                    return { false, {} };

                auto new_uri = url_encode(uri);

                if (!u.parse_from(new_uri.data())) {
                    return { false, {} };
                }
            }

            if (u.schema == "https"sv) {
#ifdef CINATRA_ENABLE_SSL
                upgrade_to_ssl(nullptr);
#else
                //please open CINATRA_ENABLE_SSL before request https!
                assert(false);
#endif
            }
            else {
#ifdef CINATRA_ENABLE_SSL
                close(false);
#endif
            }

            return { true, u };
        }

        void async_connect(context ctx) {
            reset_timer();
            boost::asio::ip::tcp::resolver::query query(ctx.host, ctx.port);
            resolver_.async_resolve(query, [this, self = this->shared_from_this(), ctx = std::move(ctx)]
            (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator& it) {
                if (ec) {                   
                    callback(ec);
                    return;
                }

                boost::asio::async_connect(socket_, it, [this, self = shared_from_this(), ctx = std::move(ctx)]
                (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator&) {
                    cancel_timer();
                    if (!ec) {
                        has_connected_ = true;
                        if (is_ssl()) {
                            handshake(std::move(ctx));
                            return;
                        }

                        do_read_write(ctx);
                    }
                    else {
                        callback(ec);
                        close();
                    }
                });
            });
        }

        void do_read_write(const context& ctx) {
            boost::system::error_code error_ignored;
            socket_.set_option(boost::asio::ip::tcp::no_delay(true), error_ignored);            
            do_read();
            do_write(ctx);
        }

        void do_write(const context& ctx) {
            if (res_content_type_ == res_content_type::multipart) {
                send_multipart_msg(ctx);
            }
            else {
                send_msg(ctx);
            }
        }

        void send_msg(const context& ctx) {
            write_msg_ = build_write_msg(ctx);
            async_write(write_msg_, [this, self = shared_from_this()](const boost::system::error_code& ec, const size_t length) {
                if (ec) {
                    callback(ec);
                    close();
                    return;
                }
            });
        }

        void send_multipart_msg(const context& ctx) {
            auto filename = std::move(multipart_str_);
            auto file = std::make_shared<std::ifstream>(filename, std::ios::binary);
            if (!file) {
                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), INVALID_FILE_PATH);
                return;
            }

            std::error_code ec;
            size_t size = fs::file_size(filename, ec);
            if (ec || start_ == -1) {
                file->close();
                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), FILE_SIZE_ERROR);
                return;
            }

            if (start_ > 0) {
                file->seekg(start_);
            }

            auto left_file_size = size - start_;
            header_str_.append("Content-Type: multipart/form-data; boundary=").append(BOUNDARY);
            auto multipart_str = multipart_file_start(fs::path(filename).filename().string());
            auto write_str = build_write_msg(ctx, total_multipart_size(left_file_size, multipart_str.size()));
            write_str.append(multipart_str);
            multipart_str_ = std::move(write_str);
            send_file_data(std::move(file));
        }

        void handshake(context ctx) {
#ifdef CINATRA_ENABLE_SSL
            auto self = this->shared_from_this();
            ssl_stream_->async_handshake(boost::asio::ssl::stream_base::client,
                [this, self, ctx = std::move(ctx)](const boost::system::error_code& ec) {
                if (!ec) {
                    do_read_write(ctx);
                }
                else {
                    callback(ec);
                    close();
                }
            });
#endif
        }
        
        std::string build_write_msg(const context& ctx, size_t content_len = 0) {
            std::string write_msg(method_name(ctx.method));
            //can be optimized here
            write_msg.append(" ").append(ctx.path);
            if (!ctx.query.empty()) {
                write_msg.append("?").append(ctx.query);
            }
            write_msg.append(" HTTP/1.1\r\nHost:").
                append(ctx.host).append("\r\n");

            if (!header_str_.empty()) {
                if (header_str_.find("Content-Type") == std::string::npos) {
                    auto type_str = get_content_type_str(res_content_type_);
                    if (!type_str.empty()) {
                        headers_.emplace_back("Content-Type", std::move(type_str));
                    }
                }
            }      

            bool has_connection = false;
            //add user header
            if (!headers_.empty()) {
                for (auto& pair : headers_) {
                    if (pair.first == "Connection") {
                        has_connection = true;
                    }
                    write_msg.append(pair.first).append(": ").append(pair.second).append("\r\n");
                }
            }

            if (!header_str_.empty()) {
                if (header_str_.find("Connection")!=std::string::npos) {
                    has_connection = true;
                }
                write_msg.append(header_str_).append("\r\n");
            }

            //add content
            if (!ctx.body.empty()) {
                char buffer[20];
                auto p = i32toa_jeaiii((int)ctx.body.size(), buffer);

                write_msg.append("Content-Length: ").append(buffer, p - buffer).append("\r\n");
            }
            else {
                if (ctx.method == http_method::POST) {
                    if (content_len > 0) {
                        char buffer[20];
                        auto p = i32toa_jeaiii((int)content_len, buffer);

                        write_msg.append("Content-Length: ").append(buffer, p - buffer).append("\r\n");
                    }
                    else {
                        write_msg.append("Content-Length: 0\r\n");
                    }
                }
            }

            if (!has_connection) {
                write_msg.append("Connection: keep-alive\r\n");
            }

            write_msg.append("\r\n");

            if (!ctx.body.empty()) {
                write_msg.append(std::move(ctx.body));
            }

            return write_msg;
        }

        void do_read() {
            reset_timer();
            async_read_until(TWO_CRCF, [this, self = shared_from_this()](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    //parse header
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    size_t buf_size = read_buf_.size();
                    int ret = parser_.parse_response(data_ptr, size, 0);
                    read_buf_.consume(size);
                    if (ret < 0) {
                        callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404,
                            RESP_PARSE_ERROR);
                        if (buf_size > size) {
                            read_buf_.consume(buf_size - size);
                        }

                        read_or_close(parser_.keep_alive());
                        return;
                    }

                    bool is_chunked = parser_.is_chunked();

                    if (is_chunked) {
                        copy_headers();
                        //read_chunk_header
                        read_chunk_head(parser_.keep_alive());
                    }
                    else {
                        if (parser_.body_len() == 0) {
                            callback(ec, parser_.status());

                            read_or_close(parser_.keep_alive());
                            return;
                        }

                        size_t content_len = (size_t)parser_.body_len();
                        if ((size_t)parser_.total_len() <= buf_size) {
                            callback(ec, parser_.status(), { data_ptr + parser_.header_len(), content_len });
                            read_buf_.consume(content_len);

                            read_or_close(parser_.keep_alive());
                            return;
                        }

                        size_t size_to_read = content_len - read_buf_.size();
                        copy_headers();
                        do_read_body(parser_.keep_alive(), parser_.status(), size_to_read);
                    }
                }
                else {
                    callback(ec);
                    close();

                    auto status = future_.wait_for(std::chrono::seconds(0));
                    if (status == std::future_status::ready) {
                        future_.wait();
                        read_finished_ = {};
                    }

                    read_finished_.set_value(true);
                }
            });
        }

        void do_read_body(bool keep_alive, int status, size_t size_to_read) {
            reset_timer();
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive, status](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    size_t data_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());

                    callback(ec, status, { data_ptr, data_size });

                    read_buf_.consume(data_size);

                    read_or_close(keep_alive);
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void read_or_close(bool keep_alive) {
            if (keep_alive) {
                do_read();
            }
            else {
                close();
            }
        }

        void read_chunk_head(bool keep_alive) {
            reset_timer();
            async_read_until(CRCF, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    size_t buf_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    std::string_view size_str(data_ptr, size - CRCF.size());
                    auto chunk_size = hex_to_int(size_str);
                    if (chunk_size < 0) {
                        callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404,
                            INVALID_CHUNK_SIZE);
                        read_or_close(keep_alive);
                        return;
                    }

                    read_buf_.consume(size);

                    if (chunk_size == 0) {
                        if (read_buf_.size() < CRCF.size()) {
                            read_buf_.consume(read_buf_.size());
                            read_chunk_body(keep_alive, CRCF.size() - read_buf_.size());
                        }
                        else {
                            read_buf_.consume(CRCF.size());
                            read_chunk_body(keep_alive, 0);
                        }
                        return;
                    }

                    if ((size_t)chunk_size <= read_buf_.size()) {
                        const char* data = boost::asio::buffer_cast<const char*>(read_buf_.data());
                        append_chunk(std::string_view(data, chunk_size));
                        read_buf_.consume(chunk_size + CRCF.size());
                        read_chunk_head(keep_alive);
                        return;
                    }

                    size_t extra_size = read_buf_.size();
                    size_t size_to_read = chunk_size - extra_size;
                    const char* data = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    append_chunk({ data, extra_size });
                    read_buf_.consume(extra_size);

                    read_chunk_body(keep_alive, size_to_read + CRCF.size());
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void read_chunk_body(bool keep_alive, size_t size_to_read) {
            reset_timer();
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
                cancel_timer();
                if (!ec) {
                    if (size <= CRCF.size()) {
                        //finish all chunked
                        read_buf_.consume(size);
                        callback(ec, 200, chunked_result_);
                        clear_chunk_buffer();
                        do_read();
                        return;
                    }

                    size_t buf_size = read_buf_.size();
                    const char* data_ptr = boost::asio::buffer_cast<const char*>(read_buf_.data());
                    append_chunk({ data_ptr, size - CRCF.size() });
                    read_buf_.consume(size);
                    read_chunk_head(keep_alive);
                }
                else {
                    callback(ec);
                    close();
                }
            });
        }

        void append_chunk(std::string_view chunk) {
            if (on_chunk_) {
                on_chunk_({}, chunk);
                return;
            }

            if (download_file_) {
                download_file_->write(chunk.data(), chunk.size());
            }
            else {
                chunked_result_.append(chunk);
            }
        }

        void clear_chunk_buffer() {
            if (download_file_) {
                download_file_->close();
            }
            else {
                if (!chunked_result_.empty()) {
                    chunked_result_.clear();
                }
            }
        }

        template<typename Handler>
        void async_read(size_t size_to_read, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read(*ssl_stream_, read_buf_, boost::asio::transfer_exactly(size_to_read), std::move(handler));
#endif
            }
            else {
                boost::asio::async_read(socket_, read_buf_, boost::asio::transfer_exactly(size_to_read), std::move(handler));
            }
        }

        template<typename Handler>
        void async_read_until(const std::string& delim, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_read_until(*ssl_stream_, read_buf_, delim, std::move(handler));
#endif
            }
            else {
                boost::asio::async_read_until(socket_, read_buf_, delim, std::move(handler));
            }
        }

        template<typename Handler>
        void async_write(const std::string& msg, Handler handler) {
            if (is_ssl()) {
#ifdef CINATRA_ENABLE_SSL
                boost::asio::async_write(*ssl_stream_, boost::asio::buffer(msg), std::move(handler));
#endif
            }
            else {
                boost::asio::async_write(socket_, boost::asio::buffer(msg), std::move(handler));
            }
        }

        void close(bool close_ssl = true) {
            boost::system::error_code ec;
            if (close_ssl) {
#ifdef CINATRA_ENABLE_SSL
                if (ssl_stream_) {
                    ssl_stream_->shutdown(ec);
                    ssl_stream_ = nullptr;
                }
#endif
            }

            if (!has_connected_)
                return;

            has_connected_ = false;
            timer_.cancel(ec);
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

        void reset_timer() {
            if (timeout_seconds_ == 0 || promise_) {
                return;
            }

            auto self(this->shared_from_this());
            timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
            timer_.async_wait([this, self](const boost::system::error_code& ec) {
                if (ec) {
                    return;
                }

                callback(boost::asio::error::make_error_code(boost::asio::error::basic_errors::timed_out), 404, READ_TIMEOUT);
                close(false); //don't close ssl now, close ssl when read/write error
                if (download_file_) {
                    download_file_->close();
                }
            });
        }

        void cancel_timer() {
            if (!cb_) {
                return; //just for async request
            }

            if (timeout_seconds_ == 0 || promise_) {
                return;
            }

            timer_.cancel();
        }

        bool is_ssl() const {
#ifdef CINATRA_ENABLE_SSL
            return ssl_stream_ != nullptr;
#else
            return false;
#endif
        }

#ifdef CINATRA_ENABLE_SSL
        void upgrade_to_ssl(std::function<void(boost::asio::ssl::context&)> ssl_context_callback) {
            if (ssl_stream_)
                return;

            boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);
            ssl_context.set_default_verify_paths();
            boost::system::error_code ec;
            ssl_context.set_options(boost::asio::ssl::context::default_workarounds, ec);
            if (ssl_context_callback) {
                ssl_context_callback(ssl_context);
            }
            ssl_stream_ = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>>(socket_, ssl_context);
            //verify peer TODO
        }
#endif

        void send_file_data(std::shared_ptr<std::ifstream> file) {
            auto eof = make_upload_data(*file);
            if (eof) {
                return;
            }

            auto self = this->shared_from_this();
            async_write(multipart_str_, [this, self, file = std::move(file)](boost::system::error_code ec, std::size_t length) mutable {
                if (!ec) {
                    multipart_str_.clear();
                    send_file_data(std::move(file));
                }
                else {
                    on_chunk_(ec, "send failed");
                    close();
                }
            });
        }

        std::string multipart_file_start(std::string filename) {
            std::string multipart_start;
            multipart_start.append("--" + BOUNDARY + CRCF);
            multipart_start.append("Content-Disposition: form-data; name=\"" + std::string("test") + "\"; filename=\"" + std::move(filename) + "\"" + CRCF);
            multipart_start.append(CRCF);
            return multipart_start;
        }

        size_t total_multipart_size(size_t left_file_size, size_t multipart_start_size) {
            return left_file_size + multipart_start_size + MULTIPART_END.size();
        }

        bool make_upload_data(std::ifstream& file) {
            bool eof = file.peek() == EOF;
            if (eof) {
                file.close();
                return true;//finish all file
            }

            std::string content;
            const size_t size = 3 * 1024 * 1024;
            content.resize(size);
            file.read(&content[0], size);
            int64_t read_len = (int64_t)file.gcount();
            assert(read_len > 0);
            eof = file.peek() == EOF;

            if (read_len < size) {
                content.resize(read_len);
            }

            multipart_str_.append(content);
            if (eof) {
                multipart_str_.append(MULTIPART_END);
            }

            return false;
        }

        void copy_headers() {
            if (!copy_headers_.empty()) {
                copy_headers_.clear();
            }
            auto [headers, num_headers] = parser_.get_headers();
            for (size_t i = 0; i < num_headers; i++) {
                copy_headers_.emplace_back(std::string(headers[i].name, headers[i].name_len),
                    std::string(headers[i].value, headers[i].value_len));
            }
        }

        void reset_socket() {
            boost::system::error_code igored_ec;
            socket_ = decltype(socket_)(ios_);
            if (!socket_.is_open()) {
                socket_.open(boost::asio::ip::tcp::v4(), igored_ec);
            }
        }

        void set_error_value(const callback_t& cb, const boost::asio::error::basic_errors& ec, const std::string& error_msg) {
            if (promise_) {
                promise_->set_value({ boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404, error_msg });
            }
            if (cb) {
                cb({ boost::asio::error::make_error_code(boost::asio::error::basic_errors::invalid_argument), 404, error_msg });
            }
        }

    private:
        std::atomic_bool has_connected_ = false;
        callback_t cb_;
        std::atomic_bool in_progress_ = false;

        boost::asio::io_service& ios_;
        boost::asio::ip::tcp::resolver resolver_;
        boost::asio::ip::tcp::socket socket_;
#ifdef CINATRA_ENABLE_SSL
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> ssl_stream_;
#endif
        boost::asio::steady_timer timer_;
        std::size_t timeout_seconds_ = 60;
        boost::asio::streambuf read_buf_;

        http_parser parser_;
        std::vector<std::pair<std::string, std::string>> copy_headers_;
        std::string header_str_;
        std::vector<std::pair<std::string, std::string>> headers_;
        res_content_type res_content_type_ = res_content_type::json;

        std::string write_msg_;

        std::string chunked_result_;
        std::shared_ptr<std::ofstream> download_file_ = nullptr;
        std::function<void(boost::system::error_code, std::string_view)> on_chunk_ = nullptr;

        std::string multipart_str_;
        size_t start_;

        std::string last_uri_;
        std::promise<bool> read_finished_;
        std::future<bool> future_;

        std::shared_ptr<std::promise<callback_data>> promise_ = nullptr;
        std::weak_ptr<std::promise<callback_data>> weak_;
    };
}
