#include "fields.h"
#include "strutil.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

namespace grimnumbers {
namespace {

std::string Fmt(const char* fmt, double v)
{
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, v);
    return buf;
}

// Built-in names for CombatAttributeType.
//
// Derived from GDDPSMeter by Lumak, MIT licensed, copyright (c) 2023:
// DPSMeter/srcrenderer/DetailDamage.cpp, AttribTypeDef[]. Transcribed in the
// same order and shortened for display. See LICENSE_GDDPSMeter.txt.
//
// The enum has no EnumConverter specialisation in the export table, so these
// are not recoverable from the binary - this table is GDDPSMeter's, which
// rendered per-type damage with it against a live game (see
// reference/GDDPSMeter/DPSMeter/srcrenderer/DetailDamage.cpp). It is observed,
// not inferred, but it was observed against an older build: if an id ever
// reads wrong, override it in elements.ini rather than editing this.
//
// Only the ids that actually carry damage are given a colour; the rest are
// here so an unexpected id still reads as something.
struct BuiltinType { const char* name; uint32_t rgb; };

const BuiltinType kBuiltinTypes[] = {
    {"unknown",            0},         //  0 - also means "missed"
    {"phys pierce",        0xC9B48A},  //  1
    {"physical",           0xD8D3C8},  //  2
    {"pierce ratio",       0},         //  3
    {"pierce",             0xC9B48A},  //  4
    {"cold",               0x5FB8E6},  //  5
    {"fire",               0xE2662C},  //  6
    {"poison/acid",        0x7FBF3F},  //  7
    {"lightning",          0xE8D14A},  //  8
    {"vitality",           0x8E4FB0},  //  9
    {"chaos",              0xA8333C},  // 10
    {"aether",             0xC77FE0},  // 11
    {"mana burn",          0x4F7FD0},  // 12
    {"disruption",         0x9A7FD0},  // 13
    {"percent cur life",   0xB03030},  // 14
    {"bleeding",           0xB03030},  // 15
    {"total speed",        0},         // 16
    {"attack speed",       0},         // 17
    {"spell cast speed",   0},         // 18
    {"run speed",          0},         // 19
    {"life leech",         0x5E8C4A},  // 20
    {"mana leech",         0x4F7FD0},  // 21
    {"offensive ability",  0},         // 22
    {"defensive ability",  0},         // 23
    {"offensive reduction",0},         // 24
    {"defensive reduction",0},         // 25
    {"fumble",             0},         // 26
    {"projectile fumble",  0},         // 27
    {"total dmg red %",    0},         // 28
    {"total dmg red abs",  0},         // 29
    {"total res red %",    0},         // 30
    {"total res red abs",  0},         // 31
    {"phys res red %",     0},         // 32
    {"phys res red abs",   0},         // 33
    {"elem res red %",     0},         // 34
    {"elem res red abs",   0},         // 35
    {"phys dmg red %",     0},         // 36
    {"elem dmg red %",     0},         // 37
    {"absorption prot",    0},         // 38
    {"absorption",         0},         // 39
    {"protection",         0},         // 40
    {"armor rating",       0},         // 41
    {"stun",               0},         // 42
    {"sleep",              0},         // 43
    {"trap",               0},         // 44
    {"freeze",             0},         // 45
    {"petrify",            0},         // 46
    {"immobilize",         0},         // 47
    {"knockdown",          0},         // 48
    {"take hit",           0},         // 49
    {"taunt",              0},         // 50
    {"convert",            0},         // 51
    {"fear",               0},         // 52
    {"confusion",          0},         // 53
    {"block modifier",     0},         // 54
    {"block amount mod",   0},         // 55
    {"reflect",            0},         // 56
    {"elemental group",    0xE8A040},  // 57
    {"total dmg modifier", 0},         // 58
    {"crit dmg modifier",  0},         // 59
    {"mana burn ratio",    0},         // 60
};

const BuiltinType* Builtin(uint32_t type)
{
    const size_t n = sizeof(kBuiltinTypes) / sizeof(kBuiltinTypes[0]);
    return type < n ? &kBuiltinTypes[type] : nullptr;
}

} // namespace

bool ElementTable::Load(const wchar_t* path)
{
    entries_.clear();

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;

        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(s.substr(0, eq));
        std::string val = Trim(s.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        char* end = nullptr;
        const unsigned long id = strtoul(key.c_str(), &end, 0);
        if (end == key.c_str() || !id) continue;   // 0 is a miss, not a type

        Entry e;
        e.type = (uint32_t)id;
        e.rgb  = 0xCCCCCC;

        const size_t comma = val.find(',');
        if (comma != std::string::npos) {
            const std::string rgb = Trim(val.substr(comma + 1));
            val = Trim(val.substr(0, comma));
            e.rgb = (uint32_t)(strtoul(rgb.c_str(), nullptr, 16) & 0xFFFFFFu);
        }
        e.label = val;
        entries_.push_back(e);
    }
    fclose(f);
    return true;
}

const ElementTable::Entry* ElementTable::Find(uint32_t type) const
{
    for (const Entry& e : entries_)
        if (e.type == type) return &e;
    return nullptr;
}

// elements.ini wins over the built-in table, so a name that turns out to be
// wrong for the current game build can be corrected without a rebuild.
std::string ElementTable::Label(uint32_t type) const
{
    if (const Entry* e = Find(type)) return e->label;
    if (const BuiltinType* b = Builtin(type)) return b->name;

    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "type %u", type);
    return buf;
}

uint32_t ElementTable::Color(uint32_t type, uint32_t fallback) const
{
    if (const Entry* e = Find(type)) return e->rgb;
    if (const BuiltinType* b = Builtin(type)) {
        if (b->rgb) return b->rgb;   // 0 means "not a damage type, no colour"
    }
    return fallback;
}

std::string FormatCount(double v)
{
    char raw[64];
    _snprintf_s(raw, sizeof(raw), _TRUNCATE, "%.0f", v < 0.0 ? 0.0 : v);

    std::string in(raw), out;
    int since = 0;
    for (size_t i = in.size(); i-- > 0; ) {
        out.push_back(in[i]);
        if (++since == 3 && i > 0) { out.push_back(','); since = 0; }
    }
    return std::string(out.rbegin(), out.rend());
}

std::string FormatShort(double v)
{
    if (v < 0.0) v = 0.0;
    if (v >= 1e9) return Fmt("%.2fB", v / 1e9);
    if (v >= 1e6) return Fmt("%.2fM", v / 1e6);
    if (v >= 1e4) return Fmt("%.1fk", v / 1e3);
    return Fmt("%.0f", v);
}

std::string FieldValue(const std::string& t, const Snapshot& s)
{
    // Rolling / session rates.
    if (t == "dps.rolling")   return FormatCount(s.rollingDps);
    if (t == "dps.rolling.s") return FormatShort(s.rollingDps);
    if (t == "dps.session")   return FormatCount(s.sessionDps);
    if (t == "dps.session.s") return FormatShort(s.sessionDps);
    if (t == "dps.span")      return Fmt("%.1f", s.spanSec);
    if (t == "window")        return Fmt("%.0f", s.windowSec);

    // Outgoing totals.
    if (t == "dmg.total")     return FormatCount(s.totalDamage);
    if (t == "dmg.total.s")   return FormatShort(s.totalDamage);
    if (t == "hits.total")    return FormatCount((double)s.totalHits);
    if (t == "crit.rate")     return Fmt("%.1f%%", s.critRate);
    if (t == "crit.hits")     return FormatCount((double)s.critHits);
    if (t == "crit.damage")   return FormatCount(s.critDamage);
    if (t == "crit.damage.s") return FormatShort(s.critDamage);
    if (t == "hit.biggest")   return FormatCount(s.biggestHit);
    if (t == "hit.biggest.s") return FormatShort(s.biggestHit);

    // Damage taken.
    if (t == "dtps.rolling")   return FormatCount(s.rollingDtps);
    if (t == "dtps.rolling.s") return FormatShort(s.rollingDtps);
    if (t == "taken.total")    return FormatCount(s.totalTaken);
    if (t == "taken.total.s")  return FormatShort(s.totalTaken);
    if (t == "taken.hits")     return FormatCount((double)s.takenHits);
    if (t == "taken.ticks")    return FormatCount((double)s.takenTicks);
    if (t == "taken.biggest")  return FormatCount(s.biggestTaken);

    // Link health.
    if (t == "drops.total") return FormatCount((double)s.serverDropped);
    if (t == "drops.5s")    return FormatCount((double)s.dropped5s);
    if (t == "drops.60s")   return FormatCount((double)s.dropped60s);
    if (t == "drops.gaps")  return FormatCount((double)s.gaps);
    if (t == "link")        return s.connected ? "live" : "waiting";

    // Identity. Deliberately available but not in the shipped overlay layout.
    if (t == "player.name") return s.playerName;
    if (t == "player.id")   return FormatCount((double)s.playerId);
    if (t == "game.pid")    return FormatCount((double)s.gamePid);

    return std::string();
}

std::string ExpandTokens(const std::string& text, const Snapshot& s)
{
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ) {
        if (text[i] != '{') { out.push_back(text[i++]); continue; }

        const size_t close = text.find('}', i + 1);
        if (close == std::string::npos) { out.append(text, i, std::string::npos); break; }

        out += FieldValue(text.substr(i + 1, close - i - 1), s);
        i = close + 1;
    }
    return out;
}

} // namespace grimnumbers
