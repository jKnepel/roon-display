#include "roon_display/display_state.hpp"
#include "roon_display/roon_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void send_all(int fd, const std::string& text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) throw std::runtime_error("preview socket closed");
        offset += static_cast<std::size_t>(count);
    }
}

void send_bytes(int fd, const std::string& header, const std::vector<std::uint8_t>& bytes) {
    send_all(fd, header);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) throw std::runtime_error("preview socket closed");
        offset += static_cast<std::size_t>(count);
    }
}

class PreviewServer {
public:
    PreviewServer(std::uint16_t port, std::string root) : port_(port), root_(std::move(root)) {}
    ~PreviewServer() { if (socket_ >= 0) ::close(socket_); }

    void publish_state(const roon_display::DisplayState& state) {
        auto json = roon_display::to_json(state);
        {
            std::lock_guard lock(mutex_);
            for (const auto& [key, version] : artwork_versions_) {
                const std::string old_url = "/artwork/" + key + "\"";
                const std::string new_url = "/artwork/" + key + "?v=" + std::to_string(version) + "\"";
                const auto position = json.find(old_url);
                if (position != std::string::npos) json.replace(position, old_url.size(), new_url);
            }
        }
        { std::lock_guard lock(mutex_); latest_state_ = json; }
        publish(json);
    }

    void publish(const std::string& json) {
        std::lock_guard lock(mutex_);
        const std::string event = "data: " + json + "\n\n";
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](int fd) {
            try { send_all(fd, event); return false; }
            catch (...) { ::close(fd); return true; }
        }), clients_.end());
    }

    void cache_artwork(const std::string& key, const std::string& mime, const std::vector<std::uint8_t>& bytes) {
        std::lock_guard lock(mutex_);
        artwork_[key] = {mime.empty() ? "image/jpeg" : mime, bytes};
        ++artwork_versions_[key];
    }

    void run(roon_display::RoonClient& client) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_ < 0) throw std::runtime_error("cannot create preview socket");
        int reuse = 1;
        ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(socket_, 16) < 0) throw std::runtime_error("cannot bind preview server");
        std::cout << "Live preview: http://127.0.0.1:" << port_ << "/ui/\n";
        for (;;) {
            const int accepted_fd = ::accept(socket_, nullptr, nullptr);
            if (accepted_fd < 0) continue;
            handle(accepted_fd, client);
        }
    }

private:
    std::uint16_t port_;
    std::string root_;
    int socket_{-1};
    std::mutex mutex_;
    std::vector<int> clients_;
    std::map<std::string, std::pair<std::string, std::vector<std::uint8_t>>> artwork_;
    std::map<std::string, std::size_t> artwork_versions_;
    std::string latest_state_{"{\"activeZone\":null}"};



    void handle(int fd, roon_display::RoonClient& roon) {
        std::string raw;
        char buffer[4096]{};
        for (;;) {
            const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) { ::close(fd); return; }
            raw.append(buffer, static_cast<std::size_t>(count));
            const auto separator = raw.find("\r\n\r\n");
            if (separator == std::string::npos) continue;
            std::size_t content_length = 0;
            const auto length_marker = raw.find("Content-Length:");
            if (length_marker != std::string::npos) content_length = static_cast<std::size_t>(std::stoul(raw.substr(length_marker + 15)));
            if (raw.size() >= separator + 4 + content_length) break;
        }
        std::istringstream request(raw);
        std::string method, path, version;
        request >> method >> path >> version;
        if ((method != "GET" && method != "POST") || path.find("..") != std::string::npos) {
            ::close(fd); return;
        }
        if (method == "POST" && path == "/control") {
            const auto separator = raw.find("\r\n\r\n");
            const std::string body = separator == std::string::npos ? "" : raw.substr(separator + 4);
            const auto marker = body.find("\"command\":\"");
            if (marker == std::string::npos) { send_all(fd, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"); ::close(fd); return; }
            const auto start = marker + 11;
            const auto end = body.find("\"", start);
            const auto command = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
            bool accepted = false;
            if (command == "seek") {
                const auto position_marker = body.find("\"position\":");
                if (position_marker != std::string::npos) { try { accepted = roon.seek(std::stod(body.substr(position_marker + 11))); } catch (...) {} }
            } else accepted = roon.control(command);
            const std::string response = accepted ? "{\"ok\":true}" : "{\"ok\":false}";
            send_all(fd, "HTTP/1.1 " + std::string(accepted ? "200 OK" : "409 Conflict") + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(response.size()) + "\r\n\r\n" + response);
            ::close(fd); return;
        }

        if (path == "/state") {
            std::lock_guard lock(mutex_);
            send_all(fd, "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(latest_state_.size()) + "\r\n\r\n" + latest_state_);
            ::close(fd); return;
        }

        if (path == "/events") {
            try {
                send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                              "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n"
                              "data: {\"activeZone\":null}\n\n");
                std::lock_guard lock(mutex_);
                clients_.push_back(fd);
            } catch (...) { ::close(fd); }
            return;
        }
        if (path.starts_with("/artwork/")) {
            const auto key = path.substr(9);
            const auto query = key.find("?");
            const auto artwork_key = query == std::string::npos ? key : key.substr(0, query);
            std::lock_guard lock(mutex_);
            const auto image = artwork_.find(artwork_key);
            if (image == artwork_.end()) {
                send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            } else {
                const auto& [mime, bytes] = image->second;
                send_bytes(fd, "HTTP/1.1 200 OK\r\nContent-Type: " + mime + "\r\nContent-Length: " + std::to_string(bytes.size()) + "\r\n\r\n", bytes);
            }
            ::close(fd);
            return;
        }
        const auto query = path.find('?');
        if (query != std::string::npos) path.resize(query);
        if (path == "/" || path == "/ui" || path == "/ui/") path = "/ui/index.html";
        const std::string file = root_ + path;
        std::ifstream input(file, std::ios::binary);
        if (!input) { send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"); ::close(fd); return; }
        std::ostringstream contents; contents << input.rdbuf();
        const auto body = contents.str();
        const auto type = path.ends_with(".html") ? "text/html" : path.ends_with(".css") ? "text/css" : "application/javascript";
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) +
                       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
        ::close(fd);
    }
};

} // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 8787);
    const std::string root = argc > 2 ? argv[2] : ".";
    try {
        PreviewServer server(port, root);
        roon_display::RoonClient client({"Live Roon Preview", {}, "com.roonlabs.display_zone", true});
        client.set_state_callback([&server](const roon_display::DisplayState& state) {
            server.publish_state(state);
        });
        client.set_artwork_callback([&server, &client](const std::string& key, const std::string& mime, const std::vector<std::uint8_t>& bytes) {
            server.cache_artwork(key, mime, bytes);
            server.publish_state(client.state().snapshot());
            std::cout << "Artwork cached: " << key << " (" << bytes.size() << " bytes)\n";
        });
        std::thread roon_thread([&client] { client.run(); });
        server.run(client);
        roon_thread.join();
    } catch (const std::exception& error) {
        std::cerr << "live-preview: " << error.what() << '\n';
        return 1;
    }
}
