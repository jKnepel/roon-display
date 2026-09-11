#include "roon_display/moo.hpp"
#include "roon_display/roon_client.hpp"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <cstring>
#include <charconv>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <iostream>

namespace {

struct DesktopResources {
    std::string root;
    std::mutex mutex;
    std::map<std::string, std::pair<std::string, std::vector<std::uint8_t>>> artwork;
    std::map<std::string, std::size_t> artwork_versions;
    std::deque<std::string> artwork_order;
    std::size_t artwork_bytes{0};
    static constexpr std::size_t max_artwork_bytes{64 * 1024 * 1024};
    static constexpr std::size_t max_artwork_entries{128};
};

std::string uri_path(const std::string& uri) {
    const auto path_start = uri.find('/', uri.find("://") + 3);
    if (path_start == std::string::npos) return "/";
    auto path = uri.substr(path_start);
    if (const auto query = path.find('?'); query != std::string::npos) path.resize(query);
    return path;
}

const char* mime_type(const std::string& path) {
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".svg")) return "image/svg+xml";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".webp")) return "image/webp";
    return "application/octet-stream";
}

void finish_bytes(WebKitURISchemeRequest* request, const std::vector<std::uint8_t>& bytes, const char* mime) {
    auto* copy = static_cast<std::uint8_t*>(g_malloc(bytes.size()));
    if (!bytes.empty()) std::memcpy(copy, bytes.data(), bytes.size());
    auto* stream = g_memory_input_stream_new_from_data(copy, bytes.size(), g_free);
    webkit_uri_scheme_request_finish(request, G_INPUT_STREAM(stream), bytes.size(), mime);
    g_object_unref(stream);
}

void finish_text(WebKitURISchemeRequest* request, const std::string& text, const char* mime) {
    finish_bytes(request, std::vector<std::uint8_t>(text.begin(), text.end()), mime);
}

void handle_uri(WebKitURISchemeRequest* request, gpointer user_data) {
    auto& resources = *static_cast<DesktopResources*>(user_data);
    const auto path = uri_path(webkit_uri_scheme_request_get_uri(request));
    if (path.find("..") != std::string::npos) {
        webkit_uri_scheme_request_finish_error(request, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "invalid resource path"));
        return;
    }
    if (path.starts_with("/artwork/")) {
        const auto key = path.substr(9);
        std::lock_guard lock(resources.mutex);
        const auto found = resources.artwork.find(key);
        if (found == resources.artwork.end()) {
            webkit_uri_scheme_request_finish_error(request, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "artwork not found"));
        } else {
            finish_bytes(request, found->second.second, found->second.first.c_str());
        }
        return;
    }
    auto relative = path;
    if (relative == "/" || relative == "/ui" || relative == "/ui/") relative = "/ui/index.html";
    std::ifstream input(resources.root + relative, std::ios::binary);
    if (!input) {
        webkit_uri_scheme_request_finish_error(request, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "resource not found"));
        return;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    finish_text(request, contents.str(), mime_type(relative));
}

struct DesktopApp {
    DesktopResources resources;
    roon_display::RoonClient client;
    WebKitWebView* view{nullptr};
    std::mutex state_mutex;
    std::string state_json{"{\"activeZone\":null}"};

    explicit DesktopApp(std::string root)
        : resources{std::move(root)}, client({"Roon Display", {}, "com.roonlabs.display_zone", true}) {}

    void publish_state(const roon_display::DisplayState& state) {
        auto json = roon_display::to_json(state);
        std::lock_guard lock(resources.mutex);
        for (const auto& [key, version] : resources.artwork_versions) {
            const std::string old_url = "\"/artwork/" + key + "\"";
            const std::string new_url = "\"roon-display://app/artwork/" + key + "?v=" + std::to_string(version) + "\"";
            if (const auto position = json.find(old_url); position != std::string::npos)
                json.replace(position, old_url.size(), new_url);
        }
        { std::lock_guard state_lock(state_mutex); state_json = std::move(json); }
        push_state();
    }

    void push_state() {
        std::string json;
        { std::lock_guard lock(state_mutex); json = state_json; }
        auto* view = this->view;
        if (!view) return;
        auto* message = new std::pair<WebKitWebView*, std::string>{view, std::move(json)};
        g_idle_add([](gpointer data) -> gboolean {
            auto* message = static_cast<std::pair<WebKitWebView*, std::string>*>(data);
            // Call the shared UI contract directly. The native host callback is
            // also kept available for host-specific integrations, but state
            // delivery must not depend on that optional symbol being present.
            const auto script = "if (window.RoonDisplay) window.RoonDisplay.update(" + message->second + ");";
            webkit_web_view_run_javascript(message->first, script.c_str(), nullptr, nullptr, nullptr);
            delete message;
            return G_SOURCE_REMOVE;
        }, message);
    }

    void command(const std::string& body, const std::string& request_id) {
        const auto command = roon_display::json_string_field(body, "command");
        bool accepted = false;
        if (command && *command == "seek") {
            if (const auto marker = body.find("\"position\":"); marker != std::string::npos) {
                double position = 0;
                const auto value = body.data() + marker + 11;
                const auto parsed = std::from_chars(value, body.data() + body.size(), position);
                if (parsed.ec == std::errc{} && parsed.ptr != value)
                    accepted = client.seek(position);
            }
        } else if (command) accepted = client.control(*command);
        const auto script = "window.__roonDisplayControlResult && window.__roonDisplayControlResult(" + request_id + "," + (accepted ? "true" : "false") + ");";
        webkit_web_view_run_javascript(view, script.c_str(), nullptr, nullptr, nullptr);
    }
};

void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer data) {
    auto& app = *static_cast<DesktopApp*>(data);
    gchar* text = jsc_value_to_string(webkit_javascript_result_get_js_value(result));
    if (!text) return;
    const std::string body(text);
    g_free(text);
    const auto type = roon_display::json_string_field(body, "type");
    if (type && *type == "ready") {
        app.push_state();
    } else if (type && *type == "control") {
        const auto id = roon_display::json_string_field(body, "id");
        const auto request = roon_display::json_string_field(body, "request");
        if (id && request) app.command(*request, *id);
    }
}

void on_load_changed(WebKitWebView*, WebKitLoadEvent event, gpointer data) {
    if (event == WEBKIT_LOAD_FINISHED)
        static_cast<DesktopApp*>(data)->push_state();
}

} // namespace

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    const std::string root = argc > 1 ? argv[1] : "/app/share/roon-display";
    DesktopApp app(root);
    app.client.set_state_callback([&app](const roon_display::DisplayState& state) { app.publish_state(state); });
    app.client.set_artwork_callback([&app](const std::string& key, const std::string& mime, const std::vector<std::uint8_t>& bytes) {
        { std::lock_guard lock(app.resources.mutex); const auto old = app.resources.artwork.find(key); if (old != app.resources.artwork.end()) app.resources.artwork_bytes -= old->second.second.size(); app.resources.artwork[key] = {mime.empty() ? "image/jpeg" : mime, bytes}; app.resources.artwork_bytes += bytes.size(); app.resources.artwork_order.push_back(key); ++app.resources.artwork_versions[key]; while ((app.resources.artwork_bytes > app.resources.max_artwork_bytes || app.resources.artwork.size() > app.resources.max_artwork_entries) && !app.resources.artwork_order.empty()) { const auto evicted = app.resources.artwork_order.front(); app.resources.artwork_order.pop_front(); if (evicted == key && (app.resources.artwork_bytes > app.resources.max_artwork_bytes || app.resources.artwork.size() > app.resources.max_artwork_entries)) continue; const auto it = app.resources.artwork.find(evicted); if (it != app.resources.artwork.end()) { app.resources.artwork_bytes -= it->second.second.size(); app.resources.artwork.erase(it); } app.resources.artwork_versions.erase(evicted); } }
        app.publish_state(app.client.state().snapshot());
    });

    auto* context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "roon-display", handle_uri, &app.resources, nullptr);
    auto* manager = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(manager, "roonDisplay");
    g_signal_connect(manager, "script-message-received::roonDisplay", G_CALLBACK(on_script_message), &app);
    app.view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(manager));
    g_signal_connect(app.view, "load-changed", G_CALLBACK(on_load_changed), &app);
    g_object_unref(manager);

    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Roon Display");
    gtk_window_set_default_size(GTK_WINDOW(window), 1440, 900);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(app.view));
    webkit_web_view_load_uri(app.view, "roon-display://app/ui/index.html");
    gtk_widget_show_all(window);

    std::thread roon_thread([&app] { app.client.run(); });
    gtk_main();
    app.client.stop();
    roon_thread.join();
    return 0;
}
