// grimnumbers_client - console readout, kept as the no-GUI way to watch the pipe
// and as the tool for discovering damage-type ids (--raw prints them).
//
// Usage: grimnumbers_client [--window SECONDS] [--raw] [--top N]
//
//   --window N   rolling window in seconds (default 5)
//   --raw        also print one line per damage event, with its element split
//   --top N      show the N busiest targets (default 5)
//
// The accounting lives in Stats and the pipe in PipeClient, both shared with
// grimnumbers_app; this file is only the text rendering.

#include "fields.h"
#include "pipe_client.h"
#include "stats.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace grimnumbers;

namespace {

struct Options {
    double windowSec = 5.0;
    bool   raw       = false;
    size_t top       = 5;
};

volatile bool g_stop = false;

BOOL WINAPI OnCtrlC(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) { g_stop = true; return TRUE; }
    return FALSE;
}

Options ParseArgs(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            o.windowSec = atof(argv[++i]);
            if (o.windowSec < 0.5) o.windowSec = 0.5;
        } else if (strcmp(argv[i], "--raw") == 0) {
            o.raw = true;
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            o.top = (size_t)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: grimnumbers_client [--window SECONDS] [--raw] [--top N]\n");
            exit(0);
        }
    }
    return o;
}

const char* SourceLabel(uint8_t s)
{
    switch (s) {
    case GN_SRC_PLAYER:   return "player";
    case GN_SRC_PET:      return "pet";
    case GN_SRC_INCOMING: return "taken";
    default:              return "?";
    }
}

// One line per event. The element split is printed as raw ids, which is how
// the damage-type numbers get identified in the first place: cast a known
// skill and watch which id carries the damage.
void PrintRaw(const GmDamage& d, const ElementTable& elements)
{
    const std::string nm = ShortName(d.otherName);

    printf("  %8.1f %-6s %-5s skill=%-8u %-6s %-20.20s comp=%u%s",
           d.totalDamage, SourceLabel(d.source), d.isCrit ? "CRIT" : "",
           d.skillId, GN_IS_OUTGOING(d.source) ? "->" : "<-",
           nm.empty() ? "?" : nm.c_str(),
           d.componentCount, d.aggregated ? "" : " tick");

    for (uint8_t i = 0; i < d.elemCount && i < GN_MAX_ELEM; ++i) {
        printf("  [%s %.0f]", elements.Label(d.elems[i].type).c_str(),
               (double)d.elems[i].amount);
    }
    if (d.elemDropped) printf("  [+%u more types]", d.elemDropped);
    printf("\n");
}

void Render(const Snapshot& s, const Options& o, const ElementTable& elements)
{
    printf("\n");
    printf("  rolling DPS (%.1fs) : %12.1f\n", s.windowSec, s.rollingDps);
    printf("  session DPS (%.1fs) : %12.1f\n", s.spanSec, s.sessionDps);
    printf("  total damage       : %12.0f over %llu hits\n",
           s.totalDamage, (unsigned long long)s.totalHits);
    printf("  crits              : %12.1f%%  (%llu, %.0f dmg)\n",
           s.critRate, (unsigned long long)s.critHits, s.critDamage);
    printf("  biggest hit        : %12.0f\n", s.biggestHit);

    if (!s.elements.empty()) {
        printf("  elements (rolling DPS, share of session total):\n");
        for (const ElemRow& e : s.elements) {
            const double share = s.totalDamage > 0.0
                               ? 100.0 * e.total / s.totalDamage : 0.0;
            printf("    %-18s %12.1f/s  %5.1f%%  %14.0f total\n",
                   elements.Label(e.type).c_str(),
                   e.window / s.windowSec, share, e.total);
        }
        if (s.elemDropped)
            printf("    (%llu hits had more than %d damage types)\n",
                   (unsigned long long)s.elemDropped, GN_MAX_ELEM);
    }

    if (s.totalTaken > 0.0) {
        printf("  --- taken ---\n");
        printf("  rolling DTPS       : %12.1f\n", s.rollingDtps);
        printf("  total taken        : %12.0f over %llu hits",
               s.totalTaken, (unsigned long long)s.takenHits);
        if (s.takenTicks) printf(" + %llu ticks", (unsigned long long)s.takenTicks);
        printf("\n");
        printf("  biggest taken      : %12.0f\n", s.biggestTaken);
    }

    if (s.gaps || s.serverDropped)
        printf("  DROPPED EVENTS     : %12llu total  (5s %llu, 60s %llu)\n",
               (unsigned long long)s.serverDropped,
               (unsigned long long)s.dropped5s,
               (unsigned long long)s.dropped60s);

    if (o.top && !s.targets.empty()) {
        printf("  top targets:\n");
        for (size_t i = 0; i < s.targets.size() && i < o.top; ++i) {
            printf("    %-24.24s %12.0f  (%llu hits)\n",
                   s.targets[i].name.empty() ? "?" : s.targets[i].name.c_str(),
                   s.targets[i].total,
                   (unsigned long long)s.targets[i].hits);
        }
    }
    fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = ParseArgs(argc, argv);
    SetConsoleCtrlHandler(OnCtrlC, TRUE);

    // Line-buffer stdout. Redirected to a file it would otherwise be fully
    // buffered, so anything written since the last flush is lost if the
    // process is killed rather than closed - which is exactly how you tend to
    // stop a live monitor.
    setvbuf(stdout, nullptr, _IOLBF, 4096);

    ElementTable elements;
    elements.Load(L"elements.ini");

    printf("grimnumbers_client - waiting for Grim Dawn (pipe %ls)\n", GN_PIPE_NAME);
    printf("window=%.1fs  top=%zu%s\n", opt.windowSec, opt.top,
           opt.raw ? "  raw" : "");

    Stats stats(opt.windowSec);
    PipeClient pipe(stats);
    if (!pipe.Start()) {
        fprintf(stderr, "could not start the pipe reader\n");
        return 1;
    }

    std::string lastStatus;
    DWORD lastRender = GetTickCount();

    while (!g_stop) {
        Sleep(50);

        const std::string status = pipe.Status();
        if (status != lastStatus) {
            lastStatus = status;
            printf("\n[%s]\n", status.c_str());
        }

        if (opt.raw) {
            GmDamage d = {};
            while (stats.PopRawEvent(d)) PrintRaw(d, elements);
        }

        // Repaint at most twice a second. Stats::Take decays the rolling
        // window on its own, so the figure falls to zero when combat stops
        // instead of freezing at the last value.
        const DWORD now = GetTickCount();
        if (now - lastRender >= 500) {
            lastRender = now;
            Render(stats.Take(), opt, elements);
        }
    }

    pipe.Stop();
    printf("bye\n");
    return 0;
}
