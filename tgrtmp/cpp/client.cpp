#include "client.h"

#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tgrtmp {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_t fd) { closesocket(fd); }
int last_net_error() { return WSAGetLastError(); }
void startup_sockets() {
    static bool done = [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        return true;
    }();
    (void)done;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
void close_socket(socket_t fd) { ::close(fd); }
[[maybe_unused]] int last_net_error() { return errno; }
void startup_sockets() {
    // Writes to a connection that the server already closed would raise
    // SIGPIPE and kill the process: ignore it and handle the errors instead.
    static bool done = [] {
        ::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}
#endif

// Waits until the socket is writable, e.g. a non-blocking connect completing.
// POSIX poll and Windows WSAPoll share the shape, not the flags.
int wait_socket_writable(socket_t fd, int timeout_ms) {
#ifdef _WIN32
    pollfd pfd{fd, POLLWRNORM, 0};
    return WSAPoll(&pfd, 1, timeout_ms);
#else
    pollfd pfd{fd, POLLOUT, 0};
    return ::poll(&pfd, 1, timeout_ms);
#endif
}

std::string net_error_string(const std::string &prefix) {
#ifdef _WIN32
    char buffer[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, WSAGetLastError(), 0, buffer,
                   sizeof(buffer), nullptr);
    return prefix + ": " + buffer;
#else
    return prefix + ": " + std::strerror(errno);
#endif
}

uint32_t read_be32_view(std::string_view data, size_t offset) {
    const auto *p = reinterpret_cast<const unsigned char *>(data.data()) + offset;
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Optional wire transcript for field debugging: set TGRTMP_WIRE_LOG=<path>
// to append one line per outgoing message and every connection event.
FILE *wire_log() {
    static FILE *log = [] {
        const char *path = getenv("TGRTMP_WIRE_LOG");
        return path ? fopen(path, "a") : nullptr;
    }();
    return log;
}

void wire_log_message(uint32_t csid, uint8_t type_id, uint32_t stream_id, uint32_t timestamp_ms,
                      size_t length) {
    if (FILE *log = wire_log()) {
        fprintf(log, "%lld OUT csid=%u type=%u stream=%u ts=%u len=%zu\n",
                (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()),
                csid, type_id, stream_id, timestamp_ms, length);
        fflush(log);
    }
}

} // namespace

void Client::Transaction::complete(std::vector<AmfValue> values) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (done)
            return;
        done = true;
        result = std::move(values);
    }
    cv.notify_all();
}

void Client::Transaction::fail(std::string message) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (done)
            return;
        done = true;
        is_error = true;
        error = std::move(message);
    }
    cv.notify_all();
}

Client::~Client() {
    close();
    if (reader_.joinable())
        reader_.join();
    if (ssl_) {
        SSL_free(static_cast<SSL *>(ssl_));
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(static_cast<SSL_CTX *>(ssl_ctx_));
        ssl_ctx_ = nullptr;
    }
    if (fd_ != kInvalidSocket) {
        close_socket(fd_);
        fd_ = kInvalidSocket;
    }
}

bool Client::await_close(double timeout_s) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return closed_cv_.wait_for(lock, std::chrono::duration<double>(timeout_s),
                               [this] { return closed_.load(std::memory_order_acquire); });
}

void Client::fail(const std::string &error) {
    if (closed_.load(std::memory_order_acquire))
        return;
    if (FILE *log = wire_log()) {
        fprintf(log, "%lld CLOSE error=%s\n",
                (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()),
                error.c_str());
        fflush(log);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_ = error;
    }
    closed_.store(true, std::memory_order_release);
    if (fd_ != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(fd_, SD_BOTH);
#else
        ::shutdown(fd_, SHUT_RDWR); // Wake the reader thread.
#endif
    }
    {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        std::string message = error.empty() ? "The RTMP connection was closed" : error;
        for (auto &[id, transaction] : transactions_) {
            if (!transaction)
                continue;
            std::lock_guard<std::mutex> tlock(transaction->mutex);
            if (!transaction->done) {
                transaction->done = true;
                transaction->is_error = true;
                transaction->error = message;
            }
            transaction->cv.notify_all();
        }
        transactions_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(publish_mutex_);
        if (!publish_done_) {
            publish_done_ = true;
            publish_ok_ = false;
            publish_error_ = error.empty() ? "The RTMP connection was closed" : error;
        }
        publish_cv_.notify_all();
    }
    closed_cv_.notify_all();
}

void Client::close() {
    if (closed_.load(std::memory_order_acquire))
        return;
    if (stream_id_ != 0) {
        // Best-effort teardown notices; the reference client sends them on the
        // command chunk stream (stream id 0), not the media stream.
        try {
            call("FCUnpublish", {amf0::null_value(), amf0::string(key_)}, 0, /*notify=*/true);
            call("deleteStream", {amf0::null_value(), amf0::number(stream_id_)}, 0,
                 /*notify=*/true);
        } catch (const std::exception &) {
        }
    }
    fail("");
}

namespace {

// Resolve + connect with a timeout; returns a blocking socket.
socket_t connect_tcp(const std::string &host, uint16_t port, double timeout_s,
                     std::string &remote_ip) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *list = nullptr;
    if (int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list);
        rc != 0 || list == nullptr) {
        throw std::runtime_error("Could not resolve the RTMP host " + host);
    }
    std::string error = "Could not connect to the RTMP server";
    for (addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
        socket_t fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kInvalidSocket) {
            error = net_error_string("Could not create the socket");
            continue;
        }
#ifdef _WIN32
        u_long non_blocking = 1;
        ::ioctlsocket(fd, FIONBIO, &non_blocking);
#else
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
        int rc = ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc != 0) {
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
            if (errno != EINPROGRESS) {
#endif
                error = net_error_string("Could not connect");
                close_socket(fd);
                continue;
            }
            int timeout = static_cast<int>(timeout_s * 1000);
            if (wait_socket_writable(fd, timeout) != 1) {
                error = "Timed out connecting to the RTMP server";
                close_socket(fd);
                continue;
            }
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socket_error),
                         &length);
            if (socket_error != 0) {
                error = "Could not connect to the RTMP server: " +
                        std::string(std::strerror(socket_error));
                close_socket(fd);
                continue;
            }
        }
#ifdef _WIN32
        u_long blocking = 0;
        ::ioctlsocket(fd, FIONBIO, &blocking);
        int no_delay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay),
                     sizeof(no_delay));
#else
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        int no_delay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
#endif
        // The numeric address this socket actually reached: the log shows the
        // server behind the name, and DNS may answer differently each call.
        char address[NI_MAXHOST] = "";
        ::getnameinfo(ai->ai_addr, ai->ai_addrlen, address, sizeof(address), nullptr, 0,
                      NI_NUMERICHOST);
        remote_ip = address;

        freeaddrinfo(list);
        return fd;
    }
    freeaddrinfo(list);
    throw std::runtime_error(error);
}

} // namespace

std::unique_ptr<Client> Client::publish(std::string_view url, std::string_view key,
                                        const Options &options) {
    startup_sockets();

    auto client = std::unique_ptr<Client>(new Client);
    client->options_ = options;
    client->key_ = std::string(key);

    std::string input(url);
    size_t scheme_end = input.find("://");
    std::string scheme = scheme_end == std::string::npos ? "" : input.substr(0, scheme_end);
    if (scheme != "rtmp" && scheme != "rtmps") {
        throw std::runtime_error("Invalid RTMP URL: " + input);
    }
    std::string rest = input.substr(scheme_end + 3);
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash);
    size_t query = path.find('?');
    if (query != std::string::npos)
        path = path.substr(0, query);
    client->tls_ = scheme == "rtmps";
    uint16_t default_port = client->tls_ ? 443 : 1935;
    size_t colon = authority.rfind(':');
    bool explicit_port = false;
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        client->host_ = authority.substr(0, colon);
        client->port_ = static_cast<uint16_t>(std::stoi(authority.substr(colon + 1)));
        explicit_port = true;
    } else {
        client->host_ = authority;
        client->port_ = default_port;
    }
    size_t app_start = path.find_first_not_of('/');
    client->app_ = app_start == std::string::npos ? "" : path.substr(app_start);
    while (!client->app_.empty() && client->app_.back() == '/')
        client->app_.pop_back();
    client->tc_url_ = scheme + "://" + client->host_ +
                      (explicit_port ? ":" + std::to_string(client->port_) : "") + "/" +
                      client->app_;

    client->fd_ = connect_tcp(client->host_, client->port_, options.connect_timeout_s,
                              client->remote_);
    if (client->tls_) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx)
            throw std::runtime_error("Could not create the TLS context");
        if (options.verify_tls)
            SSL_CTX_set_default_verify_paths(ctx);
        client->ssl_ctx_ = ctx;
        SSL *ssl = SSL_new(ctx);
        client->ssl_ = ssl;
        SSL_set_fd(ssl, client->fd_);
        SSL_set_tlsext_host_name(ssl, client->host_.c_str());
        SSL_set_verify(ssl, options.verify_tls ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
        if (SSL_connect(ssl) != 1) {
            unsigned long code = ERR_get_error();
            char buffer[256];
            ERR_error_string_n(code, buffer, sizeof(buffer));
            throw std::runtime_error(
                std::string("The TLS handshake with the RTMP server failed: ") + buffer);
        }
        if (options.verify_tls && SSL_get_verify_result(ssl) != X509_V_OK) {
            throw std::runtime_error("The certificate of the RTMP server could not be verified");
        }
    }

    client->handshake_();

    // Wire the parser to the message dispatcher. The handler borrows the
    // client; both live and die together (the parser is a member).
    Client &ref = *client;
    client->parser_.set_handler([&ref](uint8_t type_id, uint32_t timestamp_ms, uint32_t stream_id,
                                       std::string_view payload) {
        ref.on_message(type_id, timestamp_ms, stream_id, payload);
    });

    client->reader_ = std::thread([c = client.get()] { c->run_reader(); });

    try {
        client->writer_.set_chunk_size(options.out_chunk_size);
        client->write_message(csid::CONTROL, msg::SET_CHUNK_SIZE, 0, 0,
                              std::string("\x00\x00\x10\x00", 4));

        // The reference client awaits the connect result before continuing.
        client->await(client->call(
            "connect",
            {amf0::object({{"app", amf0::string(client->app_)},
                           {"type", amf0::string("nonprivate")},
                           {"flashVer", amf0::string("FMLE/3.0 (compatible; tgrtmp)")},
                           {"tcUrl", amf0::string(client->tc_url_)}})},
            0, false));
        client->call("releaseStream", {amf0::null_value(), amf0::string(client->key_)}, 0, true);
        client->call("FCPublish", {amf0::null_value(), amf0::string(client->key_)}, 0, true);
        std::vector<AmfValue> result =
            client->await(client->call("createStream", {amf0::null_value()}, 0, false));
        client->stream_id_ =
            result.size() > 1 ? static_cast<uint32_t>(result[1].as_number(1.0)) : 1;
        client->call("publish",
                     {amf0::null_value(), amf0::string(client->key_), amf0::string("live")},
                     client->stream_id_, true);

        // The server answers publish with an onStatus.
        std::unique_lock<std::mutex> lock(client->publish_mutex_);
        bool ok = client->publish_cv_.wait_for(
            lock, std::chrono::duration<double>(options.command_timeout_s),
            [&] { return client->publish_done_; });
        if (!ok)
            throw std::runtime_error("The RTMP server did not answer the publish command");
        if (!client->publish_ok_)
            throw std::runtime_error(client->publish_error_);
    } catch (...) {
        client->fail("");
        throw;
    }
    return client;
}

void Client::handshake_() {
    // The simple handshake: C0+C1, S0+S1+S2, C2 (RTMP spec §5.2).
    std::string c0c1;
    c0c1.reserve(1537);
    c0c1.push_back('\x03');
    c0c1.append(8, '\x00');
    std::string random(1528, '\x00');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(random.data()), 1528) != 1) {
        std::mt19937 rng{std::random_device{}()};
        for (auto &byte : random)
            byte = static_cast<char>(rng());
    }
    c0c1.append(random);
    write_all(c0c1);

    std::string response;
    response.reserve(3073);
    while (response.size() < 3073) {
        char buffer[65536];
        int got;
        if (ssl_) {
            got = SSL_read(static_cast<SSL *>(ssl_), buffer, static_cast<int>(sizeof(buffer)));
            if (got <= 0) {
                throw std::runtime_error(
                    "The RTMP server closed the connection during the handshake");
            }
        } else {
            got = static_cast<int>(::recv(fd_, buffer, sizeof(buffer), 0));
            if (got <= 0) {
                throw std::runtime_error(
                    "The RTMP server closed the connection during the handshake");
            }
        }
        response.append(buffer, static_cast<size_t>(got));
    }
    if (static_cast<unsigned char>(response[0]) != 0x03) {
        throw std::runtime_error("Unsupported RTMP version " +
                                 std::to_string(static_cast<unsigned char>(response[0])));
    }
    // C2 echoes S1; anything after S2 already belongs to the chunk stream.
    std::string c2 = response.substr(1, 1536);
    write_all(c2);
    parser_.feed(std::string_view(response).substr(1 + 1536 * 2));
}

void Client::write_all(std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int got;
        if (ssl_) {
            got = SSL_write(static_cast<SSL *>(ssl_), data.data() + sent,
                            static_cast<int>(data.size() - sent));
            if (got <= 0) {
                int error = SSL_get_error(static_cast<SSL *>(ssl_), got);
                fail("The RTMP connection failed while writing (TLS error " +
                     std::to_string(error) + ")");
                throw std::runtime_error("The RTMP connection failed while writing");
            }
        } else {
#ifdef _WIN32
            got = ::send(fd_, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            got =
                static_cast<int>(::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL));
#endif
            if (got <= 0) {
                fail(net_error_string("The RTMP connection failed while writing"));
                throw std::runtime_error("The RTMP connection failed while writing");
            }
        }
        sent += static_cast<size_t>(got);
    }
}

void Client::write_message(uint32_t chunk_stream, uint8_t type_id, uint32_t stream_id,
                           uint32_t timestamp_ms, std::string_view payload) {
    wire_log_message(chunk_stream, type_id, stream_id, timestamp_ms, payload.size());
    std::string out;
    size_t chunks = payload.size() / writer_.chunk_size() + 1;
    out.reserve(payload.size() + 13 + chunks * 5);
    writer_.write_message(out, chunk_stream, type_id, stream_id, timestamp_ms, payload);
    std::lock_guard<std::mutex> lock(write_mutex_);
    write_all(out);
}

std::shared_ptr<Client::Transaction>
Client::call(const std::string &name, std::vector<AmfValue> args, uint32_t stream_id, bool notify) {
    auto transaction = std::make_shared<Transaction>();
    {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        transaction->id = ++next_transaction_;
        if (!notify)
            transactions_[transaction->id] = transaction;
    }

    std::vector<AmfValue> message;
    message.reserve(2 + args.size());
    message.emplace_back(name);
    message.emplace_back(static_cast<double>(transaction->id));
    for (auto &arg : args)
        message.push_back(std::move(arg));

    std::string body;
    for (const auto &value : message)
        amf0::encode(body, value);
    // As the reference client does, only `publish` is sent on the media
    // stream; every other command rides the command chunk stream (id 0).
    uint32_t command_stream = name == "publish" ? stream_id : 0;
    write_message(csid::COMMAND, msg::COMMAND_AMF0, command_stream, 0, body);
    return transaction;
}

std::vector<AmfValue> Client::await(const std::shared_ptr<Transaction> &transaction) {
    std::unique_lock<std::mutex> lock(transaction->mutex);
    bool done =
        transaction->cv.wait_for(lock, std::chrono::duration<double>(options_.command_timeout_s),
                                 [&] { return transaction->done; });
    if (!done) {
        throw std::runtime_error("The RTMP server did not answer the " +
                                 std::to_string(transaction->id) + " command in time");
    }
    if (transaction->is_error)
        throw std::runtime_error(transaction->error);
    return std::move(transaction->result);
}

void Client::run_reader() {
    try {
        while (read_some()) {
            // Acknowledge the received bytes within the window the server
            // asked for, as the reference client does.
            if (bytes_received_ - bytes_acknowledged_ >= peer_ack_window_) {
                bytes_acknowledged_ = bytes_received_;
                std::string ack(4, '\x00');
                uint32_t received = static_cast<uint32_t>(bytes_received_ & 0xFFFFFFFF);
                ack[0] = static_cast<char>((received >> 24) & 0xFF);
                ack[1] = static_cast<char>((received >> 16) & 0xFF);
                ack[2] = static_cast<char>((received >> 8) & 0xFF);
                ack[3] = static_cast<char>(received & 0xFF);
                write_message(csid::CONTROL, msg::ACKNOWLEDGEMENT, 0, 0, ack);
            }
        }
        fail("The RTMP server closed the connection");
    } catch (const std::exception &e) {
        fail(e.what());
    } catch (...) {
        fail("The RTMP connection failed");
    }
}

bool Client::read_some() {
    char buffer[65536];
    int got;
    if (ssl_) {
        got = SSL_read(static_cast<SSL *>(ssl_), buffer, static_cast<int>(sizeof(buffer)));
        if (got <= 0) {
            int error = SSL_get_error(static_cast<SSL *>(ssl_), got);
            if (error == SSL_ERROR_ZERO_RETURN)
                return false;
            throw std::runtime_error("The RTMP server closed the connection");
        }
    } else {
        got = static_cast<int>(::recv(fd_, buffer, sizeof(buffer), 0));
        if (got <= 0)
            return false;
    }
    bytes_received_ += static_cast<uint64_t>(got);
    parser_.feed(std::string_view(buffer, static_cast<size_t>(got)));
    return true;
}

void Client::on_message(uint8_t type_id, uint32_t, uint32_t, std::string_view payload) {
    switch (type_id) {
    case msg::SET_CHUNK_SIZE:
        if (payload.size() >= 4) {
            parser_.set_chunk_size(read_be32_view(payload, 0) & 0x7FFFFFFF);
        }
        return;
    case msg::ABORT:
        if (payload.size() >= 4)
            parser_.abort_stream(read_be32_view(payload, 0));
        return;
    case msg::WINDOW_ACK_SIZE:
        if (payload.size() >= 4)
            peer_ack_window_ = read_be32_view(payload, 0);
        return;
    case msg::SET_PEER_BANDWIDTH:
        // The response to SetPeerBandwidth is a Window Acknowledgement
        // Size message (RTMP spec §5.4).
        if (payload.size() >= 4) {
            write_message(csid::CONTROL, msg::WINDOW_ACK_SIZE, 0, 0, payload.substr(0, 4));
        }
        return;
    case msg::USER_CONTROL:
        // Ping request (6) -> ping response (7) echoing the pinged time.
        if (payload.size() >= 6 && (static_cast<unsigned char>(payload[0]) << 8 |
                                    static_cast<unsigned char>(payload[1])) == 6) {
            std::string pong;
            pong.push_back('\x00');
            pong.push_back('\x07');
            pong.append(payload.substr(2, 4));
            write_message(csid::CONTROL, msg::USER_CONTROL, 0, 0, pong);
        }
        return;
    case msg::COMMAND_AMF0:
        on_command(amf0::decode(payload));
        return;
    default:
        return;
    }
}

std::string Client::describe(const AmfValue *info) {
    if (info == nullptr)
        return "unknown error";
    if (auto *object = info->as_object()) {
        (void)object;
        std::string code, description;
        if (const AmfValue *value = info->find("code")) {
            if (auto *s = value->as_string())
                code = *s;
        }
        if (const AmfValue *value = info->find("description")) {
            if (auto *s = value->as_string())
                description = *s;
        }
        return code + " " + description;
    }
    if (auto *s = info->as_string())
        return *s;
    if (info->is_number())
        return std::to_string(info->as_number());
    return "unknown error";
}

void Client::on_command(std::vector<AmfValue> &&values) {
    if (values.empty() || !values[0].is_string())
        return;
    std::string name = *values[0].as_string();
    int64_t transaction = static_cast<int64_t>(values.size() > 1 ? values[1].as_number(0.0) : 0.0);

    if (name == "_result" || name == "_error") {
        std::shared_ptr<Transaction> pending;
        {
            std::lock_guard<std::mutex> lock(transactions_mutex_);
            if (auto it = transactions_.find(static_cast<uint32_t>(transaction));
                it != transactions_.end()) {
                pending = std::move(it->second);
                transactions_.erase(it);
            }
        }
        if (!pending)
            return;
        if (name == "_result") {
            std::vector<AmfValue> rest(values.begin() + 2, values.end());
            pending->complete(std::move(rest));
        } else {
            const AmfValue *info = values.size() > 3 ? &values[3] : nullptr;
            pending->fail("The RTMP server refused the command: " + describe(info));
        }
        return;
    }

    if (name == "onStatus") {
        const AmfValue *info = values.size() > 3 ? &values[3] : nullptr;
        std::string code = info ? (info->find("code") && info->find("code")->as_string()
                                       ? *info->find("code")->as_string()
                                       : "")
                                : "";
        std::lock_guard<std::mutex> lock(publish_mutex_);
        if (publish_done_)
            return;
        if (code == "NetStream.Publish.Start") {
            publish_done_ = true;
            publish_ok_ = true;
        } else if (info && info->find("level") && info->find("level")->as_string() &&
                   *info->find("level")->as_string() == "error") {
            publish_done_ = true;
            publish_ok_ = false;
            publish_error_ = "The RTMP server refused the stream: " + describe(info);
        }
        publish_cv_.notify_all();
    }
}

void Client::send_metadata(std::string_view amf_body) {
    write_message(csid::DATA, msg::DATA_AMF0, stream_id_, 0, amf_body);
}

void Client::send_audio(uint32_t timestamp_ms, std::string_view body) {
    write_message(csid::AUDIO, msg::AUDIO, stream_id_, timestamp_ms, body);
}

void Client::send_video(uint32_t timestamp_ms, std::string_view body) {
    write_message(csid::VIDEO, msg::VIDEO, stream_id_, timestamp_ms, body);
}

void Client::send_video_annexb(uint32_t timestamp_ms, bool keyframe, std::string_view annexb,
                               uint32_t composition_time) {
    wire_log_message(csid::VIDEO, msg::VIDEO, stream_id_, timestamp_ms, annexb.size() + 5);
    std::string out;
    writer_.write_avc_frame(out, csid::VIDEO, stream_id_, timestamp_ms, keyframe, annexb,
                            composition_time);
    std::lock_guard<std::mutex> lock(write_mutex_);
    write_all(out);
}

} // namespace tgrtmp
