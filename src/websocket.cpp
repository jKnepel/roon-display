#include "roon_display/websocket.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <stdexcept>

namespace roon_display {
namespace {

struct AddressGuard {
    addrinfo* value{};
    ~AddressGuard() { if (value) ::freeaddrinfo(value); }
};

void send_all(int fd, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) throw std::runtime_error("WebSocket send failed");
        sent += static_cast<std::size_t>(count);
    }
}

std::string base64(std::span<const std::uint8_t> input) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const std::uint32_t chunk = (static_cast<std::uint32_t>(input[i]) << 16) |
            (i + 1 < input.size() ? static_cast<std::uint32_t>(input[i + 1]) << 8 : 0) |
            (i + 2 < input.size() ? input[i + 2] : 0);
        output += alphabet[(chunk >> 18) & 0x3f];
        output += alphabet[(chunk >> 12) & 0x3f];
        output += i + 1 < input.size() ? alphabet[(chunk >> 6) & 0x3f] : '=';
        output += i + 2 < input.size() ? alphabet[chunk & 0x3f] : '=';
    }
    return output;
}

} // namespace

WebSocket::~WebSocket() { close(); }

void WebSocket::connect(const std::string& host, std::uint16_t port,
                        const std::string& path) {
    pending_.clear();
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    AddressGuard addresses;
    const auto port_text = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses.value) != 0)
        throw std::runtime_error("could not resolve Roon Core address");
    for (auto* address = addresses.value; address; address = address->ai_next) {
        socket_ = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_.load() < 0) continue;
        if (::connect(socket_.load(), address->ai_addr, address->ai_addrlen) == 0) break;
        ::close(socket_.load());
        socket_ = -1;
    }
    if (socket_.load() < 0) throw std::runtime_error("could not connect to Roon Core");

    std::array<std::uint8_t, 16> nonce{};
    std::random_device random;
    std::generate(nonce.begin(), nonce.end(), [&] { return random(); });
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + port_text +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
        base64(nonce) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send_all(socket_.load(), std::span(reinterpret_cast<const std::uint8_t*>(request.data()),
                                request.size()));

    std::string response;
    std::array<std::uint8_t, 2048> buffer{};
    while (response.find("\r\n\r\n") == std::string::npos) {
        const auto count = ::recv(socket_.load(), buffer.data(), buffer.size(), 0);
        if (count <= 0) throw std::runtime_error("WebSocket handshake was closed");
        response.append(reinterpret_cast<const char*>(buffer.data()), count);
        if (response.size() > 65536) throw std::runtime_error("WebSocket handshake too large");
    }
    if (!response.starts_with("HTTP/1.1 101") && !response.starts_with("HTTP/1.0 101"))
        throw std::runtime_error("Roon Core rejected WebSocket upgrade: " +
                                 response.substr(0, response.find("\r\n")));
    const auto body = response.find("\r\n\r\n") + 4;
    pending_.assign(response.begin() + static_cast<std::ptrdiff_t>(body), response.end());
}

void WebSocket::read_exact(std::span<std::uint8_t> output) {
    std::size_t offset = 0;
    if (!pending_.empty()) {
        const auto count = std::min(output.size(), pending_.size());
        std::copy_n(pending_.begin(), count, output.begin());
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
        offset = count;
    }
    while (offset < output.size()) {
        const auto count = ::recv(socket_.load(), output.data() + offset, output.size() - offset, 0);
        if (count <= 0) throw std::runtime_error("WebSocket connection closed");
        offset += static_cast<std::size_t>(count);
    }
}

void WebSocket::send_frame(std::uint8_t opcode, std::span<const std::uint8_t> payload) {
    if (socket_.load() < 0) throw std::runtime_error("WebSocket is not connected");
    std::vector<std::uint8_t> frame{static_cast<std::uint8_t>(0x80 | opcode)};
    if (payload.size() <= 125) {
        frame.push_back(static_cast<std::uint8_t>(0x80 | payload.size()));
    } else if (payload.size() <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        frame.push_back(static_cast<std::uint8_t>(payload.size()));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<std::uint8_t>(payload.size() >> shift));
    }
    std::array<std::uint8_t, 4> mask{};
    std::random_device random;
    std::generate(mask.begin(), mask.end(), [&] { return random(); });
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < payload.size(); ++i)
        frame.push_back(payload[i] ^ mask[i % mask.size()]);
    send_all(socket_.load(), frame);
}

void WebSocket::send_binary(std::span<const std::uint8_t> payload) {
    send_frame(0x2, payload);
}

std::vector<std::uint8_t> WebSocket::receive_message() {
    std::vector<std::uint8_t> message;
    bool started = false;
    for (;;) {
        std::array<std::uint8_t, 2> header{};
        read_exact(header);
        const bool final = header[0] & 0x80;
        const auto opcode = static_cast<std::uint8_t>(header[0] & 0x0f);
        const bool masked = header[1] & 0x80;
        std::uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            std::array<std::uint8_t, 2> extended{};
            read_exact(extended);
            length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
        } else if (length == 127) {
            std::array<std::uint8_t, 8> extended{};
            read_exact(extended);
            length = 0;
            for (const auto byte : extended) length = (length << 8) | byte;
        }
        if (length > 64 * 1024 * 1024) throw std::runtime_error("WebSocket frame too large");
        std::array<std::uint8_t, 4> mask{};
        if (masked) read_exact(mask);
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
        read_exact(payload);
        if (masked)
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];

        if (opcode == 0x8) throw std::runtime_error("Roon Core closed WebSocket");
        if (opcode == 0x9) { send_frame(0xa, payload); continue; }
        if (opcode == 0xa) continue;
        if (opcode == 0x1 || opcode == 0x2) {
            if (started) throw std::runtime_error("unexpected new WebSocket message");
            started = true;
            message = std::move(payload);
        } else if (opcode == 0x0 && started) {
            message.insert(message.end(), payload.begin(), payload.end());
        } else {
            throw std::runtime_error("unsupported WebSocket opcode");
        }
        if (final) return message;
    }
}

void WebSocket::close() {
    const int socket = socket_.exchange(-1);
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

} // namespace roon_display

