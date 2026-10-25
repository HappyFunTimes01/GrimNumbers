// Smoke test: drives the real pipe server with synthetic damage events so the
// wire format, ring buffer and the client's DPS math can be exercised without
// launching Grim Dawn. Only the hooks themselves are untested by this.
//
//   grimnumbers_pipe_smoke [--dps N] [--seconds N] [--dtps N]
//
// Emits `dps` damage per second in 10 hits/sec, so the client's rolling DPS
// should converge on the requested number. `--dtps` adds incoming damage at a
// known rate, half as aggregated hits and half as un-aggregated DoT ticks, so
// the client's hit-vs-tick accounting can be checked too.

#include "grimnumbers/protocol.h"
#include "pipe_server.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

uint32_t g_t0 = 0;

// Heartbeats must share the epoch of the damage events below.
uint32_t SmokeClock() { return GetTickCount() - g_t0; }

// Mirrors what hooks.cpp does: re-announce identity to a late joiner.
void AnnounceState()
{
    GmPlayer p = {};
    p.playerId = 4242;
    strcpy_s(p.name, "SmokeTestHero");
    grimnumbers::Publish(&p, GN_MSG_PLAYER, sizeof(p));
}

} // namespace

int main(int argc, char** argv)
{
    double dps = 10000.0;
    double dtps = 0.0;
    int seconds = 15;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dps") == 0 && i + 1 < argc) dps = atof(argv[++i]);
        else if (strcmp(argv[i], "--dtps") == 0 && i + 1 < argc) dtps = atof(argv[++i]);
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);
    }

    grimnumbers::LogInit();
    g_t0 = GetTickCount();
    grimnumbers::PipeSetOnConnect(&AnnounceState);
    grimnumbers::PipeSetTimeSource(&SmokeClock);
    if (!grimnumbers::PipeStart()) { fprintf(stderr, "PipeStart failed\n"); return 1; }

    printf("feeding %.0f dps", dps);
    if (dtps > 0.0) printf(" and %.0f dtps", dtps);
    printf(" for %ds on %ls\n", seconds, GN_PIPE_NAME);
    printf("run grimnumbers_client in another console.\n");

    const int hitsPerSec = 10;
    const float perHit = (float)(dps / hitsPerSec);
    // Incoming: 2 aggregated hits/sec carrying half the budget, and 8 DoT
    // ticks/sec carrying the other half.
    const float perTaken = (float)(dtps * 0.5 / 2.0);
    const float perTick  = (float)(dtps * 0.5 / 8.0);
    const uint32_t t0 = g_t0;

    const char* targets[] = { "Fleshwarped Abomination", "Chthonian Hellhound", "Loghorrean" };

    for (int i = 0; i < seconds * hitsPerSec; ++i) {
        GmDamage d = {};
        d.gameTimeMs     = GetTickCount() - t0;
        d.attackerId     = 4242;
        d.defenderId     = 1000 + (i % 3);
        d.skillId        = 777;
        d.source         = GN_SRC_PLAYER;
        d.componentCount = 3;
        // Every 5th hit crits for double, and the average is kept on target by
        // shaving the non-crit hits.
        const bool crit = (i % 5) == 0;
        d.isCrit     = crit ? 1 : 0;
        d.totalDamage = crit ? perHit * 2.0f : perHit * 0.75f;
        d.aggregated = 1;

        // A synthetic damage-type split, so the element breakdown can be
        // exercised without the game. These ids are arbitrary stand-ins, NOT
        // claimed values of CombatAttributeType - the real ones have to be
        // observed (see config/elements.ini).
        const uint32_t kFakeTypes[3]  = {1, 4, 6};
        const float    kFakeShares[3] = {0.50f, 0.35f, 0.15f};
        d.elemCount = 3;
        for (uint8_t e = 0; e < 3; ++e) {
            d.elems[e].type   = kFakeTypes[e];
            d.elems[e].amount = d.totalDamage * kFakeShares[e];
        }

        strcpy_s(d.otherName, targets[i % 3]);
        grimnumbers::Publish(&d, GN_MSG_DAMAGE, sizeof(d));

        if (dtps > 0.0) {
            GmDamage in = {};
            in.gameTimeMs = d.gameTimeMs;
            in.attackerId = 1000 + (i % 3);
            in.defenderId = 4242;
            in.source     = GN_SRC_INCOMING;
            strcpy_s(in.otherName, targets[i % 3]);

            // One aggregated hit every 5th iteration (2/sec), a DoT tick on
            // the other 8.
            if (i % 5 == 0) {
                in.totalDamage    = perTaken;
                in.componentCount = 4;
                in.aggregated     = 1;
            } else {
                in.totalDamage    = perTick;
                in.componentCount = 1;
                in.aggregated     = 0;
            }
            grimnumbers::Publish(&in, GN_MSG_DAMAGE, sizeof(in));
        }

        Sleep(1000 / hitsPerSec);
    }

    printf("done, draining...\n");
    Sleep(1000);
    grimnumbers::PipeStop();
    grimnumbers::LogShutdown();
    return 0;
}
