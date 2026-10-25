// Wire format shared by the in-process hook DLL and the out-of-process client.
// POD only, fixed layout, little-endian. Both ends are x64 MSVC.
#pragma once

#include <stdint.h>

#define GN_PIPE_NAME   L"\\\\.\\pipe\\GrimNumbers"
#define GN_PROTO_MAGIC 0x4D554E47u /* 'GNUM' */
#define GN_PROTO_VER   4u
#define GN_NAME_MAX    48
#define GN_MAX_ELEM    8

#pragma pack(push, 4)

enum GmMsgType : uint16_t {
    GN_MSG_HELLO     = 1,  // server -> client, once per connection
    GN_MSG_DAMAGE    = 2,  // one committed damage event
    GN_MSG_DEATH     = 3,  // a character died
    GN_MSG_PLAYER    = 4,  // player identity became known / changed
    GN_MSG_HEARTBEAT = 5,  // idle tick; see GmHeartbeat
};

// Source of a damage event, so the client can split player vs pet vs incoming.
enum GmSource : uint8_t {
    GN_SRC_UNKNOWN  = 0,
    GN_SRC_PLAYER   = 1,
    GN_SRC_PET      = 2,
    GN_SRC_INCOMING = 3,  // damage taken by the player
};

// True for damage the player dealt, which is what DPS is computed from.
#define GN_IS_OUTGOING(src) ((src) == GN_SRC_PLAYER || (src) == GN_SRC_PET)

// One damage-type component of a hit. `type` is the game's raw
// CombatAttributeType value as it arrives at CombatManager::ApplyDamage.
//
// Those ids are NOT nameable from the export table - there is no
// EnumConverter specialisation for CombatAttributeType - so the
// wire format forwards the number and leaves naming to the client, which maps
// ids to labels through a config file the user fills in by observation.
// Anything unmapped is shown as "type N" rather than guessed at.
struct GmElem {
    uint32_t type;      // raw CombatAttributeType, never 0 (0 means a miss)
    float    amount;    // ceil'd, as the game displays it
};

struct GmHeader {
    uint32_t magic;     // GN_PROTO_MAGIC
    uint16_t version;   // GN_PROTO_VER
    uint16_t type;      // GmMsgType
    uint32_t size;      // total bytes of this message, header included
    uint32_t seq;       // monotonic per connection; gaps mean the ring dropped
};

struct GmHello {
    GmHeader hdr;
    uint32_t pid;
    uint32_t gameTimeMs;      // GAME::GetGameTime() at connect
    char     gameBuild[32];   // Game.dll timestamp, for sanity checks
};

// One committed hit. "Committed" means the game finished aggregating the
// per-damage-type components and drew combat text for it, so totalDamage is
// the number the player actually sees.
struct GmDamage {
    GmHeader hdr;
    uint32_t gameTimeMs;      // GAME::GetGameTime()
    uint32_t attackerId;      // Object::GetObjectId of the attacker
    uint32_t defenderId;      // Object::GetObjectId of the victim
    uint32_t skillId;         // skill that produced the hit, 0 if unattributed
    float    totalDamage;     // sum over damage-type components
    uint8_t  source;          // GmSource
    uint8_t  isCrit;          // 1 if the crit combat-text path fired
    uint8_t  componentCount;  // number of ApplyDamage components folded in
    uint8_t  aggregated;      // 1 = one visible hit; 0 = a single unscoped
                              //     component emitted on its own (DoT tick)
    // The party that is not the player: the victim for outgoing damage, the
    // attacker for incoming. May be empty when the name was never seen.
    char     otherName[GN_NAME_MAX];

    // Per-damage-type split of totalDamage. Components of the same type are
    // summed into one entry. Overflow past GN_MAX_ELEM distinct types is
    // counted in elemDropped and still included in totalDamage, so the split
    // can be short of the total but never exceeds it.
    uint8_t  elemCount;
    uint8_t  elemDropped;
    uint16_t _pad0;
    GmElem   elems[GN_MAX_ELEM];
};

struct GmDeath {
    GmHeader hdr;
    uint32_t gameTimeMs;
    uint32_t characterId;
    char     name[GN_NAME_MAX];
};

struct GmPlayer {
    GmHeader hdr;
    uint32_t gameTimeMs;
    uint32_t playerId;
    char     name[GN_NAME_MAX]; // portrait/char name if resolvable
};

// Sent every 250 ms, idle or not. It exists for two reasons beyond keeping
// the link warm:
//
//  - The server only discovers that a client has gone away when a write
//    fails. Without traffic it would never notice, and a newly started client
//    could not connect until the next hit landed.
//  - The client only redraws when something arrives, so with no combat its
//    rolling DPS would freeze at the last value instead of decaying to zero.
struct GmHeartbeat {
    GmHeader hdr;
    uint32_t gameTimeMs;
    uint32_t _pad;
    // A session total is nearly useless on its own: a burst while the ring had
    // no reader taints every reading that follows. The windowed counts say
    // whether the link is losing events *now*, which is what matters for the
    // DPS figure on screen. All three are zeroed when a client connects.
    uint64_t dropped;     // since this client connected
    uint64_t dropped5s;   // in the last 5 seconds
    uint64_t dropped60s;  // in the last 60 seconds
};

// Largest message, used to size client read buffers.
union GmAnyMsg {
    GmHeader hdr;
    GmHello  hello;
    GmDamage damage;
    GmDeath  death;
    GmPlayer player;
    GmHeartbeat heartbeat;
};

#pragma pack(pop)
