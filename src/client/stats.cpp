#include "stats.h"

#include <algorithm>
#include <cctype>
#include <string.h>

namespace grimnumbers {
namespace {

// Game time is a 32-bit millisecond counter; the subtraction wraps correctly
// as long as two events are less than ~24 days apart.
inline uint32_t Elapsed(uint32_t later, uint32_t earlier) { return later - earlier; }

struct Guard {
    CRITICAL_SECTION* cs;
    explicit Guard(CRITICAL_SECTION& c) : cs(&c) { EnterCriticalSection(cs); }
    ~Guard() { LeaveCriticalSection(cs); }
};

} // namespace

std::string ShortName(const char* raw)
{
    if (!raw || !*raw) return std::string();
    std::string s(raw);

    size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);

    if (s.size() > 4) {
        std::string tail = s.substr(s.size() - 4);
        for (char& c : tail) c = (char)tolower((unsigned char)c);
        if (tail == ".dbr") s = s.substr(0, s.size() - 4);
    }
    return s.empty() ? std::string(raw) : s;
}

Stats::Stats(double windowSec) : windowSec_(windowSec < 0.5 ? 0.5 : windowSec)
{
    InitializeCriticalSection(&lock_);
}

Stats::~Stats() { DeleteCriticalSection(&lock_); }

void Stats::SetWindowSec(double sec)
{
    Guard g(lock_);
    windowSec_ = sec < 0.5 ? 0.5 : sec;
}

void Stats::SetConnected(bool connected)
{
    Guard g(lock_);
    connected_ = connected;
}

void Stats::Reset()
{
    Guard g(lock_);
    const double keepWindow = windowSec_;
    const bool   keepConn   = connected_;

    window_.clear();            takenWindow_.clear();
    windowDamage_ = 0.0;        takenWindowDamage_ = 0.0;
    totalDamage_  = 0.0;        totalTaken_   = 0.0;
    totalHits_ = critHits_ = 0; takenHits_ = takenTicks_ = 0;
    critDamage_ = 0.0;
    biggestHit_ = biggestTaken_ = 0.0f;
    firstTimeMs_ = lastTimeMs_ = nowMs_ = 0;
    started_ = false;
    expectedSeq_ = 0;
    gaps_ = serverDropped_ = dropped5s_ = dropped60s_ = elemDropped_ = 0;
    targets_.clear();
    elements_.clear();
    playerId_ = 0;
    playerName_.clear();
    gamePid_ = 0;
    rawEvents_.clear();

    windowSec_ = keepWindow;
    connected_ = keepConn;
}

ElemRow& Stats::Elem(uint32_t type)
{
    for (ElemRow& e : elements_)
        if (e.type == type) return e;
    elements_.push_back(ElemRow{type, 0.0, 0.0, 0});
    return elements_.back();
}

void Stats::Trim(std::deque<Hit>& q, double& sum, bool applyElems)
{
    const uint32_t windowMs = (uint32_t)(windowSec_ * 1000.0);
    while (!q.empty() && Elapsed(nowMs_, q.front().timeMs) > windowMs) {
        const Hit& h = q.front();
        sum -= h.damage;
        if (applyElems) {
            for (uint8_t i = 0; i < h.elemCount; ++i) {
                ElemRow& e = Elem(h.elems[i].type);
                e.window -= h.elems[i].amount;
                if (e.window < 0.0) e.window = 0.0;
            }
        }
        q.pop_front();
    }
    if (sum < 0.0) sum = 0.0;
}

void Stats::TrimAll()
{
    Trim(window_,      windowDamage_,      /*applyElems=*/true);
    Trim(takenWindow_, takenWindowDamage_, /*applyElems=*/false);
}

void Stats::OnDamage(const GmDamage& d)
{
    if (!started_) { started_ = true; firstTimeMs_ = d.gameTimeMs; }
    lastTimeMs_ = d.gameTimeMs;
    nowMs_      = d.gameTimeMs;

    Hit h = {};
    h.timeMs    = d.gameTimeMs;
    h.damage    = d.totalDamage;
    h.elemCount = d.elemCount > GN_MAX_ELEM ? GN_MAX_ELEM : d.elemCount;
    memcpy(h.elems, d.elems, sizeof(h.elems));

    if (d.elemDropped) elemDropped_++;

    if (GN_IS_OUTGOING(d.source)) {
        window_.push_back(h);
        windowDamage_ += d.totalDamage;

        totalDamage_ += d.totalDamage;
        totalHits_++;
        if (d.isCrit) { critHits_++; critDamage_ += d.totalDamage; }
        if (d.totalDamage > biggestHit_) biggestHit_ = d.totalDamage;

        for (uint8_t i = 0; i < h.elemCount; ++i) {
            ElemRow& e = Elem(h.elems[i].type);
            e.total  += h.elems[i].amount;
            e.window += h.elems[i].amount;
            e.hits++;
        }

        TargetRow& t = targets_[d.defenderId];
        t.total += d.totalDamage;
        t.hits++;
        if (t.name.empty() && d.otherName[0]) t.name = ShortName(d.otherName);
    } else if (d.source == GN_SRC_INCOMING) {
        takenWindow_.push_back(h);
        takenWindowDamage_ += d.totalDamage;

        totalTaken_ += d.totalDamage;
        if (d.aggregated) takenHits_++; else takenTicks_++;
        if (d.totalDamage > biggestTaken_) biggestTaken_ = d.totalDamage;
    }

    TrimAll();
}

void Stats::OnMessage(const GmAnyMsg& msg, uint32_t size)
{
    Guard g(lock_);

    // Heartbeats bypass the ring and so do not consume a sequence number;
    // counting them would fabricate gaps.
    if (msg.hdr.type != GN_MSG_HEARTBEAT) {
        if (expectedSeq_ && msg.hdr.seq > expectedSeq_)
            gaps_ += msg.hdr.seq - expectedSeq_;
        expectedSeq_ = msg.hdr.seq + 1;
    }

    switch (msg.hdr.type) {
    case GN_MSG_HELLO:
        if (size >= sizeof(GmHello)) gamePid_ = msg.hello.pid;
        break;

    case GN_MSG_DAMAGE:
        if (size >= sizeof(GmDamage)) {
            OnDamage(msg.damage);
            // Bounded: only the console client drains this, so cap it rather
            // than letting a GUI-only run grow it forever.
            if (rawEvents_.size() >= 512) rawEvents_.pop_front();
            rawEvents_.push_back(msg.damage);
        }
        break;

    case GN_MSG_PLAYER:
        if (size >= sizeof(GmPlayer)) {
            playerId_   = msg.player.playerId;
            playerName_ = ShortName(msg.player.name);
        }
        break;

    case GN_MSG_HEARTBEAT:
        if (size >= sizeof(GmHeartbeat)) {
            serverDropped_ = msg.heartbeat.dropped;
            dropped5s_     = msg.heartbeat.dropped5s;
            dropped60s_    = msg.heartbeat.dropped60s;
            // Only "now" advances. lastTimeMs_ stays at the last hit so
            // session DPS keeps spanning the fight instead of decaying while
            // the player stands still.
            if (started_) { nowMs_ = msg.heartbeat.gameTimeMs; TrimAll(); }
        }
        break;

    default:
        break;
    }
}

bool Stats::PopRawEvent(GmDamage& out)
{
    Guard g(lock_);
    if (rawEvents_.empty()) return false;
    out = rawEvents_.front();
    rawEvents_.pop_front();
    return true;
}

Snapshot Stats::Take()
{
    Guard g(lock_);
    if (started_) TrimAll();

    Snapshot s;
    s.connected  = connected_;
    s.gamePid    = gamePid_;
    s.playerId   = playerId_;
    s.playerName = playerName_;

    s.windowSec   = windowSec_;
    s.rollingDps  = windowDamage_ / windowSec_;
    s.spanSec     = started_ ? Elapsed(lastTimeMs_, firstTimeMs_) / 1000.0 : 0.0;
    s.sessionDps  = s.spanSec > 0.1 ? totalDamage_ / s.spanSec : 0.0;
    s.totalDamage = totalDamage_;
    s.totalHits   = totalHits_;
    s.critHits    = critHits_;
    s.critDamage  = critDamage_;
    s.critRate    = totalHits_ ? 100.0 * (double)critHits_ / (double)totalHits_ : 0.0;
    s.biggestHit  = biggestHit_;

    s.rollingDtps  = takenWindowDamage_ / windowSec_;
    s.totalTaken   = totalTaken_;
    s.takenHits    = takenHits_;
    s.takenTicks   = takenTicks_;
    s.biggestTaken = biggestTaken_;

    s.gaps          = gaps_;
    s.serverDropped = serverDropped_;
    s.dropped5s     = dropped5s_;
    s.dropped60s    = dropped60s_;
    s.elemDropped   = elemDropped_;

    s.targets.reserve(targets_.size());
    for (const auto& kv : targets_) s.targets.push_back(kv.second);
    std::sort(s.targets.begin(), s.targets.end(),
              [](const TargetRow& a, const TargetRow& b) { return a.total > b.total; });

    s.elements = elements_;
    std::sort(s.elements.begin(), s.elements.end(),
              [](const ElemRow& a, const ElemRow& b) { return a.total > b.total; });

    return s;
}

} // namespace grimnumbers
