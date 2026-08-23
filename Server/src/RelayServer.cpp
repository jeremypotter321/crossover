#include "RelayServer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

#include <slikenet/MessageIdentifiers.h>
#include <slikenet/version.h>

namespace crossover {
namespace {

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Log(const std::string& line) {
    std::cout << "[crossover] " << line << std::endl;
}

std::string VecStr(const Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

// Packets arriving faster than this from one client are dropped. A well-behaved
// client sends movement + rotation once per physics resolve, nowhere near this.
constexpr int kMaxPacketsPerSecond = 600;
constexpr int64_t kPersistIntervalMs = 10000;

} // namespace

RelayServer::RelayServer(const Config& config) : config_(config) {}

RelayServer::~RelayServer() { Stop(); }

bool RelayServer::ProtocolSelfTest() {
    bool ok = true;

    if (static_cast<int>(ID_CONNECTION_NOTIFICATION) != 134) {
        Log("SELFTEST FAIL: ID_USER_PACKET_ENUM is not 134 -- SLikeNet version mismatch");
        ok = false;
    }

    // A 12-byte vector must land on the wire as BE(z), BE(y), BE(x). See Protocol.h.
    SLNet::BitStream bs;
    Vec3 v{1.0f, 2.0f, 3.0f};
    bs.Write(v);
    const unsigned char expected[12] = {
        0x40, 0x40, 0x00, 0x00,  // 3.0f big-endian
        0x40, 0x00, 0x00, 0x00,  // 2.0f big-endian
        0x3F, 0x80, 0x00, 0x00,  // 1.0f big-endian
    };
    if (bs.GetNumberOfBytesUsed() != 12 ||
        std::memcmp(bs.GetData(), expected, 12) != 0) {
        char got[64] = {0};
        for (int i = 0; i < std::min(12, (int)bs.GetNumberOfBytesUsed()); ++i)
            std::snprintf(got + i * 3, 4, "%02X ", bs.GetData()[i]);
        Log(std::string("SELFTEST FAIL: Vec3 wire layout is ") + got +
            "-- expected 40 40 00 00 40 00 00 00 3F 80 00 00 (z,y,x big-endian)");
        ok = false;
    }

    // And a plain int must be big-endian.
    SLNet::BitStream bsi;
    int one = 1;
    bsi.Write(one);
    const unsigned char expectedInt[4] = {0x00, 0x00, 0x00, 0x01};
    if (bsi.GetNumberOfBytesUsed() != 4 ||
        std::memcmp(bsi.GetData(), expectedInt, 4) != 0) {
        Log("SELFTEST FAIL: int is not big-endian on the wire");
        ok = false;
    }

    // Round-trip through the same path a client would use.
    SLNet::BitStream rt(bs.GetData(), bs.GetNumberOfBytesUsed(), false);
    Vec3 back{};
    rt.Read(back);
    if (back.x != 1.0f || back.y != 2.0f || back.z != 3.0f) {
        Log("SELFTEST FAIL: Vec3 does not round-trip");
        ok = false;
    }

    if (ok) Log("protocol self-test passed (ID base 134, big-endian z/y/x vectors)");
    return ok;
}

bool RelayServer::Start() {
    peer_ = SLNet::RakPeerInterface::GetInstance();
    if (!peer_) {
        Log("FATAL: could not create RakPeerInterface");
        return false;
    }

    SLNet::SocketDescriptor sd(config_.gamePort, nullptr);
    sd.socketFamily = AF_INET;

    // maxConnections must cover every client slot; the server holds no slot itself.
    SLNet::StartupResult result =
        peer_->Startup(config_.maxPlayers, &sd, 1);
    if (result != SLNet::RAKNET_STARTED) {
        Log("FATAL: Startup failed on UDP port " + std::to_string(config_.gamePort) +
            " (code " + std::to_string(static_cast<int>(result)) + ")");
        SLNet::RakPeerInterface::DestroyInstance(peer_);
        peer_ = nullptr;
        return false;
    }

    peer_->SetMaximumIncomingConnections(config_.maxPlayers);
    peer_->SetOccasionalPing(true);

    startedAtMs_ = NowMs();
    lastPersistMs_ = startedAtMs_;
    LoadPersistedAnchor();

    Log("listening on UDP " + std::to_string(config_.gamePort) +
        " for up to " + std::to_string(config_.maxPlayers) + " players");
    return true;
}

void RelayServer::Stop() {
    if (!peer_) return;

    PersistAnchor();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Player& p : players_) {
            if (!p.spawned) continue;
            SLNet::BitStream bs;
            bs.Write(static_cast<SLNet::MessageID>(ID_DESTROY_NET_PLAYER));
            bs.Write(p.networkId);
            RelayExcept(p.networkId, bs, HIGH_PRIORITY, RELIABLE_ORDERED);
        }
        players_.clear();
    }
    peer_->Shutdown(500);
    SLNet::RakPeerInterface::DestroyInstance(peer_);
    peer_ = nullptr;
    Log("stopped");
}

void RelayServer::Tick() {
    if (!peer_) return;

    for (SLNet::Packet* packet = peer_->Receive(); packet;
         peer_->DeallocatePacket(packet), packet = peer_->Receive()) {
        const unsigned char id = packet->data[0];

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (Player* p = FindByAddress(packet->systemAddress)) {
                p->lastPacketMs = NowMs();
                p->packetsIn++;
                if (!AcceptRate(*p, p->lastPacketMs)) continue;
            }
        }

        switch (id) {
        case ID_NEW_INCOMING_CONNECTION:
            Log(std::string("connection from ") + packet->systemAddress.ToString(true));
            break;

        case crossover::ID_CONNECTION_NOTIFICATION:
            HandleConnectionNotification(packet);
            break;

        case crossover::ID_CREATE_NET_PLAYER:
            HandleCreateNetPlayer(packet);
            break;

        case crossover::ID_PLAYER_MOVEMENT:
            HandlePlayerMovement(packet);
            break;

        case crossover::ID_PLAYER_ROTATION:
            HandlePlayerRotation(packet);
            break;

        case ID_DISCONNECTION_NOTIFICATION:
            HandleDrop(packet, "disconnected");
            break;

        case ID_CONNECTION_LOST:
            HandleDrop(packet, "connection lost");
            break;

        case ID_INCOMPATIBLE_PROTOCOL_VERSION:
            Log(std::string("rejected ") + packet->systemAddress.ToString(true) +
                ": incompatible RakNet protocol (client reports " +
                std::to_string(packet->length > 1 ? (int)packet->data[1] : -1) +
                ", server is " + std::to_string(RAKNET_PROTOCOL_VERSION) + ")");
            break;

        case ID_NO_FREE_INCOMING_CONNECTIONS:
            Log(std::string("rejected ") + packet->systemAddress.ToString(true) + ": server full");
            break;

        // The client should never send these to us, but the mod's own host logs
        // unknowns rather than dropping the connection, so mirror that.
        default:
            if (config_.verbose) {
                Log("unhandled message id " + std::to_string((int)id) + " from " +
                    packet->systemAddress.ToString(true));
            }
            break;
        }
    }

    if (NowMs() - lastPersistMs_ > kPersistIntervalMs) {
        lastPersistMs_ = NowMs();
        PersistAnchor();
    }
}

void RelayServer::HandleConnectionNotification(SLNet::Packet* packet) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (FindByAddress(packet->systemAddress)) {
        Log(std::string("duplicate ID_CONNECTION_NOTIFICATION from ") +
            packet->systemAddress.ToString(true) + " -- ignored");
        return;
    }

    if (static_cast<int>(players_.size()) >= config_.maxPlayers) {
        Log(std::string("refusing ") + packet->systemAddress.ToString(true) + ": server full");
        peer_->CloseConnection(packet->systemAddress, true);
        return;
    }

    Player p;
    p.address = packet->systemAddress;
    p.guid = packet->guid;
    p.networkId = AllocateNetworkId();
    p.connectedAtMs = NowMs();
    p.lastPacketMs = p.connectedAtMs;
    p.rateWindowStartMs = p.connectedAtMs;
    players_.push_back(p);

    const Vec3 spawn = SpawnPositionFor();

    // Exactly what NetPlayerManager::ConnectionNotification sends. RELIABLE_ORDERED
    // matters: it must reach the client before any relayed traffic, because the
    // client's localNetPlayer does not exist until it lands.
    SLNet::BitStream bs;
    bs.Write(static_cast<SLNet::MessageID>(ID_CREATE_LOCAL_NET_PLAYER));
    bs.Write(p.networkId);
    bs.Write(spawn);
    SendTo(p.address, bs, HIGH_PRIORITY, RELIABLE_ORDERED);

    Log("assigned network id " + std::to_string(p.networkId) + " to " +
        p.address.ToString(true) + ", spawning at " + VecStr(spawn));
}

void RelayServer::HandleCreateNetPlayer(SLNet::Packet* packet) {
    if (packet->length < (unsigned)kSizeCreateNetPlayer) {
        Log("malformed ID_CREATE_NET_PLAYER (" + std::to_string(packet->length) + " bytes)");
        return;
    }

    SLNet::BitStream bs(packet->data, packet->length, false);
    bs.IgnoreBytes(sizeof(SLNet::MessageID));
    int claimedId = -1, defGlobalIndex = 0;
    Vec3 position{};
    bs.Read(claimedId);
    bs.Read(defGlobalIndex);
    bs.Read(position);

    std::lock_guard<std::mutex> lock(mutex_);
    Player* self = FindByAddress(packet->systemAddress);
    if (!self) {
        Log(std::string("ID_CREATE_NET_PLAYER from unknown ") +
            packet->systemAddress.ToString(true) + " -- ignored");
        return;
    }

    // The id is the server's to hand out; never trust the client's copy of it.
    if (claimedId != self->networkId) {
        Log("client " + std::string(self->address.ToString(true)) + " claimed network id " +
            std::to_string(claimedId) + ", using " + std::to_string(self->networkId));
    }

    const bool firstSpawn = !self->spawned;
    self->spawned = true;
    self->defGlobalIndex = defGlobalIndex;
    self->position = position;

    // Tell everyone already in the world about the newcomer.
    SLNet::BitStream out;
    out.Write(static_cast<SLNet::MessageID>(crossover::ID_CREATE_NET_PLAYER));
    out.Write(self->networkId);
    out.Write(defGlobalIndex);
    out.Write(position);
    RelayExcept(self->networkId, out, HIGH_PRIORITY, RELIABLE_ORDERED);

    // ...and tell the newcomer about everyone already in the world.
    SendRoster(*self);

    if (firstSpawn) {
        Log("player " + std::to_string(self->networkId) + " spawned at " + VecStr(position) +
            " (def " + std::to_string(defGlobalIndex) + "); " +
            std::to_string(players_.size()) + " connected");
    }
}

void RelayServer::HandlePlayerMovement(SLNet::Packet* packet) {
    if (packet->length < (unsigned)kSizePlayerMovement) return;

    SLNet::BitStream bs(packet->data, packet->length, false);
    bs.IgnoreBytes(sizeof(SLNet::MessageID));
    int claimedId = -1;
    Vec3 position{}, acceleration{};
    bs.Read(claimedId);
    bs.Read(position);
    bs.Read(acceleration);

    std::lock_guard<std::mutex> lock(mutex_);
    Player* self = FindByAddress(packet->systemAddress);
    if (!self || !self->spawned) return;

    self->position = position;

    SLNet::BitStream out;
    out.Write(static_cast<SLNet::MessageID>(crossover::ID_PLAYER_MOVEMENT));
    out.Write(self->networkId);
    out.Write(position);
    out.Write(acceleration);
    RelayExcept(self->networkId, out, HIGH_PRIORITY, UNRELIABLE_SEQUENCED);
}

void RelayServer::HandlePlayerRotation(SLNet::Packet* packet) {
    if (packet->length < (unsigned)kSizePlayerRotation) return;

    SLNet::BitStream bs(packet->data, packet->length, false);
    bs.IgnoreBytes(sizeof(SLNet::MessageID));
    int claimedId = -1;
    Vec3 up{}, forward{};
    bs.Read(claimedId);
    bs.Read(up);
    bs.Read(forward);

    std::lock_guard<std::mutex> lock(mutex_);
    Player* self = FindByAddress(packet->systemAddress);
    if (!self || !self->spawned) return;

    self->up = up;
    self->forward = forward;

    SLNet::BitStream out;
    out.Write(static_cast<SLNet::MessageID>(crossover::ID_PLAYER_ROTATION));
    out.Write(self->networkId);
    out.Write(up);
    out.Write(forward);
    RelayExcept(self->networkId, out, HIGH_PRIORITY, UNRELIABLE_SEQUENCED);
}

void RelayServer::HandleDrop(SLNet::Packet* packet, const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);

    Player* self = FindByAddress(packet->systemAddress);
    if (!self) return;

    const int networkId = self->networkId;
    const bool wasSpawned = self->spawned;

    players_.erase(std::remove_if(players_.begin(), players_.end(),
                                  [&](const Player& p) { return p.networkId == networkId; }),
                   players_.end());

    if (wasSpawned) {
        SLNet::BitStream bs;
        bs.Write(static_cast<SLNet::MessageID>(ID_DESTROY_NET_PLAYER));
        bs.Write(networkId);
        RelayExcept(networkId, bs, HIGH_PRIORITY, RELIABLE_ORDERED);
    }

    Log("player " + std::to_string(networkId) + " " + reason + "; " +
        std::to_string(players_.size()) + " connected");
}

void RelayServer::SendRoster(const Player& to) {
    std::vector<const Player*> others;
    for (const Player& p : players_) {
        if (p.networkId == to.networkId || !p.spawned) continue;
        others.push_back(&p);
    }
    if (others.empty()) return;

    SLNet::BitStream bs;
    bs.Write(static_cast<SLNet::MessageID>(ID_CREATE_NET_PLAYERS));
    bs.Write(static_cast<int>(others.size()));
    for (const Player* p : others) {
        bs.Write(p->networkId);
        bs.Write(p->defGlobalIndex);
        bs.Write(p->position);
    }
    SendTo(to.address, bs, HIGH_PRIORITY, RELIABLE_ORDERED);
}

void RelayServer::RelayExcept(int exceptNetworkId, const SLNet::BitStream& bs,
                              PacketPriority priority, PacketReliability reliability) {
    for (const Player& p : players_) {
        if (p.networkId == exceptNetworkId || !p.spawned) continue;
        peer_->Send(reinterpret_cast<const char*>(bs.GetData()),
                    static_cast<int>(bs.GetNumberOfBytesUsed()),
                    priority, reliability, 0, p.address, false);
    }
}

void RelayServer::SendTo(const SLNet::SystemAddress& address, const SLNet::BitStream& bs,
                         PacketPriority priority, PacketReliability reliability) {
    peer_->Send(reinterpret_cast<const char*>(bs.GetData()),
                static_cast<int>(bs.GetNumberOfBytesUsed()),
                priority, reliability, 0, address, false);
}

Player* RelayServer::FindByAddress(const SLNet::SystemAddress& address) {
    for (Player& p : players_) {
        if (p.address == address) return &p;
    }
    return nullptr;
}

Player* RelayServer::FindByNetworkId(int networkId) {
    for (Player& p : players_) {
        if (p.networkId == networkId) return &p;
    }
    return nullptr;
}

int RelayServer::AllocateNetworkId() {
    // Lowest free id at or above 1; id 0 stays reserved for the server itself.
    for (int candidate = kFirstClientNetworkId;; ++candidate) {
        bool used = false;
        for (const Player& p : players_) {
            if (p.networkId == candidate) { used = true; break; }
        }
        if (!used) return candidate;
    }
}

const Player* RelayServer::AnchorPlayer() const {
    const Player* best = nullptr;
    for (const Player& p : players_) {
        if (!p.spawned) continue;
        if (!best || p.networkId < best->networkId) best = &p;
    }
    return best;
}

Vec3 RelayServer::SpawnPositionFor() const {
    if (config_.spawnMode == Config::SpawnMode::Anchor) {
        if (const Player* anchor = AnchorPlayer()) return anchor->position;
        if (persistedAnchorKnown_ && !config_.spawnConfigured) return persistedAnchor_;
    }
    return config_.spawn;
}

bool RelayServer::AcceptRate(Player& player, int64_t nowMs) {
    if (nowMs - player.rateWindowStartMs >= 1000) {
        if (player.rateLimited) {
            Log("player " + std::to_string(player.networkId) + " exceeded " +
                std::to_string(kMaxPacketsPerSecond) + " packets/s (" +
                std::to_string(player.rateWindowCount) + "), traffic was dropped");
        }
        player.rateWindowStartMs = nowMs;
        player.rateWindowCount = 0;
        player.rateLimited = false;
    }
    if (++player.rateWindowCount > kMaxPacketsPerSecond) {
        player.rateLimited = true;
        return false;
    }
    return true;
}

void RelayServer::PersistAnchor() {
    if (config_.stateFile.empty()) return;

    Vec3 anchor{};
    {
        // Caller may or may not hold the lock; take a copy under a short one.
        std::lock_guard<std::mutex> lock(mutex_);
        const Player* a = AnchorPlayer();
        if (!a) return;
        anchor = a->position;
    }

    std::ofstream out(config_.stateFile, std::ios::trunc);
    if (!out) return; // no volume mounted -- persistence is best-effort
    out << anchor.x << " " << anchor.y << " " << anchor.z << "\n";
    out.close();

    std::lock_guard<std::mutex> lock(mutex_);
    persistedAnchor_ = anchor;
    persistedAnchorKnown_ = true;
}

void RelayServer::LoadPersistedAnchor() {
    if (config_.stateFile.empty()) return;
    std::ifstream in(config_.stateFile);
    if (!in) return;
    Vec3 v{};
    if (in >> v.x >> v.y >> v.z) {
        persistedAnchor_ = v;
        persistedAnchorKnown_ = true;
        Log("restored rendezvous point " + VecStr(v) + " from " + config_.stateFile);
    }
}

StatusSnapshot RelayServer::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    StatusSnapshot s;
    s.running = peer_ != nullptr;
    s.maxPlayers = config_.maxPlayers;
    s.gamePort = config_.gamePort;
    s.uptimeMs = NowMs() - startedAtMs_;
    s.playerCount = static_cast<int>(players_.size());

    const Player* anchor = AnchorPlayer();
    if (anchor) {
        s.anchor = anchor->position;
        s.anchorKnown = true;
    } else if (persistedAnchorKnown_) {
        s.anchor = persistedAnchor_;
        s.anchorKnown = true;
    }

    const int64_t now = NowMs();
    for (const Player& p : players_) {
        s.players.push_back({p.networkId, p.address.ToString(true), p.spawned,
                             p.position, now - p.connectedAtMs});
    }
    return s;
}

} // namespace crossover
