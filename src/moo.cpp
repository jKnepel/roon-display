#include "roon_display/moo.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace roon_display {
namespace {

std::vector<std::uint8_t> make_message(const std::string& first_line,
                                       const std::string& request_id,
                                       const std::string& json_body) {
    std::ostringstream out;
    out << "MOO/1 " << first_line << '\n'
        << "Request-Id: " << request_id << '\n';
    if (!json_body.empty()) {
        out << "Content-Length: " << json_body.size() << '\n'
            << "Content-Type: application/json\n";
    }
    out << '\n';
    auto header = out.str();
    std::vector<std::uint8_t> result(header.begin(), header.end());
    result.insert(result.end(), json_body.begin(), json_body.end());
    return result;
}

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

} // namespace

std::string MooMessage::body_text() const {
    return {body.begin(), body.end()};
}

std::vector<std::uint8_t> make_moo_request(std::uint64_t request_id,
                                           const std::string& endpoint,
                                           const std::string& json_body) {
    return make_message("REQUEST " + endpoint, std::to_string(request_id), json_body);
}

std::vector<std::uint8_t> make_moo_response(const std::string& verb,
                                            const std::string& name,
                                            const std::string& request_id,
                                            const std::string& json_body) {
    return make_message(verb + " " + name, request_id, json_body);
}

std::optional<MooMessage> parse_moo(std::span<const std::uint8_t> bytes,
                                    std::string* error) {
    const std::string text(bytes.begin(), bytes.end());
    const auto header_end = text.find("\n\n");
    if (header_end == std::string::npos) {
        set_error(error, "missing blank line after MOO headers");
        return std::nullopt;
    }

    std::istringstream lines(text.substr(0, header_end));
    std::string line;
    std::getline(lines, line);
    constexpr std::string_view prefix{"MOO/1 "};
    if (!line.starts_with(prefix)) {
        set_error(error, "unsupported MOO start line");
        return std::nullopt;
    }

    MooMessage message;
    std::istringstream start(line.substr(prefix.size()));
    start >> message.verb;
    std::string target;
    std::getline(start >> std::ws, target);
    if (message.verb == "REQUEST") {
        const auto slash = target.find('/');
        if (slash == std::string::npos) {
            set_error(error, "request target has no service/method separator");
            return std::nullopt;
        }
        message.service = target.substr(0, slash);
        message.name = target.substr(slash + 1);
    } else {
        message.name = target;
    }

    std::size_t content_length = 0;
    bool has_content_length = false;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            set_error(error, "invalid MOO header");
            return std::nullopt;
        }
        auto key = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(' '));
        message.headers[key] = value;
        if (key == "Request-Id") message.request_id = value;
        if (key == "Content-Type") message.content_type = value;
        if (key == "Content-Length") {
            has_content_length = true;
            const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                                content_length);
            if (result.ec != std::errc{}) {
                set_error(error, "invalid Content-Length");
                return std::nullopt;
            }
        }
    }
    if (message.request_id.empty()) {
        set_error(error, "missing Request-Id");
        return std::nullopt;
    }
    const auto body_start = header_end + 2;
    const auto actual = bytes.size() - body_start;
    if ((has_content_length && actual != content_length) ||
        (!has_content_length && actual != 0)) {
        set_error(error, "body length does not match Content-Length");
        return std::nullopt;
    }
    if (has_content_length) {
        message.body.assign(bytes.begin() + static_cast<std::ptrdiff_t>(body_start), bytes.end());
    }
    return message;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
        case '\"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[c >> 4] << hex[c & 0xf];
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

std::optional<std::string> json_string_field(const std::string& json,
                                             const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    std::string result;
    for (++pos; pos < json.size(); ++pos) {
        if (json[pos] == '"') return result;
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            default: result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
    }
    return std::nullopt;
}

} // namespace roon_display

