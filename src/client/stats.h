// Damage accounting shared by every front end (console, app window, overlay).
//
// A Stats object is fed raw protocol messages by the pipe reader thread and
// read by the UI thread, so it carries its own lock and hands out a Snapshot
// rather than letting callers walk its containers. Everything the UI needs is
// computed while the lock is held; nothing in Snapshot points back into Stats.
#pragma once

#include "grimnumbers/protocol.h"

#include <windows.h>
#include <stdint.h>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace grimnumbers {

// Names arrive as database paths ("records/creatures/pc/femalepc01.dbr").
// The directories are identical for every entity, so a fixed-width column
// would show nothing but the prefix. Keep the file stem.
std::string ShortName(const char* raw);

struct TargetRow {
    std::string name;
    double      total = 0.0;
    uint64_t    hits  = 0;
};

// One damage type's share. `type` is the game's raw CombatAttributeType; the
// label comes from the element table, not from here.
struct ElemRow {
    uint32_t type   = 0;
    double   total  = 0.0;   // session
    double   window = 0.0;   // rolling window, so an element DPS can decay
    uint64_t hits   = 0;
};

// Everything a front end draws, as of one instant. Plain values only.
struct Snapshot {
    bool        connected   = false;
    uint32_t    gamePid     = 0;
    uint32_t    playerId    = 0;
    std::string playerName;

    double   windowSec    = 5.0;
    double   rollingDps   = 0.0;
    double   sessionDps   = 0.0;
    double   spanSec      = 0.0;
    double   totalDamage  = 0.0;
    uint64_t totalHits    = 0;
    uint64_t critHits     = 0;
    double   critDamage   = 0.0;
    double   critRate     = 0.0;   // percent
    double   biggestHit   = 0.0;

    double   rollingDtps  = 0.0;
    double   totalTaken   = 0.0;
    uint64_t takenHits    = 0;
    uint64_t takenTicks   = 0;
    double   biggestTaken = 0.0;

    uint64_t gaps          = 0;
    uint64_t serverDropped = 0;
    uint64_t dropped5s     = 0;
    uint64_t dropped60s    = 0;
    uint64_t elemDropped   = 0;    // hits whose split overflowed GN_MAX_ELEM

    // Sorted by session total, descending.
    std::vector<TargetRow> targets;
    std::vector<ElemRow>   elements;   // outgoing damage only
};

class Stats {
public:
    explicit Stats(double windowSec = 5.0);
    ~Stats();

    Stats(const Stats&)            = delete;
    Stats& operator=(const Stats&) = delete;

    // Fed from the reader thread. `size` is the record's own size, so a
    // message shorter than its struct is ignored rather than read past.
    void OnMessage(const GmAnyMsg& msg, uint32_t size);

    // Link state, for the "waiting for Grim Dawn" case.
    void SetConnected(bool connected);

    // Drops everything but the window length. Called on reconnect.
    void Reset();

    // Advances the rolling window to the newest known game time and returns a
    // consistent view. Safe to call at any rate from any thread.
    Snapshot Take();

    double WindowSec() const { return windowSec_; }
    void   SetWindowSec(double sec);

    // Oldest unread damage event, for the console client's raw line. Returns
    // false once the queue is empty. The queue is bounded: a front end that
    // never drains it costs memory, not an unbounded leak.
    bool PopRawEvent(GmDamage& out);

private:
    struct Hit {
        uint32_t timeMs;
        float    damage;
        uint8_t  elemCount;
        GmElem   elems[GN_MAX_ELEM];
    };

    void OnDamage(const GmDamage& d);
    void Trim(std::deque<Hit>& q, double& sum, bool applyElems);
    void TrimAll();
    ElemRow& Elem(uint32_t type);

    mutable CRITICAL_SECTION lock_;
    double   windowSec_;

    bool     connected_ = false;
    uint32_t gamePid_   = 0;
    uint32_t playerId_  = 0;
    std::string playerName_;

    std::deque<Hit> window_;
    double   windowDamage_ = 0.0;
    double   totalDamage_  = 0.0;
    uint64_t totalHits_    = 0;
    uint64_t critHits_     = 0;
    double   critDamage_   = 0.0;
    float    biggestHit_   = 0.0f;

    std::deque<Hit> takenWindow_;
    double   takenWindowDamage_ = 0.0;
    double   totalTaken_   = 0.0;
    uint64_t takenHits_    = 0;
    uint64_t takenTicks_   = 0;
    float    biggestTaken_ = 0.0f;

    uint32_t firstTimeMs_ = 0;
    uint32_t lastTimeMs_  = 0;   // last DAMAGE; bounds session DPS
    uint32_t nowMs_       = 0;   // latest tick; drives the rolling window
    bool     started_     = false;

    uint32_t expectedSeq_   = 0;
    uint64_t gaps_          = 0;
    uint64_t serverDropped_ = 0;
    uint64_t dropped5s_     = 0;
    uint64_t dropped60s_    = 0;
    uint64_t elemDropped_   = 0;

    std::map<uint32_t, TargetRow> targets_;
    std::vector<ElemRow>          elements_;

    std::deque<GmDamage> rawEvents_;
};

} // namespace grimnumbers
