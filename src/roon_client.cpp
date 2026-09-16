#include "roon_display/roon_client.hpp"

#include "roon_display/sood.hpp"

#include <unistd.h>

#include <charconv>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace roon_display {

namespace {

std::string json_number(double value) {
    char buffer[64]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return result.ec == std::errc{} ? std::string(buffer, result.ptr) : "0";
}

std::string json_top_level_keys(const std::string& json) {
    std::ostringstream keys;
    bool first = true;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            std::size_t previous = i;
            while (previous > 0 && (json[previous - 1] == ' ' || json[previous - 1] == '\n' || json[previous - 1] == '\r' || json[previous - 1] == '\t')) --previous;
            if (depth == 1 && previous > 0 && (json[previous - 1] == '{' || json[previous - 1] == ',')) {
                const auto key_start = i + 1;
                std::size_t key_end = key_start;
                bool key_escaped = false;
                for (; key_end < json.size(); ++key_end) {
                    if (key_escaped) key_escaped = false;
                    else if (json[key_end] == '\\') key_escaped = true;
                    else if (json[key_end] == '"') break;
                }
                std::size_t after = key_end + 1;
                while (after < json.size() && (json[after] == ' ' || json[after] == '\n' || json[after] == '\r' || json[after] == '\t')) ++after;
                if (key_end < json.size() && after < json.size() && json[after] == ':') {
                    if (!first) keys << ',';
                    keys << json.substr(key_start, key_end - key_start);
                    first = false;
                }
            }
            in_string = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return keys.str();
}

std::string hex_prefix(const std::vector<std::uint8_t>& bytes, std::size_t count = 16) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < std::min(count, bytes.size()); ++i)
        out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    return out.str();
}

} // namespace

RoonClient::RoonClient(ClientOptions options) : options_(std::move(options)) {
    if (options_.display_key.empty()) options_.display_key = stable_display_key();
}

std::uint64_t RoonClient::send_request(const std::string& endpoint, const std::string& body) {
    std::lock_guard lock(send_mutex_);
    const auto id = next_request_id_++;
    if (options_.trace)
        std::cout << "-> REQUEST " << id << ' ' << endpoint
                  << (body.empty() ? "" : " " + body) << '\n';
    websocket_.send_binary(make_moo_request(id, endpoint, body));
    return id;
}
bool RoonClient::control(const std::string& command) {
    if (!active_zone_id_ || command.empty()) return false;
    send_request("com.roonlabs.transport:2/control",
                 "{\"zone_or_output_id\":\"" + json_escape(*active_zone_id_) +
                 "\",\"control\":\"" + json_escape(command) + "\"}");
    return true;
}

bool RoonClient::seek(double position) {
    if (!active_zone_id_ || position < 0) return false;
    send_request("com.roonlabs.transport:2/seek",
                 "{\"zone_or_output_id\":\"" + json_escape(*active_zone_id_) +
                 "\",\"how\":\"absolute\",\"seconds\":" + json_number(position) + "}");
    return true;
}

void RoonClient::send_response(const MooMessage& request, const std::string& verb,
                               const std::string& name, const std::string& body) {
    std::lock_guard lock(send_mutex_);
    if (options_.trace)
        std::cout << "-> " << verb << ' ' << request.request_id << ' ' << name
                  << (body.empty() ? "" : " " + body) << '\n';
    websocket_.send_binary(make_moo_response(verb, name, request.request_id, body));
}

std::string RoonClient::registration_json() const {
    return "{\"extension_id\":\"" + json_escape(options_.extension_id) +
        "\",\"display_name\":\"Roon API Display Zone\""
        ",\"display_version\":\"1.0.0\",\"publisher\":\"Roon Labs, LLC\""
        ",\"email\":\"contact@roonlabs.com\",\"required_services\":["
        "\"com.roonlabs.transport:2\",\"com.roonlabs.image:1\"],"
        "\"optional_services\":[],\"provided_services\":["
        "\"com.roonlabs.zonedisplay:1\",\"com.roonlabs.pairing:1\","
        "\"com.roonlabs.ping:1\"]}";
}

std::string RoonClient::displays_json() const {
    const std::string active_zone = active_zone_id_
        ? "\"" + json_escape(*active_zone_id_) + "\"" : "null";
    return "{\"displays\":[{\"display_key\":\"" + json_escape(options_.display_key) +
        "\",\"auto_name\":\"" + json_escape(options_.display_name) +
        "\",\"active_zone_id\":" + active_zone + "}]}";
}

void RoonClient::send_display_changed() {
    const auto displays = displays_json();
    const auto begin = displays.find("[");
    const auto end = displays.rfind("]");
    const std::string body = "{\"displays_changed\":" +
        displays.substr(begin, end - begin + 1) + "}";
    for (const auto& request_id : display_subscription_ids_) {
        std::lock_guard lock(send_mutex_);
        if (options_.trace)
            std::cout << "-> CONTINUE " << request_id << " Changed " << body << std::endl;
        websocket_.send_binary(make_moo_response("CONTINUE", "Changed", request_id, body));
    }
}

void RoonClient::request_active_artwork() {
    if (!active_zone_id_) return;
    const auto snapshot = state_.snapshot();
    const auto zone = snapshot.zones.find(*active_zone_id_);
    if (zone == snapshot.zones.end() || !zone->second.now_playing) return;

    const auto request_image = [this](const std::string& key) {
        for (const auto& request : artwork_requests_) if (request.second == key) return;
        const auto id = send_request("com.roonlabs.image:1/get_image",
            "{\"image_key\":\"" + json_escape(key) + "\",\"scale\":\"fit\",\"width\":4096,\"height\":4096,\"format\":\"image/jpeg\"}");
        artwork_requests_[std::to_string(id)] = key;
    };

    const auto& now = *zone->second.now_playing;
    if (now.image_key && requested_artwork_key_ != *now.image_key) {
        requested_artwork_key_ = *now.image_key;
        request_image(*now.image_key);
    }
    for (const auto& key : now.artist_image_keys) {
        if (requested_artist_artwork_keys_.insert(key).second) request_image(key);
    }
}

void RoonClient::handle_response(const MooMessage& message) {
    const auto body = message.body_text();
    if (message.name == "Success" && core_id_.empty()) {
        if (const auto id = json_string_field(body, "core_id")) core_id_ = *id;
        std::cout << "Roon Core connected";
        if (const auto name = json_string_field(body, "display_name")) std::cout << ": " << *name;
        std::cout << '\n';
        send_request("com.roonlabs.registry:1/register", registration_json());
    } else if (message.name == "Registered") {
        if (const auto id = json_string_field(body, "core_id")) core_id_ = *id;
        paired_core_id_ = core_id_;
        std::cout << "Display service registered; waiting for subscribe_displays\n";
        transport_subscription_id_ = std::to_string(next_request_id_);
        send_request("com.roonlabs.transport:2/subscribe_zones",
                     "{\"subscription_key\":0}");
    } else if (!transport_subscription_id_.empty() &&
               message.request_id == transport_subscription_id_ &&
               // Roon uses independent request-id sequences for requests in
               // each direction. A pairing request can therefore reuse the
               // id of our transport subscription. Its Subscribed response
               // contains paired_core_id, not zones, and must not replace the
               // transport state.
               !(message.name == "Subscribed" && body.find("\"zones\"") == std::string::npos)) {
        state_.apply_transport(message.name, body);
        if (message.name == "Subscribed")
            std::cout << "Transport zone subscription active\n";
        request_active_artwork();
    } else {
        const auto artwork = artwork_requests_.find(message.request_id);
        if (artwork != artwork_requests_.end()) {
            if (message.name == "Success" && artwork_callback_)
                artwork_callback_(artwork->second, message.content_type, message.body);
            artwork_requests_.erase(artwork);
        }
    }
}

void RoonClient::handle_request(const MooMessage& request) {
    const auto endpoint = request.service + "/" + request.name;
    if (endpoint == "com.roonlabs.ping:1/ping") {
        send_response(request, "COMPLETE", "Success");
    } else if (endpoint == "com.roonlabs.zonedisplay:1/subscribe_displays") {
        display_subscription_ids_.push_back(request.request_id);
        send_response(request, "CONTINUE", "Subscribed", displays_json());
        std::cout << "Display announced to Roon: " << options_.display_name
                  << " (" << options_.display_key << ")" << std::endl;
    } else if (endpoint == "com.roonlabs.zonedisplay:1/unsubscribe_displays") {
        display_subscription_ids_.clear();
        send_response(request, "COMPLETE", "Unsubscribed");
    } else if (endpoint == "com.roonlabs.zonedisplay:1/get_displays") {
        send_response(request, "COMPLETE", "Success", displays_json());
    } else if (endpoint == "com.roonlabs.zonedisplay:1/activate") {
        const auto key = json_string_field(request.body_text(), "display_key");
        const auto zone = json_string_field(request.body_text(), "zone_id");
        if (!key || *key != options_.display_key) {
            send_response(request, "COMPLETE", "InvalidKey");
        } else if (!zone) {
            send_response(request, "COMPLETE", "InvalidRequest",
                          "{\"error\":\"missing zone_id\"}");
        } else {
            active_zone_id_ = *zone;
            state_.set_active_zone(active_zone_id_);
            request_active_artwork();
            std::cout << "Zone assigned: " << *zone << std::endl;
            send_response(request, "COMPLETE", "Success");
            send_display_changed();
        }
    } else if (endpoint == "com.roonlabs.zonedisplay:1/deactivate") {
        const auto key = json_string_field(request.body_text(), "display_key");
        if (!key || *key != options_.display_key) {
            send_response(request, "COMPLETE", "InvalidKey");
        } else {
            active_zone_id_.reset();
            state_.set_active_zone(std::nullopt);
            std::cout << "Zone assignment cleared" << std::endl;
            send_response(request, "COMPLETE", "Success");
            send_display_changed();
        }
    } else if (endpoint == "com.roonlabs.zonedisplay:1/update_settings") {
        const auto key = json_string_field(request.body_text(), "display_key");
        if (!key || *key != options_.display_key) {
            send_response(request, "COMPLETE", "InvalidKey");
        } else {
            std::cout << "Display settings: " << request.body_text() << std::endl;
            send_response(request, "COMPLETE", "Success");
        }
    } else if (endpoint == "com.roonlabs.pairing:1/subscribe_pairing") {
        const std::string paired = paired_core_id_.empty()
            ? "{\"paired_core_id\":null}"
            : "{\"paired_core_id\":\"" + json_escape(paired_core_id_) + "\"}";
        send_response(request, "CONTINUE", "Subscribed", paired);
    } else if (endpoint == "com.roonlabs.pairing:1/get_pairing") {
        const std::string paired = paired_core_id_.empty()
            ? "{\"paired_core_id\":null}"
            : "{\"paired_core_id\":\"" + json_escape(paired_core_id_) + "\"}";
        send_response(request, "COMPLETE", "Success", paired);
    } else if (endpoint == "com.roonlabs.pairing:1/pair") {
        paired_core_id_ = core_id_;
        send_response(request, "COMPLETE", "Success");
    } else if (endpoint == "com.roonlabs.pairing:1/unsubscribe_pairing") {
        send_response(request, "COMPLETE", "Unsubscribed");
    } else {
        send_response(request, "COMPLETE", "InvalidRequest",
                      "{\"error\":\"unknown request: " + json_escape(endpoint) + "\"}");
    }
}

void RoonClient::stop() {
    stop_requested_.store(true);
    websocket_.close();
    stop_condition_.notify_all();
}

void RoonClient::run() {
    std::cout << "Roon Display\n------------\nAdvertising:\n  Name: " << options_.display_name << "\n  Key: " << options_.display_key << "\n\nWaiting for Roon...\n";
    while (!stop_requested_.load()) {
        try {
            SoodDiscovery discovery;
            std::optional<SoodMessage> found;
            while (!stop_requested_.load() && !found) found = discovery.discover(std::chrono::seconds(5));
            if (stop_requested_.load()) break;
            const auto port = static_cast<std::uint16_t>(std::stoi(*found->properties.at("http_port")));
            std::cout << "Roon Core discovered at " << found->source_address << ":" << port << std::endl;
            websocket_.connect(found->source_address, port);
            send_request("com.roonlabs.registry:1/info");
            while (!stop_requested_.load()) {
                const auto bytes = websocket_.receive_message();
                std::string error;
                const auto message = parse_moo(bytes, &error);
                if (!message) throw std::runtime_error("invalid MOO message: " + error);
                const std::string protocol_body = message->content_type == "application/json" ? message->body_text() : std::string{};
                const std::string body_preview = message->body.empty() ? "" : (message->content_type == "application/json" ? " " + protocol_body : " <binary " + std::to_string(message->body.size()) + " bytes>");
                std::cerr << "[protocol] direction=in verb=" << message->verb
                          << " service=" << message->service
                          << " request_id=" << message->request_id
                          << " name=" << message->name
                          << " content_type=" << (message->content_type.empty() ? "none" : message->content_type)
                          << " body_bytes=" << message->body.size();
                if (!protocol_body.empty())
                    std::cerr << " json_keys=" << json_top_level_keys(protocol_body);
                else if (!message->body.empty())
                    std::cerr << " hex_prefix=" << hex_prefix(message->body);
                std::cerr << '\n';
                const bool contains_waveform = protocol_body.find("\"waveform\"") != std::string::npos ||
                                               protocol_body.find("\"waveform_key\"") != std::string::npos;
                const bool contains_now_playing = protocol_body.find("\"now_playing\"") != std::string::npos ||
                                                  protocol_body.find("\"nowPlaying\"") != std::string::npos;
                const bool is_waveform_message = message->name.find("waveform") != std::string::npos ||
                                                 message->name.find("Waveform") != std::string::npos;
                const bool is_track_change = message->name == "Changed" && contains_now_playing;
                if (is_waveform_message || contains_waveform || is_track_change) {
                    std::cerr << "[protocol-waveform] candidate="
                              << ((is_waveform_message || contains_waveform) ? "waveform" : "track-change")
                              << " verb=" << message->verb
                              << " service=" << message->service
                              << " request_id=" << message->request_id
                              << " name=" << message->name
                              << " waveform=" << (contains_waveform ? "yes" : "no")
                              << " now_playing=" << (contains_now_playing ? "yes" : "no")
                              << " body_bytes=" << protocol_body.size();
                    if (!protocol_body.empty())
                        std::cerr << " body_prefix=" << protocol_body.substr(0, 240);
                    std::cerr << '\n';
                }
                if (options_.trace)
                    std::cout << "<- " << message->verb << " " << message->request_id << " "
                              << (message->verb == "REQUEST" ? message->service + "/" : "")
                              << message->name << body_preview << std::endl;
                if (message->verb == "REQUEST") handle_request(*message); else handle_response(*message);
            }
        } catch (const std::exception& error) {
            websocket_.close();
            if (stop_requested_.load()) break;
            std::cerr << "Roon connection lost: " << error.what() << ", retrying" << std::endl;
            core_id_.clear(); paired_core_id_.clear(); display_subscription_ids_.clear(); transport_subscription_id_.clear();
            artwork_requests_.clear(); requested_artwork_key_.clear(); requested_artist_artwork_keys_.clear();
            std::unique_lock lock(stop_mutex_);
            stop_condition_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_requested_.load(); });
        }
    }
    websocket_.close();
}

std::string stable_display_key() {
    std::array<char, 256> hostname{};
    if (::gethostname(hostname.data(), hostname.size()) != 0) return "roon-display-local";
    const auto hash = std::hash<std::string>{}(hostname.data());
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<std::uint32_t>(hash >> 32) << '-'
        << std::setw(4) << static_cast<std::uint16_t>(hash >> 16) << '-'
        << "4" << std::setw(3) << static_cast<std::uint16_t>(hash & 0x0fff) << '-'
        << "8" << std::setw(3) << static_cast<std::uint16_t>((hash >> 20) & 0x0fff) << '-'
        << std::setw(12) << static_cast<std::uint64_t>(hash & 0xffffffffffffULL);
    return out.str();
}

} // namespace roon_display
