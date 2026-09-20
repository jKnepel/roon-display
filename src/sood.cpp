#include "roon_display/sood.hpp"

#include <arpa/inet.h>
#ifndef ROON_DISPLAY_TIZEN
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <random>
#include <stdexcept>

namespace roon_display {
namespace {

std::string transaction_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string value(36, '0');
    for (auto& c : value) c = hex[random() & 0xf];
    value[8] = value[13] = value[18] = value[23] = '-';
    value[14] = '4';
    return value;
}

struct SocketGuard {
    int fd{-1};
    ~SocketGuard() { if (fd >= 0) ::close(fd); }
};

} // namespace

std::vector<std::uint8_t> encode_sood_query(
    const std::map<std::string, std::optional<std::string>>& properties) {
    std::vector<std::uint8_t> result{'S', 'O', 'O', 'D', 2, 'Q'};
    for (const auto& [name, value] : properties) {
        if (name.empty() || name.size() > 255) throw std::invalid_argument("invalid SOOD key");
        result.push_back(static_cast<std::uint8_t>(name.size()));
        result.insert(result.end(), name.begin(), name.end());
        if (!value) {
            result.push_back(0xff);
            result.push_back(0xff);
        } else {
            if (value->size() >= 65535) throw std::invalid_argument("SOOD value too long");
            result.push_back(static_cast<std::uint8_t>(value->size() >> 8));
            result.push_back(static_cast<std::uint8_t>(value->size()));
            result.insert(result.end(), value->begin(), value->end());
        }
    }
    return result;
}

std::optional<SoodMessage> parse_sood(std::span<const std::uint8_t> bytes,
                                      std::string source_address,
                                      std::uint16_t source_port) {
    if (bytes.size() < 6 || std::memcmp(bytes.data(), "SOOD", 4) != 0 || bytes[4] != 2)
        return std::nullopt;
    SoodMessage result{static_cast<char>(bytes[5]), std::move(source_address), source_port, {}};
    std::size_t pos = 6;
    while (pos < bytes.size()) {
        const auto name_length = bytes[pos++];
        if (name_length == 0 || pos + name_length + 2 > bytes.size()) return std::nullopt;
        std::string name(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                         bytes.begin() + static_cast<std::ptrdiff_t>(pos + name_length));
        pos += name_length;
        const std::uint16_t value_length = (static_cast<std::uint16_t>(bytes[pos]) << 8) |
                                           bytes[pos + 1];
        pos += 2;
        if (value_length == 0xffff) {
            result.properties[name] = std::nullopt;
        } else {
            if (pos + value_length > bytes.size()) return std::nullopt;
            result.properties[name] = std::string(
                bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                bytes.begin() + static_cast<std::ptrdiff_t>(pos + value_length));
            pos += value_length;
        }
    }
    if (const auto it = result.properties.find("_replyaddr");
        it != result.properties.end() && it->second) result.source_address = *it->second;
    if (const auto it = result.properties.find("_replyport");
        it != result.properties.end() && it->second)
        result.source_port = static_cast<std::uint16_t>(std::stoi(*it->second));
    return result;
}

std::optional<SoodMessage> SoodDiscovery::discover(std::chrono::milliseconds timeout) const {
    SocketGuard socket{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (socket.fd < 0) throw std::runtime_error("unable to create SOOD UDP socket");
    int enabled = 1;
    ::setsockopt(socket.fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket.fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
        throw std::runtime_error("unable to bind SOOD UDP socket");

    const auto query = encode_sood_query({
        {"_tid", transaction_id()}, {"query_service_id", core_service_id}});
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    ::inet_pton(AF_INET, multicast_address, &destination.sin_addr);

#ifdef ROON_DISPLAY_TIZEN
    // The Tizen socket extension does not expose getifaddrs(). Its default
    // route selects the active LAN interface for this multicast query.
    ::sendto(socket.fd, query.data(), query.size(), 0,
             reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
#else
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) throw std::runtime_error("unable to enumerate interfaces");
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        ::setsockopt(socket.fd, IPPROTO_IP, IP_MULTICAST_IF, &address->sin_addr,
                     sizeof(address->sin_addr));
        ::sendto(socket.fd, query.data(), query.size(), 0,
                 reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        if (item->ifa_broadaddr && (item->ifa_flags & IFF_BROADCAST)) {
            auto broadcast = *reinterpret_cast<sockaddr_in*>(item->ifa_broadaddr);
            broadcast.sin_port = htons(port);
            ::sendto(socket.fd, query.data(), query.size(), 0,
                     reinterpret_cast<sockaddr*>(&broadcast), sizeof(broadcast));
        }
    }
    ::freeifaddrs(interfaces);
#endif

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        timeval tv{static_cast<long>(remaining.count() / 1000000),
                   static_cast<long>(remaining.count() % 1000000)};
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket.fd, &read_set);
        const int ready = ::select(socket.fd + 1, &read_set, nullptr, nullptr, &tv);
        if (ready <= 0) break;
        std::array<std::uint8_t, 65535> buffer{};
        sockaddr_in sender{};
        socklen_t sender_length = sizeof(sender);
        const auto size = ::recvfrom(socket.fd, buffer.data(), buffer.size(), 0,
                                     reinterpret_cast<sockaddr*>(&sender), &sender_length);
        if (size <= 0) continue;
        std::array<char, INET_ADDRSTRLEN> ip{};
        ::inet_ntop(AF_INET, &sender.sin_addr, ip.data(), ip.size());
        auto message = parse_sood(std::span(buffer.data(), static_cast<std::size_t>(size)),
                                  ip.data(), ntohs(sender.sin_port));
        if (!message) continue;
        const auto service = message->properties.find("service_id");
        const auto port_value = message->properties.find("http_port");
        if (service != message->properties.end() && service->second == core_service_id &&
            port_value != message->properties.end() && port_value->second) return message;
    }
    return std::nullopt;
}

} // namespace roon_display
