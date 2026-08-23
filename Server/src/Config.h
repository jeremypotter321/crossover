#pragma once

#include <string>

#include "Protocol.h"

namespace crossover {

struct Config {
    // UDP port the SLikeNet relay binds. On Railway this is never exposed --
    // it only ever receives loopback traffic from the built-in TCP tunnel.
    unsigned short gamePort = 60000;

    // TCP port the tunnel listens on. This is the one Railway's TCP Proxy maps
    // to a public host:port. 0 disables the tunnel (direct-UDP hosting).
    unsigned short tunnelPort = 60001;
    bool tunnelEnabled = true;

    // Plain-HTTP status/health endpoint, served on Railway's HTTP domain.
    // 0 disables it.
    unsigned short statusPort = 8080;

    // Concurrent players. The mod's own host defaults to 4 including itself; a
    // dedicated server owns no avatar, so this is purely client slots.
    int maxPlayers = 8;

    // Where a joining client is teleported to.
    //   anchor : the position of the longest-standing player, falling back to
    //            `spawn` when the server is empty. This reproduces the mod's
    //            "join lands you on the host" behaviour.
    //   fixed  : always `spawn`.
    enum class SpawnMode { Anchor, Fixed };
    SpawnMode spawnMode = SpawnMode::Anchor;
    Vec3 spawn{};
    bool spawnConfigured = false;

    // Persist the last anchor position so a redeploy keeps the rendezvous point.
    // Empty disables persistence.
    std::string stateFile;

    int tickHz = 60;
    bool verbose = false;

    static Config FromEnvironment();
    std::string Describe() const;
};

} // namespace crossover
