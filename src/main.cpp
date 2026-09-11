#include "roon_display/roon_client.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void usage(const char* executable) {
    std::cout << "Usage: " << executable
              << " [--name DISPLAY_NAME] [--display-key UUID] [--quiet]\n";
}

} // namespace

int main(int argc, char** argv) {
    roon_display::ClientOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "--name" || argument == "--display-key") && i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (argument == "--name") options.display_name = argv[++i];
        else if (argument == "--display-key") options.display_key = argv[++i];
        else if (argument == "--quiet") options.trace = false;
        else if (argument == "--help" || argument == "-h") { usage(argv[0]); return 0; }
        else { std::cerr << "Unknown argument: " << argument << '\n'; usage(argv[0]); return 2; }
    }
    try {
        roon_display::RoonClient client(std::move(options));
        client.set_state_callback([](const roon_display::DisplayState& state) {
            if (!state.active_zone_id) return;
            const auto it = state.zones.find(*state.active_zone_id);
            if (it == state.zones.end()) return;
            const auto& zone = it->second;
            std::cout << "\nNow Playing [" << zone.display_name << "] ("
                      << roon_display::to_string(zone.state) << ")\n";
            if (!zone.now_playing) return;
            const auto& now = *zone.now_playing;
            if (!now.two_line.line1.empty()) std::cout << "  " << now.two_line.line1 << '\n';
            if (now.two_line.line2) std::cout << "  " << *now.two_line.line2 << '\n';
            if (now.seek_position && now.length)
                std::cout << "  Position: " << std::fixed << std::setprecision(1)
                          << *now.seek_position << " / " << *now.length << " s\n";
        });
        client.run();
    } catch (const std::exception& error) {
        std::cerr << "roon-display: " << error.what() << '\n';
        return 1;
    }
}

