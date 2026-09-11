#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace roon_display {

class WebSocket {
public:
    WebSocket() = default;
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    void connect(const std::string& host, std::uint16_t port,
                 const std::string& path = "/api");
    void send_binary(std::span<const std::uint8_t> payload);
    std::vector<std::uint8_t> receive_message();
    void close();

private:
    int socket_{-1};
    std::vector<std::uint8_t> pending_;

    void send_frame(std::uint8_t opcode, std::span<const std::uint8_t> payload);
    void read_exact(std::span<std::uint8_t> output);
};

} // namespace roon_display

