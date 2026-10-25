#include "gd_symbols.h"
#include "log.h"

#include <windows.h>
#include <string.h>

namespace gd {

Symbols g_sym = {};

namespace {

struct Entry {
    const char* module;   // "Game.dll" or "Engine.dll"
    const char* mangled;
    void**      slot;
    bool        required;
    const char* note;     // reference RVA from the 2026-10-08 build
};

HMODULE g_game = nullptr;
HMODULE g_engine = nullptr;

HMODULE ModuleFor(const char* name)
{
    return _stricmp(name, "Game.dll") == 0 ? g_game : g_engine;
}

} // namespace

bool ResolveSymbols(uint32_t timeoutMs)
{
    // Grim Dawn.exe statically imports Engine.dll, Game.dll and WINMM.dll, so
    // by the time our DllMain runs the loader may not have mapped Game.dll
    // yet. Poll rather than assume an order.
    const DWORD deadline = GetTickCount() + timeoutMs;
    for (;;) {
        g_engine = GetModuleHandleA("Engine.dll");
        g_game   = GetModuleHandleA("Game.dll");
        if (g_engine && g_game) break;
        if (GetTickCount() >= deadline) {
            LOGE("timed out waiting for Game.dll/Engine.dll (engine=%p game=%p)",
                 (void*)g_engine, (void*)g_game);
            return false;
        }
        Sleep(50);
    }
    LOGI("Engine.dll=%p Game.dll=%p", (void*)g_engine, (void*)g_game);

    const Entry table[] = {
        // --- hook targets ---
        {"Game.dll",
         "?AttackTarget@Character@GAME@@UEAA_NIAEAVEntity@2@AEAVParametersCombat@2@_NIII@Z",
         &g_sym.Character_AttackTarget, true, "Game+0x00067460"},
        {"Game.dll",
         "?ApplyDamage@CombatManager@GAME@@QEAA_NMAEBUPlayStatsDamageType@2@W4CombatAttributeType@2@AEBV?$vector@I@mem@@@Z",
         &g_sym.CombatManager_ApplyDamage, true, "Game+0x0011D340"},
        {"Game.dll",
         "?TakeAttack@CombatManager@GAME@@QEAA_NAEAVParametersCombat@2@AEAVSkillManager@2@AEAVCharacterBio@2@@Z",
         &g_sym.CombatManager_TakeAttack, false, "Game+0x0011D950"},
        {"Game.dll", "?RegisterCombatTextHit@Player@GAME@@UEAAMM@Z",
         &g_sym.Player_RegisterCombatTextHit, true, "Game+0x003E36E0"},
        {"Game.dll", "?RegisterCombatTextCrit@Player@GAME@@UEAAMM@Z",
         &g_sym.Player_RegisterCombatTextCrit, true, "Game+0x003E3740"},
        {"Game.dll", "?UpdateSelf@Player@GAME@@UEAAXH@Z",
         &g_sym.Player_UpdateSelf, true, "Game+0x003D24A0"},
        {"Game.dll", "?PostPetSpawn@Player@GAME@@UEAAXAEBVWorldVec3@2@III_N@Z",
         &g_sym.Player_PostPetSpawn, false, "Game+0x003E6B50"},
        {"Game.dll", "?CharacterIsDying@Character@GAME@@UEAAXXZ",
         &g_sym.Character_CharacterIsDying, false, "Game+0x00068DB0"},
        {"Game.dll", "?RegisterDamage@GameEngine@GAME@@QEAAXIIM@Z",
         &g_sym.GameEngine_RegisterDamage, false, "Game+0x002C5BC0"},

        // --- direct calls ---
        {"Engine.dll", "?GetObjectId@Object@GAME@@QEBAIXZ",
         (void**)&g_sym.GetObjectId, true, "Engine+0x0003A740"},
        {"Engine.dll", "?GetObjectName@Object@GAME@@QEBAPEBDXZ",
         (void**)&g_sym.GetObjectName, true, "Engine+0x0003A760"},
        {"Game.dll", "?GetAttackerId@CombatManager@GAME@@QEBA?BIXZ",
         (void**)&g_sym.GetAttackerId, true, "Game+0x0011D740"},
        {"Game.dll", "?GetCharacter@CombatManager@GAME@@QEAAPEAVCharacter@2@XZ",
         (void**)&g_sym.CombatManagerGetCharacter, true, "Game+0x00072F30"},
        {"Game.dll", "?GetPlayerId@GameEngine@GAME@@QEBAIXZ",
         (void**)&g_sym.GetPlayerId, false, "Game+0x002D4B20"},
        {"Engine.dll", "?GetGameTime@GAME@@YAHXZ",
         (void**)&g_sym.GetGameTime, true, "Engine+0x001E9400"},

        // --- exported data ---
        {"Game.dll", "?gGameEngine@GAME@@3PEAVGameEngine@1@EA",
         (void**)&g_sym.gGameEngine, false, "Game+0x00A66288"},
    };

    bool ok = true;
    for (const Entry& e : table) {
        HMODULE m = ModuleFor(e.module);
        FARPROC p = GetProcAddress(m, e.mangled);
        *e.slot = (void*)p;
        if (!p) {
            if (e.required) {
                LOGE("missing REQUIRED %s!%s (was %s)", e.module, e.mangled, e.note);
                ok = false;
            } else {
                LOGW("missing optional %s!%s (was %s)", e.module, e.mangled, e.note);
            }
        }
    }
    return ok;
}

uint32_t ObjectId(const void* obj)
{
    if (!obj || !g_sym.GetObjectId) return 0;
    return g_sym.GetObjectId((const Object*)obj);
}

void ObjectName(const void* obj, char* out, size_t outSize)
{
    if (!outSize) return;
    out[0] = '\0';
    if (!obj || !g_sym.GetObjectName) return;
    const char* n = g_sym.GetObjectName((const Object*)obj);
    if (!n) return;

    // Names come back as database paths, e.g.
    // "records/creatures/enemies/npc_aetherialguard_01.dbr" - 51 characters,
    // which does not fit the 48-byte wire field. Truncating would keep the
    // shared "records/creatures/..." prefix and throw away the only part that
    // identifies the creature, so keep the file stem instead.
    const char* base = n;
    for (const char* p = n; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;

    size_t len = strlen(base);
    if (len > 4 && _stricmp(base + len - 4, ".dbr") == 0) len -= 4;
    if (len == 0) { base = n; len = strlen(n); }   // path ended in a separator
    if (len > outSize - 1) len = outSize - 1;

    memcpy(out, base, len);
    out[len] = '\0';
}

uint32_t PlayerId()
{
    if (g_sym.GetPlayerId && g_sym.gGameEngine && *g_sym.gGameEngine)
        return g_sym.GetPlayerId(*g_sym.gGameEngine);
    return 0;
}

uint32_t GameTimeMs()
{
    return g_sym.GetGameTime ? (uint32_t)g_sym.GetGameTime() : GetTickCount();
}

} // namespace gd
