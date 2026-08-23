#include "StatusServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace crossover {
namespace {

void Log(const std::string& line) {
    std::cout << "[status] " << line << std::endl;
}

void Respond(int fd, const char* status, const char* contentType, const std::string& body) {
    std::string out = std::string("HTTP/1.1 ") + status + "\r\n" +
                      "Content-Type: " + contentType + "\r\n" +
                      "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                      "Cache-Control: no-store\r\n" +
                      "Connection: close\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

} // namespace

StatusServer::StatusServer(unsigned short port, JsonProvider provider)
    : port_(port), provider_(std::move(provider)) {}

StatusServer::~StatusServer() { Stop(); }

bool StatusServer::Start() {
    if (port_ == 0) return true; // disabled

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;

    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listenFd_, 8) < 0) {
        Log("could not listen on " + std::to_string(port_) + ": " + std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&StatusServer::ServeLoop, this);
    Log("listening on TCP " + std::to_string(port_));
    return true;
}

void StatusServer::Stop() {
    if (!running_.exchange(false)) return;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void StatusServer::ServeLoop() {
    while (running_) {
        struct pollfd pfd{listenFd_, POLLIN, 0};
        if (::poll(&pfd, 1, 500) <= 0) continue;
        if (!running_) break;

        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) continue;

        char buf[1024] = {0};
        struct pollfd rfd{fd, POLLIN, 0};
        if (::poll(&rfd, 1, 2000) > 0) {
            ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) buf[n] = '\0';
        }

        const std::string request(buf);
        if (request.rfind("GET /health", 0) == 0) {
            Respond(fd, "200 OK", "text/plain", "ok");
        } else if (request.rfind("GET ", 0) == 0) {
            Respond(fd, "200 OK", "application/json", provider_());
        } else {
            Respond(fd, "405 Method Not Allowed", "text/plain", "");
        }
        ::close(fd);
    }
}

} // namespace crossover
