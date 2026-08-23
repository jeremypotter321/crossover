#include "Tunnel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace crossover {
namespace {

constexpr size_t kMaxDatagram = 2048;
constexpr size_t kFrameHeader = 2;

void Log(const std::string& line) {
    std::cout << "[tunnel] " << line << std::endl;
}

bool WriteAll(int fd, const unsigned char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EINTR)) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, 2000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

TunnelServer::TunnelServer(unsigned short listenPort, unsigned short relayUdpPort,
                           int maxConnections)
    : listenPort_(listenPort), relayUdpPort_(relayUdpPort), maxConnections_(maxConnections) {}

TunnelServer::~TunnelServer() { Stop(); }

bool TunnelServer::Start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        Log(std::string("FATAL: socket() failed: ") + std::strerror(errno));
        return false;
    }

    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listenPort_);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        Log("FATAL: bind(" + std::to_string(listenPort_) + "/tcp) failed: " +
            std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::listen(listenFd_, 16) < 0) {
        Log(std::string("FATAL: listen() failed: ") + std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&TunnelServer::AcceptLoop, this);
    Log("listening on TCP " + std::to_string(listenPort_) + ", bridging to UDP 127.0.0.1:" +
        std::to_string(relayUdpPort_));
    return true;
}

void TunnelServer::Stop() {
    if (!running_.exchange(false)) return;

    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) acceptThread_.join();

    // Workers poll running_ on a 500 ms tick, so they unwind promptly.
    for (int i = 0; i < 40 && active_.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (active_.load() > 0) {
        Log("warning: " + std::to_string(active_.load()) + " tunnel worker(s) still draining");
    }
    Log("stopped");
}

void TunnelServer::AcceptLoop() {
    while (running_) {
        struct pollfd pfd{listenFd_, POLLIN, 0};
        int ready = ::poll(&pfd, 1, 500);
        if (ready <= 0) continue;
        if (!running_) break;

        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) continue;

        if (active_.load() >= maxConnections_) {
            Log("refusing connection: tunnel at capacity (" +
                std::to_string(maxConnections_) + ")");
            ::close(fd);
            continue;
        }

        // Nagle would coalesce movement datagrams and add tens of milliseconds.
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        Log(std::string("tunnel open from ") + ip + ":" + std::to_string(ntohs(peer.sin_port)));

        // Detached: a finished std::thread is still joinable, so there is no cheap
        // way to reap a vector of them. Stop() drains on the active_ counter instead.
        std::thread(&TunnelServer::PumpConnection, this, fd).detach();
    }
}

void TunnelServer::PumpConnection(int tcpFd) {
    active_++;

    // A fresh loopback socket per player is what gives each of them a distinct
    // SystemAddress inside SLikeNet.
    int udpFd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd < 0) {
        Log(std::string("socket(UDP) failed: ") + std::strerror(errno));
        ::close(tcpFd);
        active_--;
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0; // ephemeral
    if (::bind(udpFd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        Log(std::string("bind(UDP) failed: ") + std::strerror(errno));
        ::close(udpFd);
        ::close(tcpFd);
        active_--;
        return;
    }

    sockaddr_in relay{};
    relay.sin_family = AF_INET;
    relay.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    relay.sin_port = htons(relayUdpPort_);

    std::vector<unsigned char> inbound;
    inbound.reserve(kMaxDatagram * 4);
    unsigned char scratch[kMaxDatagram + kFrameHeader];

    while (running_) {
        struct pollfd pfds[2] = {{tcpFd, POLLIN, 0}, {udpFd, POLLIN, 0}};
        int ready = ::poll(pfds, 2, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        // TCP -> UDP: de-frame whatever whole datagrams have arrived.
        if (pfds[0].revents & POLLIN) {
            ssize_t n = ::recv(tcpFd, scratch, sizeof(scratch), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            inbound.insert(inbound.end(), scratch, scratch + n);

            bool fatal = false;
            while (inbound.size() >= kFrameHeader) {
                const size_t len = (static_cast<size_t>(inbound[0]) << 8) | inbound[1];
                if (len == 0 || len > kMaxDatagram) {
                    Log("closing tunnel: bad frame length " + std::to_string(len));
                    fatal = true;
                    break;
                }
                if (inbound.size() < kFrameHeader + len) break;
                ::sendto(udpFd, inbound.data() + kFrameHeader, len, 0,
                         reinterpret_cast<sockaddr*>(&relay), sizeof(relay));
                inbound.erase(inbound.begin(), inbound.begin() + kFrameHeader + len);
            }
            if (fatal) break;
        }

        // UDP -> TCP: frame each datagram whole.
        if (pfds[1].revents & POLLIN) {
            bool writeFailed = false;
            for (;;) {
                sockaddr_in from{};
                socklen_t fromLen = sizeof(from);
                ssize_t n = ::recvfrom(udpFd, scratch + kFrameHeader, kMaxDatagram,
                                       MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n <= 0) break;
                scratch[0] = static_cast<unsigned char>((n >> 8) & 0xFF);
                scratch[1] = static_cast<unsigned char>(n & 0xFF);
                if (!WriteAll(tcpFd, scratch, kFrameHeader + static_cast<size_t>(n))) {
                    writeFailed = true;
                    break;
                }
            }
            if (writeFailed) break;
        }
    }

    ::close(udpFd);
    ::close(tcpFd);
    active_--;
    Log("tunnel closed; " + std::to_string(active_.load()) + " open");
}

} // namespace crossover
