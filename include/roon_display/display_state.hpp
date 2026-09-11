#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace roon_display {

enum class PlaybackState { playing, paused, loading, stopped, unknown };

struct TextLayout {
    std::string line1;
    std::optional<std::string> line2;
    std::optional<std::string> line3;
};

struct NowPlayingState {
    std::optional<double> seek_position;
    std::optional<double> length;
    std::optional<std::string> image_key;
    std::vector<std::string> artist_image_keys;
    TextLayout one_line;
    TextLayout two_line;
    TextLayout three_line;
};

struct ZoneState {
    std::string zone_id;
    std::string display_name;
    PlaybackState state{PlaybackState::unknown};
    std::optional<double> seek_position;
    std::optional<double> queue_time_remaining;
    std::optional<bool> play_allowed;
    std::optional<bool> pause_allowed;
    std::optional<bool> previous_allowed;
    std::optional<bool> next_allowed;
    std::optional<bool> seek_allowed;
    std::optional<NowPlayingState> now_playing;
};

struct DisplaySettings {
    std::optional<bool> enable_lyrics;
};

struct DisplayState {
    std::map<std::string, ZoneState> zones;
    std::optional<std::string> active_zone_id;
    std::optional<DisplaySettings> settings;
};

class DisplayStateStore {
public:
    using ChangeCallback = std::function<void(const DisplayState&)>;

    const DisplayState& snapshot() const { return state_; }
    void set_change_callback(ChangeCallback callback) { callback_ = std::move(callback); }
    void set_active_zone(std::optional<std::string> zone_id);
    void apply_settings(const std::string& json);
    void apply_transport(const std::string& response, const std::string& json);

private:
    DisplayState state_;
    ChangeCallback callback_;

    void changed();
};

const char* to_string(PlaybackState state);
std::string to_json(const DisplayState& state);

} // namespace roon_display

