#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace roon_display {

struct MooMessage {
    std::string verb;
    std::string service;
    std::string name;
    std::string request_id;
    std::map<std::string, std::string> headers;
    std::string content_type;
    std::vector<std::uint8_t> body;

    std::string body_text() const;
};

std::vector<std::uint8_t> make_moo_request(std::uint64_t request_id,
                                           const std::string& endpoint,
                                           const std::string& json_body = {});
std::vector<std::uint8_t> make_moo_response(const std::string& verb,
                                            const std::string& name,
                                            const std::string& request_id,
                                            const std::string& json_body = {});
std::optional<MooMessage> parse_moo(std::span<const std::uint8_t> bytes,
                                    std::string* error = nullptr);
std::string json_escape(const std::string& value);
std::optional<std::string> json_string_field(const std::string& json,
                                             const std::string& key);

} // namespace roon_display

