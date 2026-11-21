module;

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/ssl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module boost_wrapper;

export namespace sparsity::boost_wrapper {
    namespace asio {
        using io_context = ::boost::asio::io_context;

        namespace ip {
            using tcp = ::boost::asio::ip::tcp;
        }

        namespace error {
            inline constexpr auto connection_reset = ::boost::asio::error::connection_reset;
            inline constexpr auto eof = ::boost::asio::error::eof;
            inline constexpr auto try_again = ::boost::asio::error::try_again;
            inline constexpr auto would_block = ::boost::asio::error::would_block;
            inline constexpr auto not_connected = ::boost::asio::error::not_connected;
        }

        namespace socket_base {
            using reuse_address = ::boost::asio::socket_base::reuse_address;
            inline constexpr auto max_listen_connections = ::boost::asio::socket_base::max_listen_connections;
        }
    }

    namespace beast {
        using flat_buffer = ::boost::beast::flat_buffer;
        using role_type = ::boost::beast::role_type;
        using string_view = ::boost::beast::string_view;
        using tcp_stream = ::boost::beast::tcp_stream;

        namespace http {
            using field = ::boost::beast::http::field;
            using status = ::boost::beast::http::status;
            using string_body = ::boost::beast::http::string_body;
            using verb = ::boost::beast::http::verb;

            namespace error {
                inline constexpr auto end_of_stream = ::boost::beast::http::error::end_of_stream;
            }
        }

        namespace websocket::error {
            inline constexpr auto closed = ::boost::beast::websocket::error::closed;
        }
    }

    namespace system {
        using error_code = ::boost::system::error_code;
        using system_error = ::boost::system::system_error;
    }

    using error_code = system::error_code;
    using system_error = system::system_error;
    using FlatBuffer = beast::flat_buffer;
    using TcpStream = beast::tcp_stream;
    using WebSocket = ::boost::beast::websocket::stream<TcpStream>;
    using HttpRequest = ::boost::beast::http::request<::boost::beast::http::string_body>;
    using HttpRequestParser = ::boost::beast::http::request_parser<::boost::beast::http::string_body>;
    using HttpResponse = ::boost::beast::http::response<::boost::beast::http::string_body>;

    struct WebSocketMessage {
        std::string payload;
        bool text;
    };

    WebSocket make_websocket(TcpStream stream) {
        return WebSocket(std::move(stream));
    }

    void configure_websocket(WebSocket &socket, const std::size_t maximum_message_size) {
        socket.set_option(::boost::beast::websocket::stream_base::timeout::suggested(
            ::boost::beast::role_type::server));
        socket.read_message_max(maximum_message_size);
    }

    void accept_websocket(WebSocket &socket, const HttpRequest &request) {
        socket.accept(request);
    }

    bool is_websocket_upgrade(const HttpRequest &request) {
        return ::boost::beast::websocket::is_upgrade(request);
    }

    int websocket_native_handle(WebSocket &socket) {
        return socket.next_layer().socket().native_handle();
    }

    WebSocketMessage read_websocket(WebSocket &socket) {
        FlatBuffer buffer;
        socket.read(buffer);
        return {
            ::boost::beast::buffers_to_string(buffer.data()),
            socket.got_text()
        };
    }

    void write_websocket_text(WebSocket &socket, const std::string_view payload) {
        socket.text(true);
        socket.write(::boost::asio::buffer(payload));
    }

    void read_http(TcpStream &stream, FlatBuffer &buffer, HttpRequestParser &parser, error_code &error) {
        ::boost::beast::http::read(stream, buffer, parser, error);
    }

    void write_http(TcpStream &stream, const HttpResponse &response) {
        ::boost::beast::http::write(stream, response);
    }

    std::string http_verb_string(const beast::http::verb verb) {
        const auto value = ::boost::beast::http::to_string(verb);
        return {value.data(), value.size()};
    }

    std::string http_target_string(const HttpRequest &request) {
        const auto value = request.target();
        return {value.data(), value.size()};
    }

    struct HttpUrl {
        std::string scheme;
        std::string host;
        unsigned short port{0};
        std::string target;
    };

    using HttpHeaders = std::vector<std::pair<std::string, std::string>>;
    using HttpSink = std::function<bool(std::string_view)>;

    struct HttpResult {
        int status{0};
        std::string body;
        std::multimap<std::string, std::string> headers;
    };

    class HttpCanceled final : public std::runtime_error {
    public:
        HttpCanceled() : std::runtime_error("request canceled") {
        }
    };

    class HttpCancel {
    public:
        void set(std::function<void()> callback);
        void clear();
        void cancel();
        bool canceled() const;

    private:
        std::atomic_bool canceled_{false};
        std::mutex mutex_;
        std::function<void()> callback_;
    };

    HttpUrl http_parse_url(std::string_view text);
    HttpResult http_perform(const std::string &method, const HttpUrl &url, const HttpHeaders &headers,
                            const std::string &body, const HttpSink &sink, HttpCancel &cancel);
    HttpResult http_request(const std::string &method, const HttpUrl &url, const HttpHeaders &headers,
                            const std::string &body, HttpCancel *cancel = nullptr);
    HttpResult http_get(const HttpUrl &url, const HttpHeaders &headers = {}, HttpCancel *cancel = nullptr);
    HttpResult http_post(const HttpUrl &url, const HttpHeaders &headers, const std::string &body,
                         HttpCancel *cancel = nullptr);
}

namespace sparsity::boost_wrapper {
    namespace http = ::boost::beast::http;
    namespace ssl = ::boost::asio::ssl;
    using tcp = ::boost::asio::ip::tcp;

    namespace detail {
        std::string lower(std::string value) {
            for (auto &character: value) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            return value;
        }

        std::string host_header(const HttpUrl &url) {
            const auto default_port = url.scheme == "https" ? 443 : 80;
            return url.port == default_port ? url.host : url.host + ':' + std::to_string(url.port);
        }

        template<typename Stream>
        void open_stream(Stream &stream, const HttpUrl &url) {
            auto &lowest = ::boost::beast::get_lowest_layer(stream);
            lowest.expires_after(std::chrono::seconds(30));
            tcp::resolver resolver(lowest.get_executor());
            const auto endpoints = resolver.resolve(url.host, std::to_string(url.port));
            lowest.connect(endpoints);
        }

        template<typename Stream>
        HttpResult exchange(Stream &stream, const HttpUrl &url, const std::string &method,
                            const HttpHeaders &headers, const std::string &body,
                            const HttpSink &sink, HttpCancel &cancel) {
            http::request<http::string_body> request;
            request.method_string(method);
            request.target(url.target);
            request.version(11);
            request.set(http::field::host, host_header(url));
            request.set(http::field::user_agent, "sparsity/0.1");
            request.set(http::field::accept, "*/*");
            for (const auto &[name, value]: headers) request.set(name, value);
            request.body() = body;
            request.prepare_payload();
            request.keep_alive(false);
            http::write(stream, request);

            ::boost::beast::flat_buffer buffer;
            http::response_parser<http::buffer_body> parser;
            parser.body_limit(64U << 20U);
            http::read_header(stream, buffer, parser);

            HttpResult result;
            result.status = parser.get().result_int();
            for (const auto &field: parser.get().base()) {
                result.headers.emplace(lower(std::string(field.name_string())), std::string(field.value()));
            }

            const bool failed = result.status >= 400;
            std::string error_body;
            while (!parser.is_done()) {
                if (cancel.canceled()) throw HttpCanceled();
                char data[16384];
                parser.get().body().data = data;
                parser.get().body().size = sizeof(data);
                ::boost::beast::error_code error;
                http::read_some(stream, buffer, parser, error);
                if (error == http::error::need_buffer) error = {};
                if (error) {
                    if (cancel.canceled()) throw HttpCanceled();
                    if (error == ::boost::asio::error::eof || error == ssl::error::stream_truncated) break;
                    throw ::boost::system::system_error(error);
                }
                const auto size = sizeof(data) - parser.get().body().size;
                if (size == 0) continue;
                if (failed) {
                    error_body.append(data, size);
                    continue;
                }
                if (sink && !sink(std::string_view(data, size))) break;
            }
            if (failed) result.body = std::move(error_body);
            return result;
        }
    }
}

export namespace sparsity::boost_wrapper {

    void HttpCancel::set(std::function<void()> callback) {
        std::function<void()> immediate;
        {
            std::lock_guard lock(mutex_);
            callback_ = std::move(callback);
            if (canceled_) immediate = callback_;
        }
        if (immediate) immediate();
    }

    void HttpCancel::clear() {
        std::lock_guard lock(mutex_);
        callback_ = {};
    }

    void HttpCancel::cancel() {
        std::function<void()> callback;
        {
            std::lock_guard lock(mutex_);
            canceled_ = true;
            callback = callback_;
        }
        if (callback) callback();
    }

    bool HttpCancel::canceled() const {
        return canceled_.load(std::memory_order_relaxed);
    }

    HttpUrl http_parse_url(const std::string_view text) {
        const auto scheme_end = text.find("://");
        if (scheme_end == std::string_view::npos) throw std::runtime_error("invalid URL");
        HttpUrl url;
        url.scheme = std::string(text.substr(0, scheme_end));
        if (url.scheme != "http" && url.scheme != "https") throw std::runtime_error("unsupported URL scheme");
        url.port = url.scheme == "https" ? 443 : 80;

        const auto rest = text.substr(scheme_end + 3);
        const auto path = rest.find('/');
        const auto query = rest.find('?');
        const auto authority_end = std::min(path == std::string_view::npos ? rest.size() : path,
                                            query == std::string_view::npos ? rest.size() : query);
        const auto authority = rest.substr(0, authority_end);
        if (path != std::string_view::npos) url.target = std::string(rest.substr(path));
        else if (query != std::string_view::npos) url.target = "/" + std::string(rest.substr(query));
        else url.target = "/";

        if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
            url.host = std::string(authority.substr(0, colon));
            const auto port = std::string(authority.substr(colon + 1));
            if (!port.empty()) url.port = static_cast<unsigned short>(std::stoi(port));
        } else {
            url.host = std::string(authority);
        }
        if (url.host.empty()) throw std::runtime_error("invalid URL host");
        return url;
    }

    HttpResult http_perform(const std::string &method, const HttpUrl &url, const HttpHeaders &headers,
                            const std::string &body, const HttpSink &sink, HttpCancel &cancel) {
        if (cancel.canceled()) throw HttpCanceled();
        ::boost::asio::io_context context;
        if (url.scheme == "https") {
            ssl::context ssl_context(ssl::context::tls_client);
            ssl_context.set_default_verify_paths();
            ssl_context.set_verify_mode(ssl::verify_peer);
            ::boost::beast::ssl_stream<::boost::beast::tcp_stream> stream(context, ssl_context);
            detail::open_stream(stream, url);
            if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
                throw std::runtime_error("TLS SNI failed");
            }
            stream.set_verify_callback(ssl::host_name_verification(url.host));
            stream.handshake(ssl::stream_base::client);
            ::boost::beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(300));
            cancel.set([&stream] {
                ::boost::system::error_code ignored;
                ::boost::beast::get_lowest_layer(stream).socket().cancel(ignored);
            });
            try {
                auto result = detail::exchange(stream, url, method, headers, body, sink, cancel);
                cancel.clear();
                return result;
            } catch (...) {
                cancel.clear();
                throw;
            }
        }

        ::boost::beast::tcp_stream stream(context);
        detail::open_stream(stream, url);
        stream.expires_after(std::chrono::seconds(300));
        cancel.set([&stream] {
            ::boost::system::error_code ignored;
            stream.socket().cancel(ignored);
        });
        try {
            auto result = detail::exchange(stream, url, method, headers, body, sink, cancel);
            cancel.clear();
            return result;
        } catch (...) {
            cancel.clear();
            throw;
        }
    }

    HttpResult http_request(const std::string &method, const HttpUrl &url, const HttpHeaders &headers,
                            const std::string &body, HttpCancel *cancel) {
        HttpCancel local;
        HttpCancel &token = cancel == nullptr ? local : *cancel;
        std::string received;
        auto result = http_perform(method, url, headers, body,
            [&received](const std::string_view chunk) {
                received.append(chunk);
                return true;
            }, token);
        if (result.status < 400) result.body = std::move(received);
        return result;
    }

    HttpResult http_get(const HttpUrl &url, const HttpHeaders &headers, HttpCancel *cancel) {
        return http_request("GET", url, headers, {}, cancel);
    }

    HttpResult http_post(const HttpUrl &url, const HttpHeaders &headers, const std::string &body,
                         HttpCancel *cancel) {
        return http_request("POST", url, headers, body, cancel);
    }

}
