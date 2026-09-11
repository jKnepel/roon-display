#include "roon_display/display_state.hpp"

#include "roon_display/moo.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <sstream>
#include <variant>

namespace roon_display {
namespace {

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;
struct Json : std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> {
    using variant::variant;
    const JsonObject* object() const { return std::get_if<JsonObject>(this); }
    const JsonArray* array() const { return std::get_if<JsonArray>(this); }
    const std::string* string() const { return std::get_if<std::string>(this); }
    const double* number() const { return std::get_if<double>(this); }
    const bool* boolean() const { return std::get_if<bool>(this); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}
    Json parse() {
        skip();
        auto value = value_at();
        skip();
        if (position_ != input_.size()) throw std::runtime_error("trailing JSON data");
        return value;
    }

private:
    const std::string& input_;
    std::size_t position_{0};

    void skip() { while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_; }
    bool consume(char c) { skip(); if (position_ < input_.size() && input_[position_] == c) { ++position_; return true; } return false; }
    Json value_at() {
        skip();
        if (position_ >= input_.size()) throw std::runtime_error("unexpected end of JSON");
        switch (input_[position_]) {
        case '{': return object();
        case '[': return array();
        case '"': return string_value();
        case 't': if (input_.compare(position_, 4, "true") == 0) { position_ += 4; return true; } break;
        case 'f': if (input_.compare(position_, 5, "false") == 0) { position_ += 5; return false; } break;
        case 'n': if (input_.compare(position_, 4, "null") == 0) { position_ += 4; return nullptr; } break;
        default: {
            char* end = nullptr;
            const auto number = std::strtod(input_.c_str() + position_, &end);
            if (end != input_.c_str() + position_) { position_ = static_cast<std::size_t>(end - input_.c_str()); return number; }
        }}
        throw std::runtime_error("invalid JSON value");
    }
    Json object() {
        if (!consume('{')) throw std::runtime_error("expected object");
        JsonObject result;
        if (consume('}')) return result;
        for (;;) {
            auto key = string_value();
            if (!consume(':')) throw std::runtime_error("expected object colon");
            result.emplace(*key.string(), value_at());
            if (consume('}')) return result;
            if (!consume(',')) throw std::runtime_error("expected object comma");
        }
    }
    Json array() {
        if (!consume('[')) throw std::runtime_error("expected array");
        JsonArray result;
        if (consume(']')) return result;
        for (;;) {
            result.push_back(value_at());
            if (consume(']')) return result;
            if (!consume(',')) throw std::runtime_error("expected array comma");
        }
    }
    Json string_value() {
        if (!consume('"')) throw std::runtime_error("expected string");
        std::string result;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return result;
            if (c != '\\') { result += c; continue; }
            if (position_ >= input_.size()) throw std::runtime_error("invalid string escape");
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': result += '"'; break; case '\\': result += '\\'; break;
            case '/': result += '/'; break; case 'b': result += '\b'; break;
            case 'f': result += '\f'; break; case 'n': result += '\n'; break;
            case 'r': result += '\r'; break; case 't': result += '\t'; break;
            default: result += escaped; break;
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }
};

const Json* member(const Json& value, const char* key) {
    const auto* object = value.object();
    if (!object) return nullptr;
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
}
std::string string_member(const Json& value, const char* key) {
    const auto* item = member(value, key);
    return item && item->string() ? *item->string() : std::string{};
}
std::optional<double> number_member(const Json& value, const char* key) {
    const auto* item = member(value, key);
    if (!item || !item->number()) return std::nullopt;
    return *item->number();
}
std::optional<bool> bool_member(const Json& value, const char* key) {
    const auto* item = member(value, key);
    if (!item || !item->boolean()) return std::nullopt;
    return *item->boolean();
}
std::optional<std::string> optional_string_member(const Json& value, const char* key) {
    const auto* item = member(value, key);
    if (!item || !item->string()) return std::nullopt;
    return *item->string();
}
TextLayout layout_member(const Json& value, const char* key) {
    const auto* item = member(value, key);
    if (!item || !item->object()) return {};
    return {string_member(*item, "line1"), optional_string_member(*item, "line2"), optional_string_member(*item, "line3")};
}
PlaybackState playback_state(const std::string& value) {
    if (value == "playing") return PlaybackState::playing;
    if (value == "paused") return PlaybackState::paused;
    if (value == "loading") return PlaybackState::loading;
    if (value == "stopped") return PlaybackState::stopped;
    return PlaybackState::unknown;
}
ZoneState zone(const Json& value) {
    ZoneState result;
    result.zone_id = string_member(value, "zone_id");
    result.display_name = string_member(value, "display_name");
    result.state = playback_state(string_member(value, "state"));
    result.seek_position = number_member(value, "seek_position");
    result.queue_time_remaining = number_member(value, "queue_time_remaining");
    result.play_allowed = bool_member(value, "is_play_allowed");
    result.pause_allowed = bool_member(value, "is_pause_allowed");
    result.previous_allowed = bool_member(value, "is_previous_allowed");
    result.next_allowed = bool_member(value, "is_next_allowed");
    result.seek_allowed = bool_member(value, "is_seek_allowed");
    if (const auto* playing = member(value, "now_playing"); playing && playing->object()) {
        NowPlayingState now;
        now.seek_position = number_member(*playing, "seek_position");
        now.length = number_member(*playing, "length");
        now.image_key = optional_string_member(*playing, "image_key");
        now.one_line = layout_member(*playing, "one_line");
        now.two_line = layout_member(*playing, "two_line");
        now.three_line = layout_member(*playing, "three_line");
        if (const auto* artists = member(*playing, "artist_image_keys"); artists && artists->array())
            for (const auto& artist : *artists->array()) if (artist.string()) now.artist_image_keys.push_back(*artist.string());
        result.now_playing = std::move(now);
    }
    return result;
}

} // namespace

const char* to_string(PlaybackState state) {
    switch (state) { case PlaybackState::playing: return "playing"; case PlaybackState::paused: return "paused"; case PlaybackState::loading: return "loading"; case PlaybackState::stopped: return "stopped"; default: return "unknown"; }
}

std::string to_json(const DisplayState& state) {
    std::ostringstream out;
    out << "{\"activeZone\":";
    if (!state.active_zone_id) out << "null";
    else {
        const auto it = state.zones.find(*state.active_zone_id);
        if (it == state.zones.end()) out << "null";
        else {
            const auto& zone = it->second;
            out << "{\"displayName\":\"" << json_escape(zone.display_name) << "\",\"state\":\"" << to_string(zone.state) << "\"";
            if (zone.play_allowed) out << ",\"playAllowed\":" << (*zone.play_allowed ? "true" : "false");
            if (zone.pause_allowed) out << ",\"pauseAllowed\":" << (*zone.pause_allowed ? "true" : "false");
            if (zone.previous_allowed) out << ",\"previousAllowed\":" << (*zone.previous_allowed ? "true" : "false");
            if (zone.next_allowed) out << ",\"nextAllowed\":" << (*zone.next_allowed ? "true" : "false");
            if (zone.seek_allowed) out << ",\"seekAllowed\":" << (*zone.seek_allowed ? "true" : "false");
            if (zone.seek_position) out << ",\"seekPosition\":" << *zone.seek_position;
            if (zone.now_playing) {
                const auto& now = *zone.now_playing;
                out << ",\"nowPlaying\":{";
                if (now.seek_position) out << "\"seekPosition\":" << *now.seek_position << ",";
                if (now.length) out << "\"length\":" << *now.length << ",";
                if (now.image_key) {
                    out << "\"imageKey\":\"" << json_escape(*now.image_key) << "\",\"artworkUrl\":\"/artwork/" << json_escape(*now.image_key) << "\",";
                }
                out << "\"twoLine\":{";
                out << "\"line1\":\"" << json_escape(now.two_line.line1) << "\"";
                if (now.two_line.line2) out << ",\"line2\":\"" << json_escape(*now.two_line.line2) << "\"";
                out << "},\"threeLine\":{";
                out << "\"line1\":\"" << json_escape(now.three_line.line1) << "\"";
                if (now.three_line.line2) out << ",\"line2\":\"" << json_escape(*now.three_line.line2) << "\"";
                if (now.three_line.line3) out << ",\"line3\":\"" << json_escape(*now.three_line.line3) << "\"";
                out << "}}";
            }
            out << "}";
        }
    }
    out << "}";
    return out.str();
}

void DisplayStateStore::changed() { if (callback_) callback_(state_); }

void DisplayStateStore::set_active_zone(std::optional<std::string> zone_id) {
    state_.active_zone_id = std::move(zone_id);
    changed();
}

void DisplayStateStore::apply_settings(const std::string& json) {
    try {
        const auto root = JsonParser(json).parse();
        DisplaySettings settings;
        const Json* values = member(root, "settings");
        if (!values) values = &root;
        if (const auto* lyrics = member(*values, "enable_lyrics"); lyrics && lyrics->boolean()) settings.enable_lyrics = *lyrics->boolean();
        state_.settings = settings;
        changed();
    } catch (const std::exception&) { }
}

void DisplayStateStore::apply_transport(const std::string& response, const std::string& json) {
    try {
        const auto root = JsonParser(json).parse();
        if (response == "Subscribed") {
            state_.zones.clear();
            if (const auto* zones = member(root, "zones"); zones && zones->array())
                for (const auto& item : *zones->array()) { auto parsed = zone(item); if (!parsed.zone_id.empty()) state_.zones[parsed.zone_id] = std::move(parsed); }
        } else if (response == "Changed") {
            if (const auto* removed = member(root, "zones_removed"); removed && removed->array())
                for (const auto& item : *removed->array()) state_.zones.erase(string_member(item, "zone_id"));
            for (const char* key : {"zones_added", "zones_changed"})
                if (const auto* list = member(root, key); list && list->array())
                    for (const auto& item : *list->array()) { auto parsed = zone(item); if (!parsed.zone_id.empty()) state_.zones[parsed.zone_id] = std::move(parsed); }
            if (const auto* seeks = member(root, "zones_seek_changed"); seeks && seeks->array())
                for (const auto& item : *seeks->array()) { auto it = state_.zones.find(string_member(item, "zone_id")); if (it != state_.zones.end()) { it->second.seek_position = number_member(item, "seek_position"); it->second.queue_time_remaining = number_member(item, "queue_time_remaining"); if (it->second.now_playing) it->second.now_playing->seek_position = it->second.seek_position; } }
        }
        changed();
    } catch (const std::exception&) { }
}

} // namespace roon_display

