#include "roon_display/roon_client.hpp"

#include <emscripten/emscripten.h>

#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<roon_display::RoonClient> client;

EM_JS(void, emit_state, (const char* json), {
    self.postMessage({ type: "state", state: JSON.parse(UTF8ToString(json)) });
});

EM_JS(void, emit_artwork, (const char* key, const char* mime,
                           const std::uint8_t* data, std::size_t size), {
    const bytes = HEAPU8.slice(data, data + size);
    self.postMessage({
      type: "artwork", key: UTF8ToString(key), mime: UTF8ToString(mime), bytes: bytes.buffer
    }, [bytes.buffer]);
});

EM_JS(void, emit_error, (const char* message), {
    self.postMessage({ type: "error", message: UTF8ToString(message) });
});

roon_display::ClientOptions options(int argc, char** argv) {
    roon_display::ClientOptions result;
    result.display_name = "Samsung TV";
    result.trace = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "--name" || argument == "--display-key") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (argument == "--name") result.display_name = value;
            else result.display_key = value;
        }
    }
    return result;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void roon_display_control(const char* command) {
    if (client && command) client->control(command);
}

EMSCRIPTEN_KEEPALIVE void roon_display_seek(double position) {
    if (client) client->seek(position);
}

EMSCRIPTEN_KEEPALIVE void roon_display_stop() {
    if (client) client->stop();
}

} // extern "C"

int main(int argc, char** argv) {
    try {
        client = std::make_unique<roon_display::RoonClient>(options(argc, argv));
        client->set_state_callback([](const roon_display::DisplayState& state) {
            const auto json = roon_display::to_json(state);
            emit_state(json.c_str());
        });
        client->set_artwork_callback([](const std::string& key, const std::string& mime,
                                        const std::vector<std::uint8_t>& bytes) {
            emit_artwork(key.c_str(), mime.empty() ? "image/jpeg" : mime.c_str(),
                         bytes.data(), bytes.size());
        });
        client->run();
    } catch (const std::exception& error) {
        emit_error(error.what());
        return 1;
    }
    return 0;
}
