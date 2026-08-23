#include "Config.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace crossover {
namespace {

const char* EnvOrNull(const char* name) {
    const char* v = std::getenv(name);
    if (v && *v) return v;
    return nullptr;
}

// First non-empty of the given env vars.
const char* FirstEnv(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (const char* v = EnvOrNull(n)) return v;
    }
    return nullptr;
}

int EnvInt(std::initializer_list<const char*> names, int fallback) {
    const char* v = FirstEnv(names);
    if (!v) return fallback;
    try {
        return std::stoi(v);
    } catch (...) {
        return fallback;
    }
}

bool EnvFloat(const char* name, float* out) {
    const char* v = EnvOrNull(name);
    if (!v) return false;
    try {
        *out = std::stof(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool EnvBool(const char* name, bool fallback) {
    const char* v = EnvOrNull(name);
    if (!v) return fallback;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on";
}

} // namespace

Config Config::FromEnvironment() {
    Config c;

    // The relay's UDP port stays internal on Railway, so it needs no env plumbing;
    // it is only configurable for direct-UDP hosting on a normal VPS.
    c.gamePort = static_cast<unsigned short>(EnvInt({"CROSSOVER_GAME_PORT"}, 60000));

    // RAILWAY_TCP_APPLICATION_PORT is the in-container port Railway's TCP Proxy
    // forwards to. That is the tunnel's listener.
    c.tunnelPort = static_cast<unsigned short>(
        EnvInt({"CROSSOVER_TUNNEL_PORT", "RAILWAY_TCP_APPLICATION_PORT"}, 60001));
    c.tunnelEnabled = EnvBool("CROSSOVER_TUNNEL", true) && c.tunnelPort != 0;

    // PORT is what Railway's HTTP domain routes to.
    c.statusPort = static_cast<unsigned short>(EnvInt({"CROSSOVER_STATUS_PORT", "PORT"}, 8080));

    if (c.tunnelEnabled && c.tunnelPort == c.gamePort) {
        // Same number on two protocols is legal but confusing, and it makes a
        // misconfigured direct-UDP deploy look like a working tunnel. Refuse.
        c.tunnelPort = static_cast<unsigned short>(c.gamePort + 1);
    }

    c.maxPlayers = EnvInt({"CROSSOVER_MAX_PLAYERS"}, 8);
    if (c.maxPlayers < 1) c.maxPlayers = 1;
    if (c.maxPlayers > 64) c.maxPlayers = 64;

    c.tickHz = EnvInt({"CROSSOVER_TICK_HZ"}, 60);
    if (c.tickHz < 10) c.tickHz = 10;
    if (c.tickHz > 240) c.tickHz = 240;

    if (const char* mode = EnvOrNull("CROSSOVER_SPAWN_MODE")) {
        c.spawnMode = (std::string(mode) == "fixed") ? SpawnMode::Fixed : SpawnMode::Anchor;
    }

    bool gotX = EnvFloat("CROSSOVER_SPAWN_X", &c.spawn.x);
    bool gotY = EnvFloat("CROSSOVER_SPAWN_Y", &c.spawn.y);
    bool gotZ = EnvFloat("CROSSOVER_SPAWN_Z", &c.spawn.z);
    c.spawnConfigured = gotX || gotY || gotZ;

    if (const char* sf = EnvOrNull("CROSSOVER_STATE_FILE")) {
        c.stateFile = sf;
    } else {
        c.stateFile = "/data/crossover-spawn.txt"; // no-op unless a volume is mounted
    }

    c.verbose = EnvBool("CROSSOVER_VERBOSE", false);
    return c;
}

std::string Config::Describe() const {
    std::ostringstream os;
    os << "gamePort=" << gamePort << "/udp"
       << " tunnel=" << (tunnelEnabled ? std::to_string(tunnelPort) + "/tcp" : std::string("off"))
       << " statusPort=" << (statusPort ? std::to_string(statusPort) : std::string("off"))
       << " maxPlayers=" << maxPlayers
       << " tickHz=" << tickHz
       << " spawnMode=" << (spawnMode == SpawnMode::Anchor ? "anchor" : "fixed")
       << " spawn=(" << spawn.x << ", " << spawn.y << ", " << spawn.z << ")"
       << (spawnConfigured ? "" : " [default]")
       << " stateFile=" << (stateFile.empty() ? std::string("off") : stateFile);
    return os.str();
}

} // namespace crossover
