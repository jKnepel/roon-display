#include "roon_display/display_state.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::string read_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open fixture: " + path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void print_state(const roon_display::DisplayState& state) {
    std::cout << "State changed: " << state.zones.size() << " zones\n";
    for (const auto& [id, zone] : state.zones) {
        std::cout << "  " << zone.display_name << " [" << id << "] "
                  << roon_display::to_string(zone.state);
        if (zone.now_playing) {
            const auto& now = *zone.now_playing;
            if (!now.two_line.line1.empty()) std::cout << " — " << now.two_line.line1;
            if (now.two_line.line2) std::cout << " / " << *now.two_line.line2;
        }
        std::cout << '\n';
    }
}
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "fixtures";
    try {
        roon_display::DisplayStateStore store;
        store.set_change_callback(print_state);
        store.apply_transport("Subscribed", read_file(root + "/transport/subscribed.json"));
        store.set_active_zone("living-room-zone");
        store.apply_settings(read_file(root + "/display/settings.json"));
        store.apply_transport("Changed", read_file(root + "/transport/changed.json"));
    } catch (const std::exception& error) {
        std::cerr << "state-replay: " << error.what() << '\n';
        return 1;
    }
}
