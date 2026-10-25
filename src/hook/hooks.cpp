// Damage accounting, modelled on GDDPSMeter's detour flow but rebuilt against
// the x64 exports.
//
// How a hit reaches us:
//
//   Character::AttackTarget(skillId, Entity& target, ...)
//       opens an attack. `this` is the attacker, so this is where attacker
//       identity, defender identity and skill id are known.
//
//   CombatManager::ApplyDamage(amount, PlayStatsDamageType&, attrType, skills)
//       fires once per damage-type component. `this` is the *defender's*
//       CombatManager, so GetAttackerId() names the attacker and
//       GetCharacter() names the victim. attrType == 0 is a miss.
//       Damage over time arrives here with no enclosing AttackTarget.
//
//   Player::RegisterCombatTextHit(total) / RegisterCombatTextCrit(total)
//       the commit point. The game has finished aggregating components and is
//       about to draw the number the player sees. Crit is signalled purely by
//       which of the two fires - there is no boolean to read.
//
// So we accumulate components between AttackTarget/ApplyDamage and flush on
// the combat-text call, tagging crit from the callee.

#include "hooks.h"
#include "gd_symbols.h"
#include "pipe_server.h"
#include "log.h"
#include "grimnumbers/protocol.h"

#include <MinHook.h>

#include <windows.h>
#include <string.h>
#include <math.h>

using namespace gd;

namespace grimnumbers {
namespace {

constexpr size_t kMaxPets    = 64;
constexpr size_t kMaxPending = 32;
constexpr size_t kMaxNames   = 128;

// One in-flight attack: components accumulated, not yet committed.
struct Pending {
    bool     active;
    uint32_t attackerId;
    uint32_t defenderId;
    uint32_t skillId;
    float    total;
    uint8_t  components;
    uint8_t  source;        // GmSource
    char     otherName[GN_NAME_MAX];

    // Per-damage-type split, summed by CombatAttributeType.
    GmElem   elems[GN_MAX_ELEM];
    uint8_t  elemCount;
    uint8_t  elemDropped;
};

// id -> name, filled opportunistically whenever we hold an Object*. Incoming
// damage only yields the attacker's *id* (CombatManager::GetAttackerId), and
// the export table has no id->Entity lookup that does not need UniqueIdMap and
// its layout, so names are remembered from Character::AttackTarget instead,
// where the attacker is a live pointer.
struct NameEntry {
    uint32_t id;
    char     name[GN_NAME_MAX];
};

struct State {
    CRITICAL_SECTION lock;

    Player*  playerPtr;
    uint32_t playerId;

    uint32_t pets[kMaxPets];
    size_t   petCount;

    // The attack opened by the most recent AttackTarget. Grim Dawn drives
    // combat from a single logic thread, so one slot is enough; the lock
    // guards against other threads reading it.
    Pending  direct;

    // Damage with no enclosing AttackTarget (DoT, retaliation, procs),
    // bucketed by skill id until a combat-text call commits them.
    Pending  loose[kMaxPending];

    // The incoming attack opened by CombatManager::TakeAttack, if any.
    Pending  incoming;

    NameEntry names[kMaxNames];
    size_t    nameNext;

    uint64_t emitted;
};

State g_st = {};

// Teardown guards. HooksRemove runs on paths where install never completed, so
// it must know what actually got set up.
bool g_csReady = false;
bool g_mhReady = false;

// Trampolines to the original functions.
Fn_Character_AttackTarget     o_AttackTarget     = nullptr;
Fn_CombatManager_ApplyDamage  o_ApplyDamage      = nullptr;
Fn_CombatManager_TakeAttack   o_TakeAttack       = nullptr;
Fn_Player_RegisterCombatText  o_CombatTextHit    = nullptr;
Fn_Player_RegisterCombatText  o_CombatTextCrit   = nullptr;
Fn_Player_UpdateSelf          o_UpdateSelf       = nullptr;
Fn_Player_PostPetSpawn        o_PostPetSpawn     = nullptr;
Fn_Character_CharacterIsDying o_CharacterIsDying = nullptr;

bool IsPet(uint32_t id)
{
    if (!id) return false;
    for (size_t i = 0; i < g_st.petCount; ++i)
        if (g_st.pets[i] == id) return true;
    return false;
}

void ClearPending(Pending& p)
{
    memset(&p, 0, sizeof(p));
}

// Folds one component into a pending hit's damage-type split. Components of
// the same type are summed, so a hit with three fire ticks shows one fire
// entry. Past GN_MAX_ELEM distinct types the amount is only counted, which
// keeps the split a subset of the total instead of silently rewriting it.
void AddElem(Pending& p, uint32_t type, float amount)
{
    if (!type) return;                 // 0 is a miss, never a damage type
    for (uint8_t i = 0; i < p.elemCount; ++i) {
        if (p.elems[i].type == type) { p.elems[i].amount += amount; return; }
    }
    if (p.elemCount >= GN_MAX_ELEM) {
        if (p.elemDropped < 255) p.elemDropped++;
        return;
    }
    p.elems[p.elemCount].type   = type;
    p.elems[p.elemCount].amount = amount;
    p.elemCount++;
}

// Caller holds the lock.
void RememberName(uint32_t id, const void* obj)
{
    if (!id || !obj) return;
    for (size_t i = 0; i < kMaxNames; ++i)
        if (g_st.names[i].id == id) return;      // already known

    NameEntry& e = g_st.names[g_st.nameNext % kMaxNames];
    g_st.nameNext++;
    e.id = id;
    ObjectName(obj, e.name, sizeof(e.name));
}

// Caller holds the lock.
void LookupName(uint32_t id, char* out, size_t outSize)
{
    if (!outSize) return;
    out[0] = '\0';
    if (!id) return;
    for (size_t i = 0; i < kMaxNames; ++i) {
        if (g_st.names[i].id == id) {
            strncpy_s(out, outSize, g_st.names[i].name, outSize - 1);
            return;
        }
    }
}

// Caller holds the lock. `aggregated` is false only for a lone unscoped
// component, which the client needs in order not to over-count hits.
void EmitDamage(const Pending& p, bool isCrit, bool aggregated = true)
{
    if (!p.active || p.total <= 0.0f) return;

    GmDamage m = {};
    m.gameTimeMs     = GameTimeMs();
    m.attackerId     = p.attackerId;
    m.defenderId     = p.defenderId;
    m.skillId        = p.skillId;
    m.totalDamage    = p.total;
    m.source         = p.source;
    m.isCrit         = isCrit ? 1 : 0;
    m.componentCount = p.components;
    m.aggregated     = aggregated ? 1 : 0;
    m.elemCount      = p.elemCount;
    m.elemDropped    = p.elemDropped;
    memcpy(m.elems, p.elems, sizeof(m.elems));

    if (p.otherName[0]) {
        strncpy_s(m.otherName, sizeof(m.otherName), p.otherName,
                  sizeof(m.otherName) - 1);
    } else {
        // For incoming damage the "other" party is the attacker, whose name we
        // only ever have via the cache.
        const uint32_t other = (p.source == GN_SRC_INCOMING)
                             ? p.attackerId : p.defenderId;
        LookupName(other, m.otherName, sizeof(m.otherName));
    }

    Publish(&m, GN_MSG_DAMAGE, sizeof(m));
    g_st.emitted++;
}

// Flushes everything accumulated and tags it crit or not. Called from both
// combat-text hooks.
void CommitCombatText(bool isCrit)
{
    EnterCriticalSection(&g_st.lock);

    if (g_st.direct.active) {
        EmitDamage(g_st.direct, isCrit);
        // Keep attacker/defender/skill: one attack can produce several
        // combat-text calls. Only the accumulator resets.
        g_st.direct.total = 0.0f;
        g_st.direct.components = 0;
        g_st.direct.elemCount = 0;
        g_st.direct.elemDropped = 0;
        memset(g_st.direct.elems, 0, sizeof(g_st.direct.elems));
    }

    for (size_t i = 0; i < kMaxPending; ++i) {
        if (g_st.loose[i].active) {
            EmitDamage(g_st.loose[i], isCrit);
            ClearPending(g_st.loose[i]);
        }
    }

    LeaveCriticalSection(&g_st.lock);
}

// ---- detours ------------------------------------------------------------

bool Hk_AttackTarget(Character* self, uint32_t skillId, Entity* target,
                     ParametersCombat* params, bool flag,
                     uint32_t directDmgType, uint32_t durationDmgType,
                     uint32_t damageMult)
{
    const uint32_t attackerId = ObjectId(self);
    const uint32_t defenderId = ObjectId(target);

    EnterCriticalSection(&g_st.lock);
    const bool mine = (attackerId != 0) &&
                      (attackerId == g_st.playerId || IsPet(attackerId));

    // Every attack passes through here with both parties as live pointers, so
    // this is the one place names can be learned for later id-only lookups.
    RememberName(attackerId, self);
    RememberName(defenderId, target);

    if (mine) {
        ClearPending(g_st.direct);
        g_st.direct.active     = true;
        g_st.direct.attackerId = attackerId;
        g_st.direct.defenderId = defenderId;
        g_st.direct.skillId    = skillId;
        g_st.direct.source     = (attackerId == g_st.playerId)
                               ? GN_SRC_PLAYER : GN_SRC_PET;
        ObjectName(target, g_st.direct.otherName,
                   sizeof(g_st.direct.otherName));
    }
    LeaveCriticalSection(&g_st.lock);

    bool r = o_AttackTarget(self, skillId, target, params, flag,
                            directDmgType, durationDmgType, damageMult);

    if (mine) {
        EnterCriticalSection(&g_st.lock);
        // The attack is over. The combat-text hook is what decides crit, so
        // leave a bucket that has damage in it alone and only drop the
        // "a direct attack is open" marker when nothing landed.
        if (g_st.direct.components == 0) ClearPending(g_st.direct);
        LeaveCriticalSection(&g_st.lock);
    }
    return r;
}

bool Hk_ApplyDamage(CombatManager* self, float amount,
                    const PlayStatsDamageType* stats, uint32_t combatAttrType,
                    const MemVector<uint32_t>* skills)
{
    // attrType 0 means the attack missed; the game still calls through.
    if (combatAttrType != 0 && amount > 0.0f && g_sym.GetAttackerId) {
        const uint32_t attackerId = g_sym.GetAttackerId(self);

        // `this` is always the defender's CombatManager, for outgoing and
        // incoming damage alike.
        Character* victim = g_sym.CombatManagerGetCharacter
                          ? g_sym.CombatManagerGetCharacter(self) : nullptr;
        const uint32_t defenderId = ObjectId(victim);

        uint32_t skillId = 0;
        if (skills && skills->size() > 0) skillId = (*skills)[0];

        // The game rounds each displayed component up.
        const float cf = ceilf(amount);

        EnterCriticalSection(&g_st.lock);
        RememberName(defenderId, victim);

        const bool fromMe = attackerId != 0 &&
                            (attackerId == g_st.playerId || IsPet(attackerId));
        // Only the player's own damage taken counts as incoming; pets dying is
        // not something a damage-taken readout should fold in.
        const bool toMe = !fromMe && defenderId != 0 &&
                          defenderId == g_st.playerId;

        if (fromMe) {
            if (g_st.direct.active && g_st.direct.attackerId == attackerId) {
                g_st.direct.total += cf;
                AddElem(g_st.direct, combatAttrType, cf);
                if (g_st.direct.components < 255) g_st.direct.components++;
                if (!g_st.direct.skillId) g_st.direct.skillId = skillId;
                if (!g_st.direct.defenderId) g_st.direct.defenderId = defenderId;
            } else {
                // DoT / proc / retaliation: bucket by skill id.
                Pending* slot = nullptr;
                for (size_t i = 0; i < kMaxPending; ++i) {
                    if (g_st.loose[i].active &&
                        g_st.loose[i].skillId == skillId &&
                        g_st.loose[i].attackerId == attackerId) {
                        slot = &g_st.loose[i];
                        break;
                    }
                }
                if (!slot) {
                    for (size_t i = 0; i < kMaxPending; ++i) {
                        if (!g_st.loose[i].active) { slot = &g_st.loose[i]; break; }
                    }
                    if (slot) {
                        ClearPending(*slot);
                        slot->active     = true;
                        slot->attackerId = attackerId;
                        slot->defenderId = defenderId;
                        slot->skillId    = skillId;
                        slot->source     = (attackerId == g_st.playerId)
                                         ? GN_SRC_PLAYER : GN_SRC_PET;
                        ObjectName(victim, slot->otherName,
                                   sizeof(slot->otherName));
                    }
                }
                if (slot) {
                    slot->total += cf;
                    AddElem(*slot, combatAttrType, cf);
                    if (slot->components < 255) slot->components++;
                }
            }
        } else if (toMe) {
            if (g_st.incoming.active) {
                // Inside a TakeAttack scope: fold into the one visible hit.
                g_st.incoming.total += cf;
                AddElem(g_st.incoming, combatAttrType, cf);
                if (g_st.incoming.components < 255) g_st.incoming.components++;
                if (!g_st.incoming.skillId) g_st.incoming.skillId = skillId;
                if (!g_st.incoming.attackerId) g_st.incoming.attackerId = attackerId;
            } else {
                // No enclosing attack: DurationDamageManager::ExecuteDamage and
                // TickManager::Tick call ApplyDamage directly, and nothing
                // later tells us the tick is finished. Emit the component on
                // its own and mark it un-aggregated so the client can keep its
                // hit count honest.
                Pending one = {};
                one.active     = true;
                one.attackerId = attackerId;
                one.defenderId = defenderId;
                one.skillId    = skillId;
                one.total      = cf;
                one.components = 1;
                one.source     = GN_SRC_INCOMING;
                AddElem(one, combatAttrType, cf);
                EmitDamage(one, false, /*aggregated=*/false);
            }
        }
        LeaveCriticalSection(&g_st.lock);
    }

    return o_ApplyDamage(self, amount, stats, combatAttrType, skills);
}

// Brackets one incoming attack so its damage-type components collapse into the
// single number the player's health bar reflects. Incoming damage has no
// combat-text commit point, so the scope close is the commit.
bool Hk_TakeAttack(CombatManager* self, ParametersCombat* params,
                   SkillManager* skills, CharacterBio* bio)
{
    bool opened = false;

    Character* victim = g_sym.CombatManagerGetCharacter
                      ? g_sym.CombatManagerGetCharacter(self) : nullptr;
    const uint32_t defenderId = ObjectId(victim);
    const uint32_t attackerId = g_sym.GetAttackerId ? g_sym.GetAttackerId(self) : 0;

    EnterCriticalSection(&g_st.lock);
    // Nesting is possible (retaliation re-entering combat), so only the
    // outermost scope for the player opens and commits; inner ones accumulate
    // into it, which is right -- it is all one health-bar drop.
    if (defenderId != 0 && defenderId == g_st.playerId && !g_st.incoming.active) {
        ClearPending(g_st.incoming);
        g_st.incoming.active     = true;
        g_st.incoming.attackerId = attackerId;
        g_st.incoming.defenderId = defenderId;
        g_st.incoming.source     = GN_SRC_INCOMING;
        LookupName(attackerId, g_st.incoming.otherName,
                   sizeof(g_st.incoming.otherName));
        opened = true;
    }
    LeaveCriticalSection(&g_st.lock);

    bool r = o_TakeAttack(self, params, skills, bio);

    if (opened) {
        EnterCriticalSection(&g_st.lock);
        // Crit is unknowable for incoming damage: the combat-text pair is on
        // Player and only fires for damage the player deals.
        EmitDamage(g_st.incoming, /*isCrit=*/false);
        ClearPending(g_st.incoming);
        LeaveCriticalSection(&g_st.lock);
    }
    return r;
}

float Hk_CombatTextHit(Player* self, float amount)
{
    float r = o_CombatTextHit(self, amount);
    CommitCombatText(false);
    return r;
}

float Hk_CombatTextCrit(Player* self, float amount)
{
    float r = o_CombatTextCrit(self, amount);
    CommitCombatText(true);
    return r;
}

void Hk_UpdateSelf(Player* self, int dt)
{
    // Cheapest reliable place to learn who the local player is.
    if (self != g_st.playerPtr) {
        bool announce = false;
        GmPlayer m = {};

        EnterCriticalSection(&g_st.lock);
        g_st.playerPtr = self;
        uint32_t id = ObjectId(self);
        if (!id) id = PlayerId();   // fall back to gGameEngine->GetPlayerId()
        if (id && id != g_st.playerId) {
            g_st.playerId = id;
            g_st.petCount = 0;
            ClearPending(g_st.direct);
            ClearPending(g_st.incoming);
            for (size_t i = 0; i < kMaxPending; ++i) ClearPending(g_st.loose[i]);

            m.gameTimeMs = GameTimeMs();
            m.playerId   = id;
            ObjectName(self, m.name, sizeof(m.name));
            announce = true;
        }
        LeaveCriticalSection(&g_st.lock);

        if (announce) {
            LOGI("player id=%u ptr=%p", m.playerId, (void*)self);
            Publish(&m, GN_MSG_PLAYER, sizeof(m));
        }
    }
    o_UpdateSelf(self, dt);
}

void Hk_PostPetSpawn(Player* self, const WorldVec3* pos, uint32_t petId,
                     uint32_t b, uint32_t c, bool d)
{
    EnterCriticalSection(&g_st.lock);
    if (petId && !IsPet(petId) && g_st.petCount < kMaxPets)
        g_st.pets[g_st.petCount++] = petId;
    LeaveCriticalSection(&g_st.lock);

    o_PostPetSpawn(self, pos, petId, b, c, d);
}

void Hk_CharacterIsDying(Character* self)
{
    GmDeath m = {};
    m.gameTimeMs  = GameTimeMs();
    m.characterId = ObjectId(self);
    ObjectName(self, m.name, sizeof(m.name));
    Publish(&m, GN_MSG_DEATH, sizeof(m));

    o_CharacterIsDying(self);
}

// Re-announce the player identity to a client that connected after the game
// had already settled on one. UpdateSelf only publishes on change, so without
// this a late joiner would show "player 0" forever.
void AnnounceCurrentState()
{
    GmPlayer m = {};
    EnterCriticalSection(&g_st.lock);
    const bool known = g_st.playerId != 0;
    if (known) {
        m.gameTimeMs = GameTimeMs();
        m.playerId   = g_st.playerId;
        ObjectName(g_st.playerPtr, m.name, sizeof(m.name));
    }
    LeaveCriticalSection(&g_st.lock);

    if (known) Publish(&m, GN_MSG_PLAYER, sizeof(m));
}

// ---- install ------------------------------------------------------------

struct HookSpec {
    void*       target;
    void*       detour;
    void**      original;
    const char* name;
    bool        required;
};

bool Enable(const HookSpec& h)
{
    if (!h.target) {
        if (h.required) LOGE("hook %s: no target address", h.name);
        return !h.required;
    }
    MH_STATUS s = MH_CreateHook(h.target, h.detour, h.original);
    if (s != MH_OK) {
        LOGE("MH_CreateHook(%s) = %s", h.name, MH_StatusToString(s));
        return !h.required;
    }
    s = MH_EnableHook(h.target);
    if (s != MH_OK) {
        LOGE("MH_EnableHook(%s) = %s", h.name, MH_StatusToString(s));
        return !h.required;
    }
    LOGI("hooked %-28s @ %p", h.name, h.target);
    return true;
}

} // namespace

bool HooksInstall()
{
    if (g_csReady) { LOGW("HooksInstall called twice"); return false; }

    InitializeCriticalSection(&g_st.lock);
    g_csReady = true;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        LOGE("MH_Initialize = %s", MH_StatusToString(s));
        HooksRemove();
        return false;
    }
    g_mhReady = true;

    const HookSpec specs[] = {
        {g_sym.Character_AttackTarget,        (void*)&Hk_AttackTarget,
         (void**)&o_AttackTarget,     "Character::AttackTarget",     true},
        {g_sym.CombatManager_ApplyDamage,     (void*)&Hk_ApplyDamage,
         (void**)&o_ApplyDamage,      "CombatManager::ApplyDamage",  true},
        // Optional: losing it costs incoming-damage aggregation only, so a
        // signature change here must not take outgoing DPS down with it.
        {g_sym.CombatManager_TakeAttack,      (void*)&Hk_TakeAttack,
         (void**)&o_TakeAttack,       "CombatManager::TakeAttack",   false},
        {g_sym.Player_RegisterCombatTextHit,  (void*)&Hk_CombatTextHit,
         (void**)&o_CombatTextHit,    "Player::CombatTextHit",       true},
        {g_sym.Player_RegisterCombatTextCrit, (void*)&Hk_CombatTextCrit,
         (void**)&o_CombatTextCrit,   "Player::CombatTextCrit",      true},
        {g_sym.Player_UpdateSelf,             (void*)&Hk_UpdateSelf,
         (void**)&o_UpdateSelf,       "Player::UpdateSelf",          true},
        {g_sym.Player_PostPetSpawn,           (void*)&Hk_PostPetSpawn,
         (void**)&o_PostPetSpawn,     "Player::PostPetSpawn",        false},
        {g_sym.Character_CharacterIsDying,    (void*)&Hk_CharacterIsDying,
         (void**)&o_CharacterIsDying, "Character::CharacterIsDying", false},
    };

    bool ok = true;
    for (const HookSpec& h : specs) ok = Enable(h) && ok;
    if (!ok) { HooksRemove(); return false; }

    PipeSetOnConnect(&AnnounceCurrentState);
    // Heartbeats must carry the same clock as damage events.
    PipeSetTimeSource(&GameTimeMs);
    LOGI("all hooks installed");
    return true;
}

// Safe to call when nothing was installed, and safe to call twice: both happen
// in practice. HooksInstall calls this itself when a required hook fails, and
// DllMain's detach path calls it again regardless, so without the flags the
// critical section would be deleted twice.
void HooksRemove()
{
    if (g_mhReady) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_mhReady = false;
    }
    if (g_csReady) {
        DeleteCriticalSection(&g_st.lock);
        g_csReady = false;
    }
}

} // namespace grimnumbers
