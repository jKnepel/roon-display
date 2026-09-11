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

class PreviewServer {
public:
    PreviewServer(std::uint16_t port, std::string root) : port_(port), root_(std::move(root)) {}
    ~PreviewServer() { if (socket_ >= 0) ::close(socket_); }

    void publish(const std::string& json) {
        std::lock_guard lock(mutex_);
        const std::string event = "data: " + json + "\n\n";
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](int fd) {
            try { send_all(fd, event); return false; }
            catch (...) { ::close(fd); return true; }
        }), clients_.end());
    }

    void run() {
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
            const int client = ::accept(socket_, nullptr, nullptr);
            if (client < 0) continue;
            handle(client);
        }
    }

private:
    std::uint16_t port_;
    std::string root_;
    int socket_{-1};
    std::mutex mutex_;
    std::vector<int> clients_;

    void handle(int fd) {
        char buffer[4096]{};
        const auto count = ::recv(fd, buffer, sizeof(buffer) - 1, 0);
        if (count <= 0) { ::close(fd); return; }
        std::istringstream request(std::string(buffer, static_cast<std::size_t>(count)));
        std::string method, path, version;
        request >> method >> path >> version;
        if (method != "GET" || path.find("..") != std::string::npos) {
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
        if (path == "/" || path == "/ui" || path == "/ui/") path = "/ui/index.html";
        const auto query = path.find('?');
        if (query != std::string::npos) path.resize(query);
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
            server.publish(roon_display::to_json(state));
        });
        std::thread roon_thread([&client] { client.run(); });
        server.run();
        roon_thread.join();
    } catch (const std::exception& error) {
        std::cerr << "live-preview: " << error.what() << '\n';
        return 1;
    }
}
