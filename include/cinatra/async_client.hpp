#pragma once
#include <deque>
#include <mutex>
#include <future>
#include "uri.hpp"
#include "use_asio.hpp"
#include "http_parser.hpp"

namespace cinatra {
    class async_client : public std::enable_shared_from_this<async_client> {
    public:
        async_client(boost::asio::io_service& ios) : ios_(ios), resolver_(ios), socket_(ios){
        }

        ~async_client() {
            close();
        }

        bool connect(std::string uri, size_t timeout_seconds) {
            auto [r, u] = get_uri(uri);
            if (!r) {
                return false;
            }

            auto promise = std::make_shared<std::promise<bool>>();
            std::weak_ptr<std::promise<bool>> weak(promise);
            host_ = u.get_host();

            async_connect(host_, u.get_port(), weak);
            
            auto future = promise->get_future();
            auto status = future.wait_for(std::chrono::seconds(timeout_seconds));
            if (status == std::future_status::timeout) {
                promise = nullptr;
                close(false);
                return false;
            }

            r = future.get();
            promise = nullptr;
            return r;
        }

        void on_data(callback_t cb) {
            cb_ = std::move(cb);
        }

        void async_request(std::string_view method, std::string_view url, const std::vector<std::pair<std::string, std::string>>& headers, 
            std::string_view body) {
            std::string write_msg = make_write_msg(method, url, headers, body);
            std::unique_lock lock(write_mtx_);
            outbox_.emplace_back(std::move(write_msg));
            if (outbox_.size() > 1) {
                return;
            }

            write();
        }

        bool has_connected() const {
            return has_connected_;
        }

    private:
        void async_connect(const std::string& host, const std::string& port, std::weak_ptr<std::promise<bool>> weak) {
            boost::asio::ip::tcp::resolver::query query(host, port);
            resolver_.async_resolve(query, [this, self = this->shared_from_this(), weak]
            (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator& it) {
                if (ec) {
                    if (auto sp = weak.lock(); sp)
                        sp->set_value(false);
                    return;
                }

                boost::asio::async_connect(socket_, it, [this, self = shared_from_this(), weak]
                (boost::system::error_code ec, const boost::asio::ip::tcp::resolver::iterator&) {
                    if (!ec) {
                        has_connected_ = true;
                        if (is_ssl()) {
                            handshake(weak);
                            return;
                        }

                        do_read();
                    }
                    else {
                        close();
                    }

                    if (auto sp = weak.lock(); sp)
                        sp->set_value(has_connected_);
                });
            });
        }

        void handshake(std::weak_ptr<std::promise<bool>> weak) {
#ifdef CINATRA_ENABLE_SSL
            auto self = this->shared_from_this();
            ssl_stream_->async_handshake(boost::asio::ssl::stream_base::client,
                [this, self, weak](const boost::system::error_code& ec) {
                if (!ec) {
                    do_read();
                }
                else {
                    close();
                }

                if (auto sp = weak.lock(); sp)
                    sp->set_value(has_connected_);
            });
#endif
        }

        void do_read() {
            async_read_until(TWO_CRCF, [this, self = shared_from_this()](auto ec, size_t size) {
                if (ec) {
                    close();
                    return;
                }

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
            });
        }

        void do_read_body(bool keep_alive, int status, size_t size_to_read) {
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive, status](auto ec, size_t size) {
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
            async_read_until(CRCF, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
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
            async_read(size_to_read, [this, self = shared_from_this(), keep_alive](auto ec, size_t size) {
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
            chunked_result_.append(chunk);
        }

        void clear_chunk_buffer() {
            if (!chunked_result_.empty()) {
                chunked_result_.clear();
            }
        }

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
            if (cb_) {
                cb_({ ec, status, result, get_resp_headers() });
            }
        }

        std::string make_write_msg(std::string_view method, std::string_view url, const std::vector<std::pair<std::string, std::string>>& headers,
            std::string_view body) {
            std::string write_msg(method);
            //can be optimized here
            write_msg.append(" ").append(url);

            write_msg.append(" HTTP/1.1\r\nHost:").
                append(host_).append("\r\n");

            bool has_connection = false;
            //add user header
            if (!headers.empty()) {
                for (auto& pair : headers) {
                    if (pair.first == "Connection") {
                        has_connection = true;
                    }
                    write_msg.append(pair.first).append(": ").append(pair.second).append("\r\n");
                }
            }

            //add content
            if (!body.empty()) {
                char buffer[20];
                auto p = i32toa_jeaiii((int)body.size(), buffer);

                write_msg.append("Content-Length: ").append(buffer, p - buffer).append("\r\n");
            }
            else {
                if (method == "POST"sv) {
                    write_msg.append("Content-Length: 0\r\n");
                }
            }

            if (!has_connection) {
                write_msg.append("Connection: keep-alive\r\n");
            }

            write_msg.append("\r\n");

            if (!body.empty()) {
                write_msg.append(body);
            }

            return write_msg;
        }

        void write() {
            auto& msg = outbox_[0];
            async_write(msg, [this, self = shared_from_this()](const boost::system::error_code& ec, const size_t length) {
                if (ec) {
                    //print(ec);
                    close();
                    return;
                }

                std::unique_lock lock(write_mtx_);
                if (outbox_.empty()) {
                    return;
                }

                outbox_.pop_front();

                if (!outbox_.empty()) {
                    // more messages to send
                    write();
                }
            });
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
            //timer_.cancel(ec);
            socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
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

            return { true, u };
        }

        std::pair<phr_header*, size_t> get_resp_headers() {
            if (!copy_headers_.empty())
                parser_.set_headers(copy_headers_);

            return parser_.get_headers();
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
    private:
        boost::asio::io_service& ios_;
        boost::asio::ip::tcp::resolver resolver_;
        boost::asio::ip::tcp::socket socket_;
        boost::asio::streambuf read_buf_;
        std::deque<std::string> outbox_;
        std::mutex write_mtx_;
        std::atomic_bool has_connected_ = false;
#ifdef CINATRA_ENABLE_SSL
        std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> ssl_stream_;
#endif
        callback_t cb_;
        std::string host_;

        std::string chunked_result_;

        http_parser parser_;
        std::vector<std::pair<std::string, std::string>> copy_headers_;
    };
}