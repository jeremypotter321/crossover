#pragma once

// Crossover dedicated-server wire protocol.
//
// Mirrors Core/Multiplayer/Network/NetworkMessages.h and the BitStream reads and
// writes in Core/Multiplayer/Fable/. It is duplicated rather than shared because the
// mod is an MSVC/Win32 build and this server is Linux -- if you change one, change both.
//
// ---------------------------------------------------------------------------
// Encoding: two things here are easy to get wrong and silently corrupt the wire.
//
// 1. SLikeNet 0.1.3 ships with __BITSTREAM_NATIVE_END *undefined* (see defines.h:61),
//    so BitStream::DoEndianSwap() returns `IsNetworkOrder() == false`, which on x86
//    is TRUE. Everything wider than one byte is therefore byte-reversed on the wire:
//    ints and floats travel BIG-ENDIAN, not little-endian.
//
// 2. BitStream::Write<T> reverses `sizeof(T)` bytes AS ONE BLOCK. For a 12-byte
//    C3DVector that reverses the whole struct, which flips the *field order* too:
//
//        in memory (LE):  x0 x1 x2 x3  y0 y1 y2 y3  z0 z1 z2 z3
//        on the wire:     z3 z2 z1 z0  y3 y2 y1 y0  x3 x2 x1 x0
//                         \__BE(z)__/  \__BE(y)__/  \__BE(x)__/
//
//    So a vector is BE(z), BE(y), BE(x) -- Z FIRST. Writing three separate floats
//    would produce BE(x), BE(y), BE(z) and put every player in the wrong place.
//
// The safe way to stay compatible is to never hand-roll this: use SLNet::BitStream
// with a POD of identical layout, so the exact same template runs on both ends.
// ProtocolSelfTest() in RelayServer.cpp asserts the layout above at startup.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <type_traits>

#include <slikenet/MessageIdentifiers.h>
#include <slikenet/BitStream.h>

namespace crossover {

// ID_USER_PACKET_ENUM is 134 in SLikeNet 0.1.3.
enum NetworkMessages : unsigned char {
    ID_CONNECTION_NOTIFICATION = ID_USER_PACKET_ENUM, // 134  client -> server
    ID_CREATE_LOCAL_NET_PLAYER,                       // 135  server -> one client
    ID_CREATE_NET_PLAYER,                             // 136  both directions
    ID_CREATE_NET_PLAYERS,                            // 137  server -> one client
    ID_DESTROY_NET_PLAYER,                            // 138  server -> clients
    ID_PLAYER_MOVEMENT,                               // 139  both directions
    ID_PLAYER_ROTATION                                // 140  both directions
};

// Layout-compatible stand-in for Fable's C3DVector (three floats, no vtable).
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
static_assert(sizeof(Vec3) == 12, "Vec3 must match C3DVector's 12-byte layout");
static_assert(std::is_trivially_copyable<Vec3>::value, "Vec3 must be trivially copyable");

// Exact wire sizes, used to reject malformed packets before reading them.
constexpr int kMsgIdBytes = 1;
constexpr int kIntBytes   = 4;
constexpr int kVec3Bytes  = 12;

constexpr int kSizeConnectionNotification = kMsgIdBytes;                              //  1
constexpr int kSizeCreateLocalNetPlayer   = kMsgIdBytes + kIntBytes + kVec3Bytes;     // 17
constexpr int kSizeCreateNetPlayer        = kMsgIdBytes + kIntBytes * 2 + kVec3Bytes; // 21
constexpr int kSizeCreateNetPlayersHeader = kMsgIdBytes + kIntBytes;                  //  5
constexpr int kSizeCreateNetPlayersEntry  = kIntBytes * 2 + kVec3Bytes;               // 20
constexpr int kSizeDestroyNetPlayer       = kMsgIdBytes + kIntBytes;                  //  5
constexpr int kSizePlayerMovement         = kMsgIdBytes + kIntBytes + kVec3Bytes * 2; // 29
constexpr int kSizePlayerRotation         = kMsgIdBytes + kIntBytes + kVec3Bytes * 2; // 29

// The mod reserves network id 0 for whoever is hosting. A dedicated server holds that
// id without ever owning an avatar, so clients start at 1 and no client ever believes
// it is the host (which is what stops it trying to relay on the server's behalf).
constexpr int kServerNetworkId = 0;
constexpr int kFirstClientNetworkId = 1;

} // namespace crossover
