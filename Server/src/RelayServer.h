#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <slikenet/peerinterface.h>
#include <slikenet/types.h>

#include "Config.h"
#include "Protocol.h"

namespace crossover {

// One connected game client.
struct Player {
    SLNet::SystemAddress address;
    SLNet::RakNetGUID    guid;
    int  networkId = -1;

    // False between ID_CONNECTION_NOTIFICATION and the client's ID_CREATE_NET_PLAYER.
    // Nothing is relayed to a player until it is true: the client has no avatar for
    // anyone yet, and NetPlayerManager::CreateNetPlayer would run against a
    // half-initialised local player.
    bool spawned = false;

    int  defGlobalIndex = 0;
    Vec3 position{};
    Vec3 up{};
    Vec3 forward{};

    int64_t connectedAtMs = 0;
    int64_t lastPacketMs  = 0;
    uint64_t packetsIn    = 0;

    // Crude flood guard, reset once a second.
    int64_t rateWindowStartMs = 0;
    int     rateWindowCount   = 0;
    bool    rateLimited       = false;
};

struct StatusSnapshot {
    bool running = false;
    int  playerCount = 0;
    int  maxPlayers = 0;
    unsigned short gamePort = 0;
    int64_t uptimeMs = 0;
    Vec3 anchor{};
    bool anchorKnown = false;
    struct Entry {
        int networkId;
        std::string address;
        bool spawned;
        Vec3 position;
        int64_t connectedForMs;
    };
    std::vector<Entry> players;
};

// Reproduces exactly the slice of NetPlayerManager that the mod's *host* performs
// for other players -- id assignment, spawn handoff, roster sync, movement relay,
// teardown -- while owning no avatar of its own. The mod needs no changes to talk
// to it: a client simply connects to this address instead of to another player.
class RelayServer {
public:
    explicit RelayServer(const Config& config);
    ~RelayServer();

    // Asserts the BitStream encoding assumptions in Protocol.h. Returns false and
    // logs loudly on mismatch rather than corrupting a live session.
    static bool ProtocolSelfTest();

    bool Start();
    void Tick();
    void Stop();

    StatusSnapshot Snapshot() const;

private:
    void HandleConnectionNotification(SLNet::Packet* packet);
    void HandleCreateNetPlayer(SLNet::Packet* packet);
    void HandlePlayerMovement(SLNet::Packet* packet);
    void HandlePlayerRotation(SLNet::Packet* packet);
    void HandleDrop(SLNet::Packet* packet, const char* reason);

    Player* FindByAddress(const SLNet::SystemAddress& address);
    Player* FindByNetworkId(int networkId);
    int  AllocateNetworkId();
    Vec3 SpawnPositionFor() const;
    const Player* AnchorPlayer() const;

    void SendRoster(const Player& to);
    void RelayExcept(int exceptNetworkId, const SLNet::BitStream& bs,
                     PacketPriority priority, PacketReliability reliability);
    void SendTo(const SLNet::SystemAddress& address, const SLNet::BitStream& bs,
                PacketPriority priority, PacketReliability reliability);

    bool AcceptRate(Player& player, int64_t nowMs);
    void PersistAnchor();
    void LoadPersistedAnchor();

    Config config_;
    SLNet::RakPeerInterface* peer_ = nullptr;
    std::vector<Player> players_;
    mutable std::mutex mutex_;

    int nextNetworkId_ = kFirstClientNetworkId;
    int64_t startedAtMs_ = 0;
    int64_t lastPersistMs_ = 0;
    Vec3 persistedAnchor_{};
    bool persistedAnchorKnown_ = false;
};

} // namespace crossover
