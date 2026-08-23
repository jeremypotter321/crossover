#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace crossover {

// Railway has no inbound UDP -- only an HTTP router and a raw TCP proxy -- while
// SLikeNet's RakPeerInterface is UDP-only with no TCP mode. This bridges the two by
// carrying whole UDP datagrams over one TCP connection per player:
//
//   Fable + mod --UDP--> tunnel client --TCP--> Railway TCP Proxy
//                                                  |
//                                          TunnelServer (this)
//                                                  |
//                                     one loopback UDP socket per player
//                                                  |
//                                            RelayServer (SLikeNet)
//
// Every player therefore reaches SLikeNet as 127.0.0.1 on a *distinct* ephemeral
// port. That is safe: RakNet keys peers on the full IP:port tuple, and its
// same-IP connection-frequency limiter is off by default (RakPeer.cpp:312).
//
// Framing is [uint16 big-endian length][datagram]. Datagrams are bounded by
// RakNet's MTU, well under the 2 KB cap enforced here.
class TunnelServer {
public:
    TunnelServer(unsigned short listenPort, unsigned short relayUdpPort, int maxConnections);
    ~TunnelServer();

    bool Start();
    void Stop();

    int ActiveConnections() const { return active_.load(); }

private:
    void AcceptLoop();
    void PumpConnection(int tcpFd);

    unsigned short listenPort_;
    unsigned short relayUdpPort_;
    int maxConnections_;

    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
    std::thread acceptThread_;
};

} // namespace crossover
