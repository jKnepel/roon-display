#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace roon_display {

struct SoodMessage {
    char type{};
    std::string source_address;
    std::uint16_t source_port{};
    std::map<std::string, std::optional<std::string>> properties;
};

std::vector<std::uint8_t> encode_sood_query(
    const std::map<std::string, std::optional<std::string>>& properties);
std::optional<SoodMessage> parse_sood(std::span<const std::uint8_t> bytes,
                                      std::string source_address = {},
                                      std::uint16_t source_port = 0);

class SoodDiscovery {
public:
    static constexpr std::uint16_t port = 9003;
    static constexpr const char* multicast_address = "239.255.90.90";
    static constexpr const char* core_service_id =
        "00720724-5143-4a9b-abac-0e50cba674bb";

    std::optional<SoodMessage> discover(std::chrono::milliseconds timeout) const;
};

} // namespace roon_display

