#include "pipe_server.h"
#include "log.h"
#include "grimnumbers/protocol.h"

#include <windows.h>
#include <string.h>

namespace grimnumbers {
namespace {

// Power of two so the index wrap is a mask.
// 16k slots x ~88 bytes is ~1.4 MB, cheap insurance against bursts. A high-
// rate build can produce 70+ events/sec sustained.
constexpr size_t kRingSlots = 16384;
constexpr size_t kSlotSize  = sizeof(GmAnyMsg);

struct Slot {
    unsigned char bytes[kSlotSize];
    uint32_t      size;
};

Slot              g_ring[kRingSlots];
size_t            g_head = 0;          // next write
size_t            g_tail = 0;          // next read
CRITICAL_SECTION  g_lock;
HANDLE            g_wake   = nullptr;  // signalled on publish
HANDLE            g_quit   = nullptr;
HANDLE            g_thread = nullptr;
volatile LONG     g_running = 0;
uint32_t          g_seq = 0;
volatile LONG64   g_dropped = 0;
// Diagnostics, all reset when a client connects so the numbers describe the
// current session rather than everything since the DLL loaded.
volatile LONG64   g_published = 0;   // Publish() calls accepted into the ring
volatile LONG64   g_sent      = 0;   // messages handed to WriteFile
size_t            g_highWater = 0;   // deepest the ring ever got (under g_lock)

// Drops bucketed by wall-clock second, so we can answer "are we losing events
// right now" separately from "did we ever lose any". 64 is a power of two (so
// the index wrap is a mask) and covers the longest window we report.
constexpr size_t kDropBuckets = 64;
uint32_t         g_dropBucket[kDropBuckets] = {};
uint32_t         g_dropSec = 0;    // wall second the newest bucket belongs to

// Zeroes the buckets we skipped over since the last update. Caller holds
// g_lock. Without this a window would sum counts from minutes ago.
void RollBuckets()
{
    const uint32_t sec = GetTickCount() / 1000;
    const uint32_t advance = sec - g_dropSec;
    if (advance == 0) return;
    if (advance >= kDropBuckets) {
        memset(g_dropBucket, 0, sizeof(g_dropBucket));
    } else {
        for (uint32_t i = 1; i <= advance; ++i)
            g_dropBucket[(g_dropSec + i) & (kDropBuckets - 1)] = 0;
    }
    g_dropSec = sec;
}

// Caller holds g_lock. `secs` is clamped to the history we keep.
uint64_t DropsInWindow(uint32_t secs)
{
    RollBuckets();
    uint64_t total = 0;
    for (uint32_t i = 0; i < secs && i < kDropBuckets; ++i)
        total += g_dropBucket[(g_dropSec - i) & (kDropBuckets - 1)];
    return total;
}
OnConnectFn       g_onConnect = nullptr;
TimeSourceFn      g_timeSource = nullptr;

// Writer-thread state.
HANDLE g_pipe = INVALID_HANDLE_VALUE;
// One reusable event for all writes. Creating and closing an event per
// message was the actual drain bottleneck, not the ring size.
HANDLE g_writeEvent = nullptr;

void ClosePipe()
{
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

// Creates the pipe and waits for a client. Overlapped so the wait can be
// cancelled by g_quit. Returns false when asked to shut down.
bool AcceptClient()
{
    g_pipe = CreateNamedPipeW(GN_PIPE_NAME,
                              PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_MESSAGE | PIPE_WAIT,
                              1,                  // one client at a time
                              64 * 1024, 0,
                              0, nullptr);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        LOGE("CreateNamedPipe failed: %lu", GetLastError());
        return false;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) { ClosePipe(); return false; }

    bool connected = false;
    if (ConnectNamedPipe(g_pipe, &ov)) {
        connected = true;
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = { ov.hEvent, g_quit };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                DWORD moved = 0;
                connected = GetOverlappedResult(g_pipe, &ov, &moved, FALSE) != 0;
            } else {
                CancelIo(g_pipe);
            }
        } else {
            LOGE("ConnectNamedPipe failed: %lu", err);
        }
    }

    CloseHandle(ov.hEvent);
    if (!connected) { ClosePipe(); return false; }

    // A fresh connection restarts the sequence so the client can tell its own
    // gaps from a reconnect.
    EnterCriticalSection(&g_lock);
    g_seq = 0;
    g_tail = g_head;   // don't replay backlog from before this client
    LeaveCriticalSection(&g_lock);

    // Events published with nobody attached overflow the ring as a matter of
    // course -- the writer thread is parked in ConnectNamedPipe and nothing
    // drains. Those drops say nothing about whether the link keeps up, so
    // report them once and start the counters over.
    const long long preConnect = (long long)PipeDropped();
    InterlockedExchange64(&g_dropped, 0);
    InterlockedExchange64(&g_published, 0);
    InterlockedExchange64(&g_sent, 0);
    EnterCriticalSection(&g_lock);
    g_highWater = 0;
    memset(g_dropBucket, 0, sizeof(g_dropBucket));
    g_dropSec = GetTickCount() / 1000;
    LeaveCriticalSection(&g_lock);

    LOGI("client connected (discarded %lld pre-connect drops)", preConnect);
    return true;
}

bool WriteBlocking(const void* data, DWORD size)
{
    if (!g_writeEvent) {
        g_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_writeEvent) return false;
    }
    ResetEvent(g_writeEvent);

    OVERLAPPED ov = {};
    ov.hEvent = g_writeEvent;

    // The handle is FILE_FLAG_OVERLAPPED, and for an overlapped write the
    // lpNumberOfBytesWritten out-parameter is undefined -- it comes back 0
    // even when the write succeeded. So pass nullptr and take the count from
    // GetOverlappedResult in both the immediate and the pending case. Trusting
    // WriteFile's count here made the server close a perfectly good pipe right
    // after the HELLO.
    bool pending = false;
    if (!WriteFile(g_pipe, data, size, nullptr, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            pending = true;
        } else {
            return false;
        }
    }

    if (pending) {
        HANDLE waits[2] = { ov.hEvent, g_quit };
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
        if (w != WAIT_OBJECT_0) {
            CancelIo(g_pipe);
            return false;
        }
    }

    DWORD written = 0;
    return GetOverlappedResult(g_pipe, &ov, &written, TRUE) != 0 && written == size;
}

// Packs as many whole messages as will fit into `out`. Returns the byte count
// and, via `count`, how many messages it covers; 0 when the ring is empty.
//
// One WriteFile per event was the ceiling: each is a round trip whose latency
// is the client's read loop, so the drain rate was pinned near a hundred a
// second no matter how big the ring got. A batch costs one round trip for
// hundreds of events, which is the difference between keeping up and not.
uint32_t PopBatch(unsigned char* out, uint32_t outCap, uint32_t* count)
{
    uint32_t used = 0;
    uint32_t n = 0;
    EnterCriticalSection(&g_lock);
    while (g_tail != g_head) {
        Slot& s = g_ring[g_tail & (kRingSlots - 1)];
        if (s.size == 0 || s.size > outCap) { g_tail++; continue; }  // unsendable
        if (used + s.size > outCap) break;
        memcpy(out + used, s.bytes, s.size);
        used += s.size;
        ++n;
        g_tail++;
    }
    LeaveCriticalSection(&g_lock);
    *count = n;
    return used;
}

DWORD WINAPI WriterThread(LPVOID)
{
    // Sized to the pipe's outbound buffer so a batch is never split.
    static unsigned char buf[60 * 1024];

    while (WaitForSingleObject(g_quit, 0) != WAIT_OBJECT_0) {
        if (!AcceptClient()) {
            if (WaitForSingleObject(g_quit, 500) == WAIT_OBJECT_0) break;
            continue;
        }

        // Greet the client so it can validate the protocol immediately.
        GmHello hello = {};
        hello.pid = GetCurrentProcessId();
        Publish(&hello, GN_MSG_HELLO, sizeof(hello));

        // Replay the state a late joiner would otherwise miss.
        if (g_onConnect) g_onConnect();

        DWORD    lastBeat = GetTickCount();
        DWORD    lastLog  = lastBeat;
        long long logPub = 0, logSent = 0, logDrop = 0;

        for (;;) {
            uint32_t count = 0;
            uint32_t n = PopBatch(buf, (uint32_t)sizeof(buf), &count);
            if (n) {
                if (!WriteBlocking(buf, n)) {
                    LOGI("client disconnected (%lu)", GetLastError());
                    break;
                }
                InterlockedExchangeAdd64(&g_sent, (LONG64)count);
            } else {
                HANDLE waits[2] = { g_wake, g_quit };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, 250);
                if (w == WAIT_OBJECT_0 + 1) goto done;
            }

            const DWORD now = GetTickCount();

            // A once-a-second line, so the log shows the shape of the load
            // over a fight rather than one number at the end of it.
            if (now - lastLog >= 1000) {
                const DWORD span = now - lastLog;
                lastLog = now;

                EnterCriticalSection(&g_lock);
                const size_t hw    = g_highWater;
                const size_t depth = g_head - g_tail;
                const uint64_t d5  = DropsInWindow(5);
                const uint64_t d60 = DropsInWindow(60);
                LeaveCriticalSection(&g_lock);

                const long long pub  = (long long)InterlockedCompareExchange64(&g_published, 0, 0);
                const long long sent = (long long)InterlockedCompareExchange64(&g_sent, 0, 0);
                const long long drop = (long long)PipeDropped();
                if (pub != logPub || drop != logDrop) {
                    LOGI("ring: %lld pub/s %lld sent/s %lld drop/s | depth=%zu "
                         "hw=%zu/%zu | dropped 5s=%llu 60s=%llu total=%lld",
                         (pub - logPub) * 1000 / (long long)span,
                         (sent - logSent) * 1000 / (long long)span,
                         (drop - logDrop) * 1000 / (long long)span,
                         depth, hw, kRingSlots,
                         (unsigned long long)d5, (unsigned long long)d60, drop);
                }
                logPub = pub; logSent = sent; logDrop = drop;
            }

            // Heartbeat on a timer rather than only when idle. The client
            // learns the drop counts from it, and during a sustained fight
            // the ring never empties, so an idle-only beat would go silent
            // exactly when the numbers are worth seeing. Built here rather
            // than pushed through the ring so it never displaces real events.
            if (now - lastBeat >= 250) {
                lastBeat = now;

                GmHeartbeat hb = {};
                hb.hdr.magic   = GN_PROTO_MAGIC;
                hb.hdr.version = GN_PROTO_VER;
                hb.hdr.type    = GN_MSG_HEARTBEAT;
                hb.hdr.size    = sizeof(hb);
                hb.gameTimeMs  = g_timeSource ? g_timeSource() : GetTickCount();
                hb.dropped     = PipeDropped();

                // Heartbeats bypass the ring, so taking a fresh sequence
                // number here would slot one ahead of events still queued and
                // make the client count gaps that never happened. Reuse the
                // last published value; the client skips heartbeats when it
                // tracks gaps.
                EnterCriticalSection(&g_lock);
                hb.hdr.seq   = g_seq;
                hb.dropped5s = DropsInWindow(5);
                hb.dropped60s= DropsInWindow(60);
                LeaveCriticalSection(&g_lock);

                if (!WriteBlocking(&hb, sizeof(hb))) {
                    LOGI("client gone (heartbeat failed, %lu)", GetLastError());
                    break;
                }
            }
        }
        ClosePipe();
    }
done:
    ClosePipe();
    return 0;
}

} // namespace

bool PipeStart()
{
    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_quit = CreateEventW(nullptr, TRUE,  FALSE, nullptr);
    if (!g_wake || !g_quit) { LOGE("event creation failed"); return false; }

    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (!g_thread) { LOGE("writer thread failed: %lu", GetLastError()); return false; }
    LOGI("pipe server started");
    return true;
}

void PipeStop()
{
    if (!InterlockedExchange(&g_running, 0)) return;
    if (g_quit) SetEvent(g_quit);
    if (g_thread) {
        // DllMain(PROCESS_DETACH) holds the loader lock, so never wait forever.
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_writeEvent) { CloseHandle(g_writeEvent); g_writeEvent = nullptr; }
    if (g_wake) { CloseHandle(g_wake); g_wake = nullptr; }
    if (g_quit) { CloseHandle(g_quit); g_quit = nullptr; }
    DeleteCriticalSection(&g_lock);
}

void Publish(void* msg, uint16_t type, uint32_t size)
{
    if (!InterlockedCompareExchange(&g_running, 1, 1)) return;
    if (size > kSlotSize) return;

    GmHeader* h = (GmHeader*)msg;
    h->magic   = GN_PROTO_MAGIC;
    h->version = GN_PROTO_VER;
    h->type    = type;
    h->size    = size;

    EnterCriticalSection(&g_lock);
    h->seq = ++g_seq;
    if (g_head - g_tail >= kRingSlots) {
        g_tail++;                       // overwrite the oldest
        InterlockedIncrement64(&g_dropped);
        RollBuckets();
        g_dropBucket[g_dropSec & (kDropBuckets - 1)]++;
    }
    Slot& s = g_ring[g_head & (kRingSlots - 1)];
    s.size = size;
    memcpy(s.bytes, msg, size);
    g_head++;
    const size_t depth = g_head - g_tail;
    if (depth > g_highWater) g_highWater = depth;
    LeaveCriticalSection(&g_lock);
    InterlockedIncrement64(&g_published);

    SetEvent(g_wake);
}

void PipeSetOnConnect(OnConnectFn cb)
{
    g_onConnect = cb;
}

void PipeSetTimeSource(TimeSourceFn fn)
{
    g_timeSource = fn;
}

uint64_t PipeDropped()
{
    return (uint64_t)InterlockedCompareExchange64(&g_dropped, 0, 0);
}

} // namespace grimnumbers
