#ifndef CLIENT_HTTP_HPP
#define CLIENT_HTTP_HPP

#include <limits>
#include <mutex>
#include <random>
#include <unordered_set>
#include <vector>
#include <sstream>
#include <string_view>
#include <string.h>
#include <map>
#include <vector>
#include "utils.hpp"
#include "multipart_form.hpp"
#ifdef ASIO_STANDALONE
#include <asio.hpp>
#include <asio/steady_timer.hpp>
#ifdef CINATRA_ENABLE_CLIENT_SSL
#include <asio/ssl.hpp>
#endif
namespace cinatra {
  using error_code = std::error_code;
  using errc = std::errc;
  using system_error = std::system_error;
  namespace make_error_code = std;
} // namespace cinatra
#else
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#ifdef CINATRA_ENABLE_CLIENT_SSL
#include <boost/asio/ssl.hpp>
#endif
namespace cinatra {
    namespace asio = boost::asio;
    using error_code = boost::system::error_code;
    namespace errc = boost::system::errc;
    using system_error = boost::system::system_error;
    namespace make_error_code = boost::system::errc;
} // namespace cinatra
#endif

#ifdef __SSE2__
#include <emmintrin.h>
namespace cinatra {
    inline void spin_loop_pause() noexcept { _mm_pause(); }
} // namespace cinatra
// TODO: need verification that the following checks are correct:
#elif defined(_MSC_VER) && _MSC_VER >= 1800 && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
namespace cinatra {
  inline void spin_loop_pause() noexcept { _mm_pause(); }
} // namespace cinatra
#else
namespace cinatra {
  inline void spin_loop_pause() noexcept {}
} // namespace cinatra
#endif


inline bool case_insensitive_equal(const std::string &str1, const std::string &str2) noexcept {
    return str1.size() == str2.size() &&
           std::equal(str1.begin(), str1.end(), str2.begin(), [](char a, char b) {
               return tolower(a) == tolower(b);
           });
}


class CaseInsensitiveEqual {
public:
    bool operator()(const std::string &str1, const std::string &str2) const noexcept {
        return case_insensitive_equal(str1, str2);
    }
};
// Based on https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x/2595226#2595226
class CaseInsensitiveHash {
public:
    std::size_t operator()(const std::string &str) const noexcept {
        std::size_t h = 0;
        std::hash<int> hash;
        for(auto c : str)
            h ^= hash(tolower(c)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
using CaseInsensitiveMultimap = std::unordered_multimap<std::string, std::string, CaseInsensitiveHash, CaseInsensitiveEqual>;

class ScopeRunner {
    /// Scope count that is set to -1 if scopes are to be canceled
    std::atomic<long> count;

public:
    class SharedLock {
        friend class ScopeRunner;
        std::atomic<long> &count;
        SharedLock(std::atomic<long> &count) noexcept : count(count) {}
        SharedLock &operator=(const SharedLock &) = delete;
        SharedLock(const SharedLock &) = delete;

    public:
        ~SharedLock() noexcept {
            count.fetch_sub(1);
        }
    };

    ScopeRunner() noexcept : count(0) {}

    /// Returns nullptr if scope should be exited, or a shared lock otherwise
    std::unique_ptr<SharedLock> continue_lock() noexcept {
        long expected = count;
        while(expected >= 0 && !count.compare_exchange_weak(expected, expected + 1))
            cinatra::spin_loop_pause();

        if(expected < 0)
            return nullptr;
        else
            return std::unique_ptr<SharedLock>(new SharedLock(count));
    }

    /// Blocks until all shared locks are released, then prevents future shared locks
    void stop() noexcept {
        long expected = 0;
        while(!count.compare_exchange_weak(expected, -1)) {
            if(expected < 0)
                return;
            expected = 0;
            cinatra::spin_loop_pause();
        }
    }
};
class Percent {
public:
    /// Returns percent-encoded string
    static std::string encode(const std::string &value) noexcept {
        static auto hex_chars = "0123456789ABCDEF";

        std::string result;
        result.reserve(value.size()); // Minimum size of result

        for(auto &chr : value) {
            if(!((chr >= '0' && chr <= '9') || (chr >= 'A' && chr <= 'Z') || (chr >= 'a' && chr <= 'z') || chr == '-' || chr == '.' || chr == '_' || chr == '~'))
                result += std::string("%") + hex_chars[static_cast<unsigned char>(chr) >> 4] + hex_chars[static_cast<unsigned char>(chr) & 15];
            else
                result += chr;
        }

        return result;
    }

    /// Returns percent-decoded string
    static std::string decode(const std::string &value) noexcept {
        std::string result;
        result.reserve(value.size() / 3 + (value.size() % 3)); // Minimum size of result

        for(std::size_t i = 0; i < value.size(); ++i) {
            auto &chr = value[i];
            if(chr == '%' && i + 2 < value.size()) {
                auto hex = value.substr(i + 1, 2);
                auto decoded_chr = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
                result += decoded_chr;
                i += 2;
            }
            else if(chr == '+')
                result += ' ';
            else
                result += chr;
        }

        return result;
    }
};

class HttpHeader {
public:
    /// Parse header fields
    static CaseInsensitiveMultimap parse(std::istream &stream) noexcept {
        CaseInsensitiveMultimap result;
        std::string line;
        getline(stream, line);
        std::size_t param_end;
        while((param_end = line.find(':')) != std::string::npos) {
            std::size_t value_start = param_end + 1;
            while(value_start + 1 < line.size() && line[value_start] == ' ')
                ++value_start;
            if(value_start < line.size())
                result.emplace(line.substr(0, param_end), line.substr(value_start, line.size() - value_start - 1));

            getline(stream, line);
        }
        return result;
    }

    class FieldValue {
    public:
        class SemicolonSeparatedAttributes {
        public:
            /// Parse Set-Cookie or Content-Disposition header field value. Attribute values are percent-decoded.
            static CaseInsensitiveMultimap parse(const std::string &str) {
                CaseInsensitiveMultimap result;

                std::size_t name_start_pos = std::string::npos;
                std::size_t name_end_pos = std::string::npos;
                std::size_t value_start_pos = std::string::npos;
                for(std::size_t c = 0; c < str.size(); ++c) {
                    if(name_start_pos == std::string::npos) {
                        if(str[c] != ' ' && str[c] != ';')
                            name_start_pos = c;
                    }
                    else {
                        if(name_end_pos == std::string::npos) {
                            if(str[c] == ';') {
                                result.emplace(str.substr(name_start_pos, c - name_start_pos), std::string());
                                name_start_pos = std::string::npos;
                            }
                            else if(str[c] == '=')
                                name_end_pos = c;
                        }
                        else {
                            if(value_start_pos == std::string::npos) {
                                if(str[c] == '"' && c + 1 < str.size())
                                    value_start_pos = c + 1;
                                else
                                    value_start_pos = c;
                            }
                            else if(str[c] == '"' || str[c] == ';') {
                                result.emplace(str.substr(name_start_pos, name_end_pos - name_start_pos), Percent::decode(str.substr(value_start_pos, c - value_start_pos)));
                                name_start_pos = std::string::npos;
                                name_end_pos = std::string::npos;
                                value_start_pos = std::string::npos;
                            }
                        }
                    }
                }
                if(name_start_pos != std::string::npos) {
                    if(name_end_pos == std::string::npos)
                        result.emplace(str.substr(name_start_pos), std::string());
                    else if(value_start_pos != std::string::npos) {
                        if(str.back() == '"')
                            result.emplace(str.substr(name_start_pos, name_end_pos - name_start_pos), Percent::decode(str.substr(value_start_pos, str.size() - 1)));
                        else
                            result.emplace(str.substr(name_start_pos, name_end_pos - name_start_pos), Percent::decode(str.substr(value_start_pos)));
                    }
                }

                return result;
            }
        };
    };
};


class ResponseMessage {
public:
    /// Parse status line and header fields
    static bool parse(std::istream &stream, std::string &version, std::string &status_code, CaseInsensitiveMultimap &header) noexcept {
        header.clear();
        std::string line;
        getline(stream, line);
        std::size_t version_end = line.find(' ');
        if(version_end != std::string::npos) {
            if(5 < line.size())
                version = line.substr(5, version_end - 5);
            else
                return false;
            if((version_end + 1) < line.size())
                status_code = line.substr(version_end + 1, line.size() - (version_end + 1) - 1);
            else
                return false;

            header = HttpHeader::parse(stream);
        }
        else
            return false;
        return true;
    }
};

namespace cinatra {
    template <class socket_type>
    class http_client_;

    template <class socket_type>
    class ClientBase {
    public:
        using error_code = cinatra::error_code;
        class Content : public std::istream {
            friend class ClientBase<socket_type>;

        public:
            std::size_t size() noexcept {
                return streambuf.size();
            }
            /// Convenience function to return std::string. The stream buffer is consumed.
            std::string string() noexcept {
                try {
                    std::string str;
                    auto size = streambuf.size();
                    str.resize(size);
                    read(&str[0], static_cast<std::streamsize>(size));
                    return str;
                }
                catch(...) {
                    return std::string();
                }
            }

        private:
            asio::streambuf &streambuf;
            Content(asio::streambuf &streambuf) noexcept : std::istream(&streambuf), streambuf(streambuf) {}
        };

        class client_response {
            friend class ClientBase<socket_type>;
            friend class http_client_<socket_type>;

            asio::streambuf streambuf;

            client_response(std::size_t max_response_streambuf_size) noexcept : streambuf(max_response_streambuf_size), content(streambuf) {}
        public:
            client_response& operator=(const client_response& res)
            {
                http_version = res.http_version;
                status_code = res.status_code;
                header = res.header;
                save_content = res.content.string();
                std::ostream tmp(&streambuf);
                tmp<< save_content;
                return *this;
            }
            client_response(const client_response& res):http_version(res.http_version),status_code(res.status_code),header(res.header),streambuf(res.streambuf.size()),content(streambuf)
            {
                save_content = res.content.string();
                std::ostream tmp(&streambuf);
                tmp<< save_content;
            }

            client_response& vir_copy(){
                save_content = content.string();
                std::ostream tmp(&streambuf);
                tmp<< save_content;
                return *this;
            }

        private:
            std::string http_version, status_code;

            mutable  Content content;

            std::string save_content;

            CaseInsensitiveMultimap header;
        public:
            std::string get_status_code() const
            {
                return status_code;
            }
            std::string get_http_version() const
            {
                return http_version;
            }
            std::string get_content() const
            {
                return save_content;
            }
            const std::vector<std::pair<std::string,std::string>> get_header(const std::string& name) const
            {
                std::vector<std::pair<std::string,std::string>> find_header;
                auto number = header.count(name);
                auto iter = header.find(name);
                while(number>0){
                    find_header.push_back(std::make_pair(iter->first,iter->second));
                    iter++;
                    number--;
                }
                return find_header;
            }

        };

        class Config {
            friend class ClientBase<socket_type>;

        private:
            Config() noexcept {}

        public:
            /// Set timeout on requests in seconds. Default value: 0 (no timeout).
            long timeout = 0;
            /// Set connect timeout in seconds. Default value: 0 (Config::timeout is then used instead).
            long timeout_connect = 0;
            /// Maximum size of response stream buffer. Defaults to architecture maximum.
            /// Reaching this limit will result in a message_size error code.
            std::size_t max_response_streambuf_size = std::numeric_limits<std::size_t>::max();
            /// Set proxy server (server:port)
            std::string proxy_server;
        };

    protected:
        class Connection : public std::enable_shared_from_this<Connection> {
        public:
            template <typename... Args>
            Connection(std::shared_ptr<ScopeRunner> handler_runner, long timeout, Args &&... args) noexcept
                    : handler_runner(std::move(handler_runner)), timeout(timeout), socket(new socket_type(std::forward<Args>(args)...)) {}

            std::shared_ptr<ScopeRunner> handler_runner;
            long timeout;

            std::unique_ptr<socket_type> socket; // Socket must be unique_ptr since asio::ssl::stream<asio::ip::tcp::socket> is not movable
            bool in_use = false;
            bool attempt_reconnect = true;

            std::unique_ptr<asio::steady_timer> timer;

            void set_timeout(long seconds = 0) noexcept {
                if(seconds == 0)
                    seconds = timeout;
                if(seconds == 0) {
                    timer = nullptr;
                    return;
                }
                timer = std::unique_ptr<asio::steady_timer>(new asio::steady_timer(socket->get_io_service()));
                timer->expires_from_now(std::chrono::seconds(seconds));
                auto self = this->shared_from_this();
                timer->async_wait([self](const error_code &ec) {
                    if(!ec) {
                        error_code ec;
                        self->socket->lowest_layer().cancel(ec);
                    }
                });
            }

            void cancel_timeout() noexcept {
                if(timer) {
                    error_code ec;
                    timer->cancel(ec);
                }
            }
        };

        class Session {
        public:
            Session(std::size_t max_response_streambuf_size, std::shared_ptr<Connection> connection, std::unique_ptr<asio::streambuf> request_streambuf) noexcept
                    : connection(std::move(connection)), request_streambuf(std::move(request_streambuf)), response(new client_response(max_response_streambuf_size)) {}

            std::shared_ptr<Connection> connection;
            std::unique_ptr<asio::streambuf> request_streambuf;
            std::shared_ptr<client_response> response;
            std::function<void(const std::shared_ptr<Connection> &, const error_code &)> callback;
        };

    public:
        /// Set before calling request
        Config config;

        /// If you have your own asio::io_service, store its pointer here before calling request().
        /// When using asynchronous requests, running the io_service is up to the programmer.
        std::shared_ptr<asio::io_service> io_service;

        /// Convenience function to perform synchronous request. The io_service is run within this function.
        /// If reusing the io_service for other tasks, use the asynchronous request functions instead.
        /// Do not use concurrently with the asynchronous request functions.
        void add_header(const std::string& name,const std::string& value){
            request_header_.insert(std::make_pair(name,value));
        }

        template<http_method Method>
        client_response request(const std::string &path,const multipart_form& multi_form) {
            request_header_.insert(std::make_pair("Content-Type",multi_form.content_type()));
            auto body = multi_form.to_body();
            return request<Method>(path,std::string_view(body.data(),body.size()));
        }

        template<http_method Method>
        client_response request(const std::string &path = std::string("/"),
                                std::string_view content = "") {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            client_response response(config.max_response_streambuf_size);
            error_code ec;
            request(method, path, content, [&response, &ec](const client_response& response_, const error_code &ec_) {
                response = response_;
                ec = ec_;
            });

            {
                std::unique_lock<std::mutex> lock(concurrent_synchronous_requests_mutex);
                ++concurrent_synchronous_requests;
            }
            io_service->run();
            {
                std::unique_lock<std::mutex> lock(concurrent_synchronous_requests_mutex);
                --concurrent_synchronous_requests;
                if(!concurrent_synchronous_requests)
                    io_service->reset();
            }

            if(ec)
                throw system_error(ec);

            return response;
        }

        void run()
        {
            io_service->run();
        }

        /// Convenience function to perform synchronous request. The io_service is run within this function.
        /// If reusing the io_service for other tasks, use the asynchronous request functions instead.
        /// Do not use concurrently with the asynchronous request functions.

        template<http_method Method>
        client_response request(const std::string &path, std::istream &content) {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            client_response response;
            error_code ec;
            request(method, path, content, [&response, &ec](const client_response& response_, const error_code &ec_) {
                response = response_;
                ec = ec_;
            });

            {
                std::unique_lock<std::mutex> lock(concurrent_synchronous_requests_mutex);
                ++concurrent_synchronous_requests;
            }
            io_service->run();
            {
                std::unique_lock<std::mutex> lock(concurrent_synchronous_requests_mutex);
                --concurrent_synchronous_requests;
                if(!concurrent_synchronous_requests)
                    io_service->reset();
            }

            if(ec)
                throw system_error(ec);

            return response;
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        /// Do not use concurrently with the synchronous request functions.

        template<http_method Method>
        void request(const std::string &path,const multipart_form& multi_form,std::function<void(const client_response&, const error_code &)> &&request_callback_) {
            request_header_.insert(std::make_pair("Content-Type",multi_form.content_type()));
            request<Method>(path,multi_form.to_body(),std::move(request_callback_));
        }

        void request(const std::string &method, const std::string &path, std::string_view content,std::function<void(const client_response&, const error_code &)> &&request_callback_) {
            auto session = std::make_shared<Session>(config.max_response_streambuf_size, get_connection(), create_request_header(method, path));
            auto response = session->response;
            auto request_callback = std::make_shared<std::function<void(const client_response&, const error_code &)>>(std::move(request_callback_));
            session->callback = [this, response, request_callback](const std::shared_ptr<Connection> &connection, const error_code &ec) {
                {
                    std::unique_lock<std::mutex> lock(this->connections_mutex);
                    connection->in_use = false;

                    // Remove unused connections, but keep one open for HTTP persistent connection:
                    std::size_t unused_connections = 0;
                    for(auto it = this->connections.begin(); it != this->connections.end();) {
                        if(ec && connection == *it)
                            it = this->connections.erase(it);
                        else if((*it)->in_use)
                            ++it;
                        else {
                            ++unused_connections;
                            if(unused_connections > 1)
                                it = this->connections.erase(it);
                            else
                                ++it;
                        }
                    }
                }

                if(*request_callback)
                {
                    (*request_callback)(response->vir_copy(), ec);
                }
            };

            std::ostream write_stream(session->request_streambuf.get());
            if(content.size() > 0) {
                auto header_it = request_header_.find("Content-Length");
                if(header_it == request_header_.end()) {
                    header_it = request_header_.find("Transfer-Encoding");
                    if(header_it == request_header_.end() || header_it->second != "chunked")
                        write_stream << "Content-Length: " << content.size() << "\r\n";
                }
            }
            write_stream << "\r\n"
                         << content;

            connect(session);
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        /// Do not use concurrently with the synchronous request functions.
        template<http_method Method>
        void request(const std::string &path, std::string_view content,std::function<void(const client_response&, const error_code &)> &&request_callback) {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            request(method, path, content, std::move(request_callback));
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        template<http_method Method>
        void request(const std::string &path,std::function<void(const client_response&, const error_code &)> &&request_callback) {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            request(method, path, std::string(), std::move(request_callback));
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        template<http_method Method>
        void request(std::function<void(const client_response&, const error_code &)> &&request_callback) {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            request(method, std::string("/"), std::string(), std::move(request_callback));
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        void request(const std::string &method, const std::string &path, std::istream &content,std::function<void(const client_response&, const error_code &)> &&request_callback_) {
            auto session = std::make_shared<Session>(config.max_response_streambuf_size, get_connection(), create_request_header(method, path));
            auto response = session->response;
            auto request_callback = std::make_shared<std::function<void(const client_response&, const error_code &)>>(std::move(request_callback_));
            session->callback = [this, response, request_callback](const std::shared_ptr<Connection> &connection, const error_code &ec) {
                {
                    std::unique_lock<std::mutex> lock(this->connections_mutex);
                    connection->in_use = false;

                    // Remove unused connections, but keep one open for HTTP persistent connection:
                    std::size_t unused_connections = 0;
                    for(auto it = this->connections.begin(); it != this->connections.end();) {
                        if(ec && connection == *it)
                            it = this->connections.erase(it);
                        else if((*it)->in_use)
                            ++it;
                        else {
                            ++unused_connections;
                            if(unused_connections > 1)
                                it = this->connections.erase(it);
                            else
                                ++it;
                        }
                    }
                }

                if(*request_callback)
                {
                    (*request_callback)(response->vir_copy(), ec);
                }
            };

            content.seekg(0, std::ios::end);
            auto content_length = content.tellg();
            content.seekg(0, std::ios::beg);
            std::ostream write_stream(session->request_streambuf.get());
            if(content_length > 0) {
                auto header_it = request_header_.find("Content-Length");
                if(header_it == request_header_.end()) {
                    header_it = request_header_.find("Transfer-Encoding");
                    if(header_it == request_header_.end() || header_it->second != "chunked")
                        write_stream << "Content-Length: " << content_length << "\r\n";
                }
            }
            write_stream << "\r\n";
            if(content_length > 0)
                write_stream << content.rdbuf();

            connect(session);
        }

        /// Asynchronous request where setting and/or running Client's io_service is required.
        template<http_method Method>
        void request(const std::string &path, std::istream &content,
                     std::function<void(const client_response&, const error_code &)> &&request_callback) {
            auto type_name = method_name(Method);
            std::string method(type_name.data(),type_name.size());
            request(method, path, content, std::move(request_callback));
        }

        /// Close connections
        void stop() noexcept {
            std::unique_lock<std::mutex> lock(connections_mutex);
            for(auto it = connections.begin(); it != connections.end();) {
                error_code ec;
                (*it)->socket->lowest_layer().cancel(ec);
                it = connections.erase(it);
            }
        }

        virtual ~ClientBase() noexcept {
            handler_runner->stop();
            stop();
        }

    protected:
        bool internal_io_service = false;

        std::string host;
        unsigned short port;
        unsigned short default_port;

        std::unique_ptr<asio::ip::tcp::resolver::query> query;

        std::unordered_set<std::shared_ptr<Connection>> connections;
        std::mutex connections_mutex;

        std::shared_ptr<ScopeRunner> handler_runner;

        std::size_t concurrent_synchronous_requests = 0;
        std::mutex concurrent_synchronous_requests_mutex;

        CaseInsensitiveMultimap request_header_;

        ClientBase(const std::string &host_port, unsigned short default_port) noexcept : default_port(default_port), handler_runner(new ScopeRunner()) {
            auto parsed_host_port = parse_host_port(host_port, default_port);
            host = parsed_host_port.first;
            port = parsed_host_port.second;
        }

        std::shared_ptr<Connection> get_connection() noexcept {
            std::shared_ptr<Connection> connection;
            std::unique_lock<std::mutex> lock(connections_mutex);

            if(!io_service) {
                io_service = std::make_shared<asio::io_service>();
                internal_io_service = true;
            }

            for(auto it = connections.begin(); it != connections.end(); ++it) {
                if(!(*it)->in_use && !connection) {
                    connection = *it;
                    break;
                }
            }
            if(!connection) {
                connection = create_connection();
                connections.emplace(connection);
            }
            connection->attempt_reconnect = true;
            connection->in_use = true;

            if(!query) {
                if(config.proxy_server.empty())
                    query = std::unique_ptr<asio::ip::tcp::resolver::query>(new asio::ip::tcp::resolver::query(host, std::to_string(port)));
                else {
                    auto proxy_host_port = parse_host_port(config.proxy_server, 8080);
                    query = std::unique_ptr<asio::ip::tcp::resolver::query>(new asio::ip::tcp::resolver::query(proxy_host_port.first, std::to_string(proxy_host_port.second)));
                }
            }

            return connection;
        }

        virtual std::shared_ptr<Connection> create_connection() noexcept = 0;
        virtual void connect(const std::shared_ptr<Session> &) = 0;

        std::unique_ptr<asio::streambuf> create_request_header(const std::string &method, const std::string &path) const {
            auto corrected_path = path;
            if(corrected_path == "")
                corrected_path = "/";
            if(!config.proxy_server.empty() && std::is_same<socket_type, asio::ip::tcp::socket>::value)
                corrected_path = "http://" + host + ':' + std::to_string(port) + corrected_path;

            std::unique_ptr<asio::streambuf> streambuf(new asio::streambuf());
            std::ostream write_stream(streambuf.get());
            write_stream << method << " " << corrected_path << " HTTP/1.1\r\n";
            write_stream << "Host: " << host;
            if(port != default_port)
                write_stream << ':' << std::to_string(port);
            write_stream << "\r\n";
            for(auto &h : request_header_)
                write_stream << h.first << ": " << h.second << "\r\n";
            return streambuf;
        }

        std::pair<std::string, unsigned short> parse_host_port(const std::string &host_port, unsigned short default_port) const noexcept {
            std::pair<std::string, unsigned short> parsed_host_port;
            std::size_t host_end = host_port.find(':');
            if(host_end == std::string::npos) {
                parsed_host_port.first = host_port;
                parsed_host_port.second = default_port;
            }
            else {
                parsed_host_port.first = host_port.substr(0, host_end);
                parsed_host_port.second = static_cast<unsigned short>(stoul(host_port.substr(host_end + 1)));
            }
            return parsed_host_port;
        }

        void write(const std::shared_ptr<Session> &session) {
            session->connection->set_timeout();
            asio::async_write(*session->connection->socket, session->request_streambuf->data(), [this, session](const error_code &ec, std::size_t /*bytes_transferred*/) {
                session->connection->cancel_timeout();
                auto lock = session->connection->handler_runner->continue_lock();
                if(!lock)
                    return;
                if(!ec)
                    this->read(session);
                else
                    session->callback(session->connection, ec);
            });
        }

        void read(const std::shared_ptr<Session> &session) {
            session->connection->set_timeout();
            asio::async_read_until(*session->connection->socket, session->response->streambuf, "\r\n\r\n", [this, session](const error_code &ec, std::size_t bytes_transferred) {
                session->connection->cancel_timeout();
                auto lock = session->connection->handler_runner->continue_lock();
                if(!lock)
                    return;
                if((!ec || ec == asio::error::not_found) && session->response->streambuf.size() == session->response->streambuf.max_size()) {
                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                    return;
                }
                if(!ec) {
                    session->connection->attempt_reconnect = true;
                    std::size_t num_additional_bytes = session->response->streambuf.size() - bytes_transferred;

                    if(!ResponseMessage::parse(session->response->content, session->response->http_version, session->response->status_code, session->response->header)) {
                        session->callback(session->connection, make_error_code::make_error_code(errc::protocol_error));
                        return;
                    }

                    auto header_it = session->response->header.find("Content-Length");
                    if(header_it != session->response->header.end()) {
                        auto content_length = stoull(header_it->second);
                        if(content_length > num_additional_bytes) {
                            session->connection->set_timeout();
                            asio::async_read(*session->connection->socket, session->response->streambuf, asio::transfer_exactly(content_length - num_additional_bytes), [session](const error_code &ec, std::size_t /*bytes_transferred*/) {
                                session->connection->cancel_timeout();
                                auto lock = session->connection->handler_runner->continue_lock();
                                if(!lock)
                                    return;
                                if(!ec) {
                                    if(session->response->streambuf.size() == session->response->streambuf.max_size()) {
                                        session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                                        return;
                                    }
                                    session->callback(session->connection, ec);
                                }
                                else
                                    session->callback(session->connection, ec);
                            });
                        }
                        else
                            session->callback(session->connection, ec);
                    }
                    else if((header_it = session->response->header.find("Transfer-Encoding")) != session->response->header.end() && header_it->second == "chunked") {
                        auto chunks_streambuf = std::make_shared<asio::streambuf>(this->config.max_response_streambuf_size);
                        this->read_chunked_transfer_encoded(session, chunks_streambuf);
                    }
                    else if(session->response->http_version < "1.1" || ((header_it = session->response->header.find("Session")) != session->response->header.end() && header_it->second == "close")) {
                        session->connection->set_timeout();
                        asio::async_read(*session->connection->socket, session->response->streambuf, [session](const error_code &ec, std::size_t /*bytes_transferred*/) {
                            session->connection->cancel_timeout();
                            auto lock = session->connection->handler_runner->continue_lock();
                            if(!lock)
                                return;
                            if(!ec) {
                                if(session->response->streambuf.size() == session->response->streambuf.max_size()) {
                                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                                    return;
                                }
                                session->callback(session->connection, ec);
                            }
                            else
                                session->callback(session->connection, ec == asio::error::eof ? error_code() : ec);
                        });
                    }
                    else
                        session->callback(session->connection, ec);
                }
                else {
                    if(session->connection->attempt_reconnect && ec != asio::error::operation_aborted) {
                        std::unique_lock<std::mutex> lock(connections_mutex);
                        auto it = connections.find(session->connection);
                        if(it != connections.end()) {
                            connections.erase(it);
                            session->connection = create_connection();
                            session->connection->attempt_reconnect = false;
                            session->connection->in_use = true;
                            connections.emplace(session->connection);
                            lock.unlock();
                            this->connect(session);
                        }
                        else {
                            lock.unlock();
                            session->callback(session->connection, ec);
                        }
                    }
                    else
                        session->callback(session->connection, ec);
                }
            });
        }

        void read_chunked_transfer_encoded(const std::shared_ptr<Session> &session, const std::shared_ptr<asio::streambuf> &chunks_streambuf) {
            session->connection->set_timeout();
            asio::async_read_until(*session->connection->socket, session->response->streambuf, "\r\n", [this, session, chunks_streambuf](const error_code &ec, size_t bytes_transferred) {
                session->connection->cancel_timeout();
                auto lock = session->connection->handler_runner->continue_lock();
                if(!lock)
                    return;
                if((!ec || ec == asio::error::not_found) && session->response->streambuf.size() == session->response->streambuf.max_size()) {
                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                    return;
                }
                if(!ec) {
                    std::string line;
                    getline(session->response->content, line);
                    bytes_transferred -= line.size() + 1;
                    line.pop_back();
                    unsigned long length = 0;
                    try {
                        length = stoul(line, 0, 16);
                    }
                    catch(...) {
                        session->callback(session->connection, make_error_code::make_error_code(errc::protocol_error));
                        return;
                    }

                    auto num_additional_bytes = session->response->streambuf.size() - bytes_transferred;

                    if((2 + length) > num_additional_bytes) {
                        session->connection->set_timeout();
                        asio::async_read(*session->connection->socket, session->response->streambuf, asio::transfer_exactly(2 + length - num_additional_bytes), [this, session, chunks_streambuf, length](const error_code &ec, size_t /*bytes_transferred*/) {
                            session->connection->cancel_timeout();
                            auto lock = session->connection->handler_runner->continue_lock();
                            if(!lock)
                                return;
                            if(!ec) {
                                if(session->response->streambuf.size() == session->response->streambuf.max_size()) {
                                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                                    return;
                                }
                                this->read_chunked_transfer_encoded_chunk(session, chunks_streambuf, length);
                            }
                            else
                                session->callback(session->connection, ec);
                        });
                    }
                    else
                        this->read_chunked_transfer_encoded_chunk(session, chunks_streambuf, length);
                }
                else
                    session->callback(session->connection, ec);
            });
        }

        void read_chunked_transfer_encoded_chunk(const std::shared_ptr<Session> &session, const std::shared_ptr<asio::streambuf> &chunks_streambuf, unsigned long length) {
            std::ostream tmp_stream(chunks_streambuf.get());
            if(length > 0) {
                std::unique_ptr<char[]> buffer(new char[length]);
                session->response->content.read(buffer.get(), static_cast<std::streamsize>(length));
                tmp_stream.write(buffer.get(), static_cast<std::streamsize>(length));
                if(chunks_streambuf->size() == chunks_streambuf->max_size()) {
                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                    return;
                }
            }

            // Remove "\r\n"
            session->response->content.get();
            session->response->content.get();

            if(length > 0)
                read_chunked_transfer_encoded(session, chunks_streambuf);
            else {
                if(chunks_streambuf->size() > 0) {
                    std::ostream ostream(&session->response->streambuf);
                    ostream << chunks_streambuf.get();
                }
                error_code ec;
                session->callback(session->connection, ec);
            }
        }
    };

    template <class socket_type>
    class http_client_ : public ClientBase<socket_type> {};

    using HTTP = asio::ip::tcp::socket;

    template <>
    class http_client_<HTTP> : public ClientBase<HTTP> {
    public:
        http_client_(const std::string &server_port_path) noexcept : ClientBase<HTTP>::ClientBase(server_port_path, 80) {}

    protected:
        std::shared_ptr<Connection> create_connection() noexcept override {
            return std::make_shared<Connection>(handler_runner, config.timeout, *io_service);
        }

        void connect(const std::shared_ptr<Session> &session) override {
            if(!session->connection->socket->lowest_layer().is_open()) {
                auto resolver = std::make_shared<asio::ip::tcp::resolver>(*io_service);
                session->connection->set_timeout(config.timeout_connect);
                resolver->async_resolve(*query, [this, session, resolver](const error_code &ec, asio::ip::tcp::resolver::iterator it) {
                    session->connection->cancel_timeout();
                    auto lock = session->connection->handler_runner->continue_lock();
                    if(!lock)
                        return;
                    if(!ec) {
                        session->connection->set_timeout(config.timeout_connect);
                        asio::async_connect(*session->connection->socket, it, [this, session, resolver](const error_code &ec, asio::ip::tcp::resolver::iterator /*it*/) {
                            session->connection->cancel_timeout();
                            auto lock = session->connection->handler_runner->continue_lock();
                            if(!lock)
                                return;
                            if(!ec) {
                                asio::ip::tcp::no_delay option(true);
                                error_code ec;
                                session->connection->socket->set_option(option, ec);
                                this->write(session);
                            }
                            else
                                session->callback(session->connection, ec);
                        });
                    }
                    else
                        session->callback(session->connection, ec);
                });
            }
            else
                write(session);
        }
    };
    using http_client = http_client_<HTTP>;

    //SSL
#ifdef CINATRA_ENABLE_CLIENT_SSL
    using HTTPS = asio::ssl::stream<asio::ip::tcp::socket>;
    template <>
    class http_client_<HTTPS> : public ClientBase<HTTPS> {
    public:
        http_client_(const std::string &server_port_path, bool verify_certificate = true, const std::string &cert_file = std::string(),
               const std::string &private_key_file = std::string(), const std::string &verify_file = std::string())
                : ClientBase<HTTPS>::ClientBase(server_port_path, 443), context(asio::ssl::context::tlsv12) {
            if(cert_file.size() > 0 && private_key_file.size() > 0) {
                context.use_certificate_chain_file(cert_file);
                context.use_private_key_file(private_key_file, asio::ssl::context::pem);
            }

            if(verify_certificate)
                context.set_verify_callback(asio::ssl::rfc2818_verification(host));

            if(verify_file.size() > 0)
                context.load_verify_file(verify_file);
            else
                context.set_default_verify_paths();

            if(verify_file.size() > 0 || verify_certificate)
                context.set_verify_mode(asio::ssl::verify_peer);
            else
                context.set_verify_mode(asio::ssl::verify_none);
        }

    protected:
        asio::ssl::context context;

        std::shared_ptr<Connection> create_connection() noexcept override {
            return std::make_shared<Connection>(handler_runner, config.timeout, *io_service, context);
        }

        void connect(const std::shared_ptr<Session> &session) override {
            if(!session->connection->socket->lowest_layer().is_open()) {
                auto resolver = std::make_shared<asio::ip::tcp::resolver>(*io_service);
                resolver->async_resolve(*query, [this, session, resolver](const error_code &ec, asio::ip::tcp::resolver::iterator it) {
                    auto lock = session->connection->handler_runner->continue_lock();
                    if(!lock)
                        return;
                    if(!ec) {
                        session->connection->set_timeout(this->config.timeout_connect);
                        asio::async_connect(session->connection->socket->lowest_layer(), it, [this, session, resolver](const error_code &ec, asio::ip::tcp::resolver::iterator /*it*/) {
                            session->connection->cancel_timeout();
                            auto lock = session->connection->handler_runner->continue_lock();
                            if(!lock)
                                return;
                            if(!ec) {
                                asio::ip::tcp::no_delay option(true);
                                error_code ec;
                                session->connection->socket->lowest_layer().set_option(option, ec);

                                if(!this->config.proxy_server.empty()) {
                                    auto write_buffer = std::make_shared<asio::streambuf>();
                                    std::ostream write_stream(write_buffer.get());
                                    auto host_port = this->host + ':' + std::to_string(this->port);
                                    write_stream << "CONNECT " + host_port + " HTTP/1.1\r\n"
                                                 << "Host: " << host_port << "\r\n\r\n";
                                    session->connection->set_timeout(this->config.timeout_connect);
                                    asio::async_write(session->connection->socket->next_layer(), *write_buffer, [this, session, write_buffer](const error_code &ec, std::size_t /*bytes_transferred*/) {
                                        session->connection->cancel_timeout();
                                        auto lock = session->connection->handler_runner->continue_lock();
                                        if(!lock)
                                            return;
                                        if(!ec) {
                                            std::shared_ptr<client_response> response(new client_response(this->config.max_response_streambuf_size));
                                            session->connection->set_timeout(this->config.timeout_connect);
                                            asio::async_read_until(session->connection->socket->next_layer(), response->streambuf, "\r\n\r\n", [this, session, response](const error_code &ec, std::size_t /*bytes_transferred*/) {
                                                session->connection->cancel_timeout();
                                                auto lock = session->connection->handler_runner->continue_lock();
                                                if(!lock)
                                                    return;
                                                if((!ec || ec == asio::error::not_found) && response->streambuf.size() == response->streambuf.max_size()) {
                                                    session->callback(session->connection, make_error_code::make_error_code(errc::message_size));
                                                    return;
                                                }
                                                if(!ec) {
                                                    if(!ResponseMessage::parse(response->content, response->http_version, response->status_code, response->header))
                                                        session->callback(session->connection, make_error_code::make_error_code(errc::protocol_error));
                                                    else {
                                                        if(response->status_code.empty() || response->status_code.compare(0, 3, "200") != 0)
                                                            session->callback(session->connection, make_error_code::make_error_code(errc::permission_denied));
                                                        else
                                                            this->handshake(session);
                                                    }
                                                }
                                                else
                                                    session->callback(session->connection, ec);
                                            });
                                        }
                                        else
                                            session->callback(session->connection, ec);
                                    });
                                }
                                else
                                    this->handshake(session);
                            }
                            else
                                session->callback(session->connection, ec);
                        });
                    }
                    else
                        session->callback(session->connection, ec);
                });
            }
            else
                write(session);
        }

        void handshake(const std::shared_ptr<Session> &session) {
            SSL_set_tlsext_host_name(session->connection->socket->native_handle(), this->host.c_str());

            session->connection->set_timeout(this->config.timeout_connect);
            session->connection->socket->async_handshake(asio::ssl::stream_base::client, [this, session](const error_code &ec) {
                session->connection->cancel_timeout();
                auto lock = session->connection->handler_runner->continue_lock();
                if(!lock)
                    return;
                if(!ec)
                    this->write(session);
                else
                    session->callback(session->connection, ec);
            });
        }
    };
    using https_client = http_client_<HTTPS>;
#endif

    template<typename socket_type>
    using client_response = typename http_client_<socket_type>::client_response;

    using client_request_header = CaseInsensitiveMultimap;
} // namespace cinatra

#endif /* CLIENT_HTTP_HPP */