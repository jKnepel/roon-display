#include "roon_display/roon_client.hpp"

#include "roon_display/sood.hpp"

#include <unistd.h>

#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace roon_display {

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
                 "\",\"how\":\"absolute\",\"seconds\":" + std::to_string(position) + "}");
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
    const auto zone = state_.snapshot().zones.find(*active_zone_id_);
    if (zone == state_.snapshot().zones.end() || !zone->second.now_playing || !zone->second.now_playing->image_key) return;
    const auto& key = *zone->second.now_playing->image_key;
    if (requested_artwork_key_ == key) return;
    requested_artwork_key_ = key;
    for (const auto& request : artwork_requests_) if (request.second == key) return;
    const auto id = send_request("com.roonlabs.image:1/get_image",
        "{\"image_key\":\"" + json_escape(key) + "\",\"scale\":\"fit\",\"width\":1200,\"height\":1200,\"format\":\"image/jpeg\"}");
    artwork_requests_[std::to_string(id)] = key;
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
               message.request_id == transport_subscription_id_) {
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

void RoonClient::run() {
    std::cout << "Roon Display\n------------\nAdvertising:\n  Name: " << options_.display_name
              << "\n  Key: " << options_.display_key << "\n\nWaiting for Roon...\n";
    SoodDiscovery discovery;
    std::optional<SoodMessage> found;
    while (!found) found = discovery.discover(std::chrono::seconds(5));
    const auto port = static_cast<std::uint16_t>(
        std::stoi(*found->properties.at("http_port")));
    std::cout << "Roon Core discovered at " << found->source_address << ':' << port << '\n';
    websocket_.connect(found->source_address, port);
    send_request("com.roonlabs.registry:1/info");

    for (;;) {
        const auto bytes = websocket_.receive_message();
        std::string error;
        const auto message = parse_moo(bytes, &error);
        if (!message) throw std::runtime_error("invalid MOO message: " + error);
        const std::string body_preview = message->body.empty() ? "" : (message->content_type == "application/json" ? " " + message->body_text() : " <binary " + std::to_string(message->body.size()) + " bytes>");
        if (options_.trace)
            std::cout << "<- " << message->verb << ' ' << message->request_id << ' '
                      << (message->verb == "REQUEST" ? message->service + "/" : "")
                      << message->name
                      << body_preview << '\n';
        if (message->verb == "REQUEST") handle_request(*message);
        else handle_response(*message);
    }
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

