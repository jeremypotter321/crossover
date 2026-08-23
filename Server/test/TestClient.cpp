// Headless stand-in for Fable + EgoMP.dll.
//
// Reproduces precisely the client half of NetMainGameComponent /
// NetPlayerManager: connect, announce, accept an assigned network id, report a
// spawn, then stream movement and rotation -- and log every roster event the
// server sends back. This is what lets the dedicated server be verified end to
// end on a machine that cannot build the mod.
//
// Output lines beginning with "EVENT " are stable and meant to be asserted on.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <slikenet/peerinterface.h>
#include <slikenet/types.h>

#include "Protocol.h"

using namespace crossover;

namespace {

std::string Name;

void Event(const std::string& line) {
    std::printf("EVENT [%s] %s\n", Name.c_str(), line.c_str());
    std::fflush(stdout);
}

void Info(const std::string& line) {
    std::printf("      [%s] %s\n", Name.c_str(), line.c_str());
    std::fflush(stdout);
}

std::string V(const Vec3& v) {
    char b[80];
    std::snprintf(b, sizeof(b), "(%.2f,%.2f,%.2f)", v.x, v.y, v.z);
    return b;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <host> <port> <name> [seconds=10] [x] [y] [z]\n", argv[0]);
        return 2;
    }

    const char* host = argv[1];
    const unsigned short port = static_cast<unsigned short>(std::atoi(argv[2]));
    Name = argv[3];
    const int seconds = (argc > 4) ? std::atoi(argv[4]) : 10;

    Vec3 myPosition{100.0f, 200.0f, 30.0f};
    if (argc > 7) {
        myPosition.x = std::stof(argv[5]);
        myPosition.y = std::stof(argv[6]);
        myPosition.z = std::stof(argv[7]);
    }

    // A stand-in for the hero's definition index; the server only relays it.
    const int defGlobalIndex = 4242;

    SLNet::RakPeerInterface* peer = SLNet::RakPeerInterface::GetInstance();
    SLNet::SocketDescriptor sd;
    sd.socketFamily = AF_INET;
    if (peer->Startup(1, &sd, 1) != SLNet::RAKNET_STARTED) {
        std::fprintf(stderr, "startup failed\n");
        return 1;
    }
    if (peer->Connect(host, port, nullptr, 0) != SLNet::CONNECTION_ATTEMPT_STARTED) {
        std::fprintf(stderr, "connect failed\n");
        return 1;
    }
    Info(std::string("connecting to ") + host + ":" + std::to_string(port));

    int myNetworkId = -1;
    bool spawned = false;
    int peersSeen = 0;
    int movementsReceived = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto nextTick = std::chrono::steady_clock::now();
    float phase = 0.0f;

    while (std::chrono::steady_clock::now() < deadline) {
        for (SLNet::Packet* p = peer->Receive(); p;
             peer->DeallocatePacket(p), p = peer->Receive()) {
            switch (p->data[0]) {
            case ID_CONNECTION_REQUEST_ACCEPTED: {
                Event("connected");
                SLNet::BitStream bs;
                bs.Write(static_cast<SLNet::MessageID>(ID_CONNECTION_NOTIFICATION));
                peer->Send(&bs, HIGH_PRIORITY, RELIABLE_ORDERED, 0, p->systemAddress, false);
                break;
            }

            case ID_CREATE_LOCAL_NET_PLAYER: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                Vec3 spawn{};
                bs.Read(myNetworkId);
                bs.Read(spawn);
                Event("assigned_id id=" + std::to_string(myNetworkId) + " spawn=" + V(spawn));

                // The mod teleports to the given spawn, then reports itself.
                myPosition = spawn;
                SLNet::BitStream out;
                out.Write(static_cast<SLNet::MessageID>(ID_CREATE_NET_PLAYER));
                out.Write(myNetworkId);
                out.Write(defGlobalIndex);
                out.Write(myPosition);
                peer->Send(reinterpret_cast<const char*>(out.GetData()),
                           static_cast<int>(out.GetNumberOfBytesUsed()),
                           HIGH_PRIORITY, RELIABLE_ORDERED, 0, p->systemAddress, false);
                spawned = true;
                Event("spawned at=" + V(myPosition));
                break;
            }

            case ID_CREATE_NET_PLAYER: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                int nid = -1, def = 0;
                Vec3 pos{};
                bs.Read(nid);
                bs.Read(def);
                bs.Read(pos);
                peersSeen++;
                Event("peer_joined id=" + std::to_string(nid) + " def=" + std::to_string(def) +
                      " pos=" + V(pos));
                break;
            }

            case ID_CREATE_NET_PLAYERS: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                int count = 0;
                bs.Read(count);
                Event("roster count=" + std::to_string(count));
                for (int i = 0; i < count; ++i) {
                    int nid = -1, def = 0;
                    Vec3 pos{};
                    bs.Read(nid);
                    bs.Read(def);
                    bs.Read(pos);
                    peersSeen++;
                    Event("roster_entry id=" + std::to_string(nid) + " def=" +
                          std::to_string(def) + " pos=" + V(pos));
                }
                break;
            }

            case ID_DESTROY_NET_PLAYER: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                int nid = -1;
                bs.Read(nid);
                Event("peer_left id=" + std::to_string(nid));
                break;
            }

            case ID_PLAYER_MOVEMENT: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                int nid = -1;
                Vec3 pos{}, accel{};
                bs.Read(nid);
                bs.Read(pos);
                bs.Read(accel);
                if (++movementsReceived <= 3) {
                    Event("peer_moved id=" + std::to_string(nid) + " pos=" + V(pos));
                }
                break;
            }

            case ID_PLAYER_ROTATION: {
                SLNet::BitStream bs(p->data, p->length, false);
                bs.IgnoreBytes(sizeof(SLNet::MessageID));
                int nid = -1;
                Vec3 up{}, fwd{};
                bs.Read(nid);
                bs.Read(up);
                bs.Read(fwd);
                break;
            }

            case ID_CONNECTION_ATTEMPT_FAILED:
                Event("connect_failed");
                break;
            case ID_NO_FREE_INCOMING_CONNECTIONS:
                Event("server_full");
                break;
            case ID_INCOMPATIBLE_PROTOCOL_VERSION:
                Event("incompatible_protocol");
                break;
            case ID_DISCONNECTION_NOTIFICATION:
            case ID_CONNECTION_LOST:
                Event("disconnected_by_server");
                break;
            default:
                Info("unhandled id " + std::to_string((int)p->data[0]));
                break;
            }
        }

        // Stream movement the way the mod does, once per physics resolve.
        const auto now = std::chrono::steady_clock::now();
        if (spawned && now >= nextTick) {
            nextTick = now + std::chrono::milliseconds(50);
            phase += 0.1f;
            Vec3 pos{myPosition.x + std::sin(phase) * 5.0f, myPosition.y, myPosition.z};
            Vec3 accel{std::cos(phase), 0.0f, 0.0f};

            SLNet::BitStream mv;
            mv.Write(static_cast<SLNet::MessageID>(ID_PLAYER_MOVEMENT));
            mv.Write(myNetworkId);
            mv.Write(pos);
            mv.Write(accel);
            peer->Send(reinterpret_cast<const char*>(mv.GetData()),
                       static_cast<int>(mv.GetNumberOfBytesUsed()),
                       HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0,
                       SLNet::UNASSIGNED_SYSTEM_ADDRESS, true);

            SLNet::BitStream rot;
            Vec3 up{0.0f, 0.0f, 1.0f};
            Vec3 fwd{std::cos(phase), std::sin(phase), 0.0f};
            rot.Write(static_cast<SLNet::MessageID>(ID_PLAYER_ROTATION));
            rot.Write(myNetworkId);
            rot.Write(up);
            rot.Write(fwd);
            peer->Send(reinterpret_cast<const char*>(rot.GetData()),
                       static_cast<int>(rot.GetNumberOfBytesUsed()),
                       HIGH_PRIORITY, UNRELIABLE_SEQUENCED, 0,
                       SLNet::UNASSIGNED_SYSTEM_ADDRESS, true);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Event("summary id=" + std::to_string(myNetworkId) +
          " peers_seen=" + std::to_string(peersSeen) +
          " movements_received=" + std::to_string(movementsReceived));

    peer->Shutdown(300);
    SLNet::RakPeerInterface::DestroyInstance(peer);
    return 0;
}
