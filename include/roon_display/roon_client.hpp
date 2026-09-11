#pragma once

#include "roon_display/moo.hpp"
#include "roon_display/websocket.hpp"
#include "roon_display/display_state.hpp"

#include <cstdint>
#include <utility>
#include <optional>
#include <string>
#include <vector>

namespace roon_display {

struct ClientOptions {
    std::string display_name{"C++ Roon Display"};
    std::string display_key;
    std::string extension_id{"com.roonlabs.display_zone"};
    bool trace{true};
};

class RoonClient {
public:
    explicit RoonClient(ClientOptions options);
    void run();
    const DisplayStateStore& state() const { return state_; }
    void set_state_callback(DisplayStateStore::ChangeCallback callback) { state_.set_change_callback(std::move(callback)); }

private:
    ClientOptions options_;
    WebSocket websocket_;
    std::uint64_t next_request_id_{0};
    std::string core_id_;
    std::string paired_core_id_;
    std::optional<std::string> active_zone_id_;
    std::vector<std::string> display_subscription_ids_;
    std::string transport_subscription_id_;
    DisplayStateStore state_;

    void send_request(const std::string& endpoint, const std::string& body = {});
    void send_response(const MooMessage& request, const std::string& verb,
                       const std::string& name, const std::string& body = {});
    void send_display_changed();
    void handle_request(const MooMessage& message);
    void handle_response(const MooMessage& message);
    std::string registration_json() const;
    std::string displays_json() const;
};

std::string stable_display_key();

} // namespace roon_display

