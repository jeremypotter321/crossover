#include <csignal>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>

#include "Config.h"
#include "RelayServer.h"
#include "StatusServer.h"
#include "Tunnel.h"

namespace {

std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop = true; }

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) continue;
        else out += c;
    }
    return out;
}

std::string BuildJson(const crossover::StatusSnapshot& s, int tunnelConnections,
                      bool tunnelEnabled) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << "{\"status\":\"" << (s.running ? "running" : "stopped") << "\""
       << ",\"uptimeSeconds\":" << (s.uptimeMs / 1000)
       << ",\"players\":" << s.playerCount
       << ",\"maxPlayers\":" << s.maxPlayers
       << ",\"transport\":\"" << (tunnelEnabled ? "tcp-tunnel" : "direct-udp") << "\"";
    if (tunnelEnabled) os << ",\"tunnelConnections\":" << tunnelConnections;
    if (s.anchorKnown) {
        os << ",\"rendezvous\":{\"x\":" << s.anchor.x << ",\"y\":" << s.anchor.y
           << ",\"z\":" << s.anchor.z << "}";
    } else {
        os << ",\"rendezvous\":null";
    }
    os << ",\"roster\":[";
    for (size_t i = 0; i < s.players.size(); ++i) {
        const auto& p = s.players[i];
        if (i) os << ",";
        os << "{\"networkId\":" << p.networkId
           << ",\"address\":\"" << JsonEscape(p.address) << "\""
           << ",\"inWorld\":" << (p.spawned ? "true" : "false")
           << ",\"connectedForSeconds\":" << (p.connectedForMs / 1000)
           << ",\"position\":{\"x\":" << p.position.x << ",\"y\":" << p.position.y
           << ",\"z\":" << p.position.z << "}}";
    }
    os << "]}";
    return os.str();
}

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::signal(SIGTERM, OnSignal); // Railway sends SIGTERM on redeploy
    std::signal(SIGINT, OnSignal);
    std::signal(SIGPIPE, SIG_IGN);

    const crossover::Config config = crossover::Config::FromEnvironment();
    std::cout << "[crossover] Fable: The Lost Chapters dedicated server" << std::endl;
    std::cout << "[crossover] " << config.Describe() << std::endl;

    if (!crossover::RelayServer::ProtocolSelfTest()) {
        std::cerr << "[crossover] refusing to start: wire format does not match the mod"
                  << std::endl;
        return 1;
    }

    if (!config.spawnConfigured) {
        std::cout << "[crossover] NOTE: CROSSOVER_SPAWN_X/Y/Z is unset. The first player to "
                     "join an empty server is teleported to (0, 0, 0). Later joiners land on "
                     "whoever is already in-world, so in practice: have one player join, walk "
                     "to a sensible meeting point, and read the logged position back into "
                     "CROSSOVER_SPAWN_X/Y/Z (or mount a volume so it is remembered)."
                  << std::endl;
    }

    crossover::RelayServer relay(config);
    if (!relay.Start()) return 1;

    crossover::TunnelServer tunnel(config.tunnelPort, config.gamePort, config.maxPlayers);
    if (config.tunnelEnabled && !tunnel.Start()) {
        relay.Stop();
        return 1;
    }

    crossover::StatusServer status(config.statusPort, [&]() {
        return BuildJson(relay.Snapshot(),
                         config.tunnelEnabled ? tunnel.ActiveConnections() : 0,
                         config.tunnelEnabled);
    });
    status.Start();

    const auto tickInterval = std::chrono::microseconds(1000000 / config.tickHz);
    while (!g_stop) {
        const auto begin = std::chrono::steady_clock::now();
        relay.Tick();
        std::this_thread::sleep_until(begin + tickInterval);
    }

    std::cout << "[crossover] shutting down" << std::endl;
    status.Stop();
    if (config.tunnelEnabled) tunnel.Stop();
    relay.Stop();
    return 0;
}
