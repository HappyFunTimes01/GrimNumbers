// Resolution of Grim Dawn's exported C++ symbols.
//
// Game.dll and Engine.dll export their full mangled C++ symbol table (25113
// and 6283 names respectively in the 2026-10-08 build), so every address here
// comes from GetProcAddress on the mangled name. No RVAs are hardcoded: the
// RVAs in the symbol index are reference only, for when a name disappears and
// the signature has to be re-derived.
#pragma once

#include <stdint.h>

// Opaque game types. We never dereference these; we only pass them back into
// the game's own accessors.
namespace gd {

struct Object;
struct Entity;
struct Character;
struct Player;
struct CombatManager;
struct SkillManager;
struct CharacterBio;
struct GameEngine;
struct ParametersCombat;
struct PlayStatsDamageType;
struct WorldVec3;

// mem::vector<T> is { T* begin; T* end; T* capacity; } - confirmed against
// GDDPSMeter's DetourUtil::VectorMemoryToVector0 and widened to 64-bit here.
template <typename T>
struct MemVector {
    const T* begin_;
    const T* end_;
    const T* cap_;
    size_t size() const { return begin_ && end_ >= begin_ ? size_t(end_ - begin_) : 0; }
    const T& operator[](size_t i) const { return begin_[i]; }
};

// ---- hook targets -------------------------------------------------------
// All x64 member functions: `this` arrives in RCX as the first argument.

// ?AttackTarget@Character@GAME@@UEAA_NIAEAVEntity@2@AEAVParametersCombat@2@_NIII@Z
typedef bool (*Fn_Character_AttackTarget)(Character* self, uint32_t skillId, Entity* target,
                                         ParametersCombat* params, bool flag,
                                         uint32_t directDmgType, uint32_t durationDmgType,
                                         uint32_t damageMult);

// ?ApplyDamage@CombatManager@GAME@@QEAA_NMAEBUPlayStatsDamageType@2@W4CombatAttributeType@2@AEBV?$vector@I@mem@@@Z
typedef bool (*Fn_CombatManager_ApplyDamage)(CombatManager* self, float amount,
                                             const PlayStatsDamageType* stats,
                                             uint32_t combatAttrType,
                                             const MemVector<uint32_t>* skills);

// ?TakeAttack@CombatManager@GAME@@QEAA_NAEAVParametersCombat@2@AEAVSkillManager@2@AEAVCharacterBio@2@@Z
// `this` is the DEFENDER's CombatManager. Brackets one incoming attack: it
// calls ApplyDamage directly and again via CombatAttributeAccumulator::
// ExecuteDamage, so every component of one visible hit lands inside it.
typedef bool (*Fn_CombatManager_TakeAttack)(CombatManager* self,
                                            ParametersCombat* params,
                                            SkillManager* skills,
                                            CharacterBio* bio);

// ?RegisterCombatTextHit@Player@GAME@@UEAAMM@Z / ...Crit...
typedef float (*Fn_Player_RegisterCombatText)(Player* self, float amount);

// ?UpdateSelf@Player@GAME@@UEAAXH@Z
typedef void (*Fn_Player_UpdateSelf)(Player* self, int dt);

// ?PostPetSpawn@Player@GAME@@UEAAXAEBVWorldVec3@2@III_N@Z
typedef void (*Fn_Player_PostPetSpawn)(Player* self, const WorldVec3* pos, uint32_t petId,
                                       uint32_t b, uint32_t c, bool d);

// ?CharacterIsDying@Character@GAME@@UEAAXXZ
typedef void (*Fn_Character_CharacterIsDying)(Character* self);

// ?RegisterDamage@GameEngine@GAME@@QEAAXIIM@Z
typedef void (*Fn_GameEngine_RegisterDamage)(GameEngine* self, uint32_t defenderId,
                                            uint32_t attackerId, float amount);

// ---- called, not hooked -------------------------------------------------

typedef uint32_t    (*Fn_Object_GetObjectId)(const Object* self);
typedef const char* (*Fn_Object_GetObjectName)(const Object* self);
typedef uint32_t    (*Fn_CombatManager_GetAttackerId)(const CombatManager* self);
typedef Character*  (*Fn_CombatManager_GetCharacter)(CombatManager* self);
typedef uint32_t    (*Fn_GameEngine_GetPlayerId)(const GameEngine* self);
typedef int         (*Fn_GetGameTime)(void);

// Resolved addresses. Populated by gd::ResolveSymbols().
struct Symbols {
    // hook targets
    void* Character_AttackTarget;
    void* CombatManager_ApplyDamage;
    void* CombatManager_TakeAttack;
    void* Player_RegisterCombatTextHit;
    void* Player_RegisterCombatTextCrit;
    void* Player_UpdateSelf;
    void* Player_PostPetSpawn;
    void* Character_CharacterIsDying;
    void* GameEngine_RegisterDamage;

    // direct calls
    Fn_Object_GetObjectId          GetObjectId;
    Fn_Object_GetObjectName        GetObjectName;
    Fn_CombatManager_GetAttackerId GetAttackerId;
    Fn_CombatManager_GetCharacter  CombatManagerGetCharacter;
    Fn_GameEngine_GetPlayerId      GetPlayerId;
    Fn_GetGameTime                 GetGameTime;

    // exported data: GameEngine* gGameEngine
    GameEngine** gGameEngine;
};

extern Symbols g_sym;

// Waits (up to timeoutMs) for Game.dll and Engine.dll to be present, then
// resolves every symbol above. Returns false if any required symbol is
// missing; the log names each failure.
bool ResolveSymbols(uint32_t timeoutMs);

// Convenience wrappers that tolerate null.
uint32_t ObjectId(const void* obj);
void     ObjectName(const void* obj, char* out, size_t outSize);
uint32_t PlayerId();
uint32_t GameTimeMs();

} // namespace gd
