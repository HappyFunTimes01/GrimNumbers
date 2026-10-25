#include "pipe_client.h"

#include <stdio.h>
#include <string.h>

namespace grimnumbers {
namespace {

// The server coalesces events into one write, so a single read can carry many
// records. The buffer must be at least as large as the server's batch or the
// pipe returns ERROR_MORE_DATA.
constexpr DWORD kReadBuf = 64 * 1024;

} // namespace

PipeClient::PipeClient(Stats& stats, HWND notifyWnd, UINT notifyMsg)
    : stats_(stats), notifyWnd_(notifyWnd), notifyMsg_(notifyMsg)
{
    InitializeCriticalSection(&statusLock_);
    status_ = "waiting for Grim Dawn";
}

PipeClient::~PipeClient()
{
    Stop();
    DeleteCriticalSection(&statusLock_);
}

void PipeClient::SetStatus(const char* text)
{
    EnterCriticalSection(&statusLock_);
    status_ = text;
    LeaveCriticalSection(&statusLock_);
}

std::string PipeClient::Status() const
{
    EnterCriticalSection(&statusLock_);
    std::string s = status_;
    LeaveCriticalSection(&statusLock_);
    return s;
}

bool PipeClient::Start()
{
    if (thread_) return true;
    stop_ = 0;
    thread_ = CreateThread(nullptr, 0, &ThreadMain, this, 0, nullptr);
    return thread_ != nullptr;
}

void PipeClient::Stop()
{
    if (!thread_) return;
    InterlockedExchange(&stop_, 1);
    // A blocked ReadFile only returns when the pipe closes or the game exits,
    // so give the thread a bounded wait and let the process tear it down if it
    // is still parked in the read.
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
}

DWORD WINAPI PipeClient::ThreadMain(void* self)
{
    static_cast<PipeClient*>(self)->Run();
    return 0;
}

HANDLE PipeClient::Connect()
{
    for (;;) {
        if (stop_) return INVALID_HANDLE_VALUE;

        // FILE_WRITE_ATTRIBUTES is required by SetNamedPipeHandleState below.
        // With GENERIC_READ alone that call fails, the pipe silently stays in
        // byte mode, and reads then split or coalesce messages instead of
        // delivering them whole - which misframes the stream.
        HANDLE h = CreateFileW(GN_PIPE_NAME, GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) return h;

            // Never continue in byte mode: every later read would be
            // misframed, which is far worse than failing here.
            SetStatus("could not set pipe message mode");
            CloseHandle(h);
            Sleep(1000);
            continue;
        }

        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY) {
            SetStatus("waiting for Grim Dawn");
        } else {
            char buf[96];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "connect failed: %lu", err);
            SetStatus(buf);
        }
        Sleep(1000);
    }
}

// Returns false when the connection should be dropped.
bool PipeClient::ReadOnce(HANDLE pipe)
{
    static unsigned char rbuf[kReadBuf];

    DWORD got = 0;
    if (!ReadFile(pipe, rbuf, kReadBuf, &got, nullptr)) {
        const DWORD err = GetLastError();
        // All three mean "the other end went away", which is normal when the
        // game exits or the server shuts the pipe down.
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_NO_DATA) {
            SetStatus("game disconnected");
        } else {
            char buf[96];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "read failed: %lu", err);
            SetStatus(buf);
        }
        return false;
    }

    // One read can carry a whole batch; walk the records by hdr.size.
    for (DWORD off = 0; off + sizeof(GmHeader) <= got; ) {
        const GmHeader* h = (const GmHeader*)(rbuf + off);
        const DWORD rec = h->size;
        if (rec < sizeof(GmHeader) || off + rec > got) {
            SetStatus("bad record size - dropping connection");
            return false;
        }

        // Copy out so the dispatch can read whole structs even when a record
        // is shorter than the union.
        GmAnyMsg msg = {};
        memcpy(&msg, rbuf + off, rec < sizeof(msg) ? rec : sizeof(msg));
        off += rec;

        if (msg.hdr.magic != GN_PROTO_MAGIC) {
            SetStatus("bad magic - dropping connection");
            return false;
        }
        if (msg.hdr.version != GN_PROTO_VER) {
            char buf[112];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "protocol v%u, expected v%u - rebuild both sides",
                        msg.hdr.version, GN_PROTO_VER);
            SetStatus(buf);
            return false;
        }

        stats_.OnMessage(msg, rec);
    }

    if (notifyWnd_ && notifyMsg_) PostMessage(notifyWnd_, notifyMsg_, 0, 0);
    return true;
}

void PipeClient::Run()
{
    while (!stop_) {
        HANDLE pipe = Connect();
        if (pipe == INVALID_HANDLE_VALUE) break;

        stats_.Reset();
        stats_.SetConnected(true);
        SetStatus("connected");
        if (notifyWnd_ && notifyMsg_) PostMessage(notifyWnd_, notifyMsg_, 0, 0);

        while (!stop_ && ReadOnce(pipe)) {}

        CloseHandle(pipe);
        stats_.SetConnected(false);
        if (notifyWnd_ && notifyMsg_) PostMessage(notifyWnd_, notifyMsg_, 0, 0);
    }
    if (stop_) SetStatus("stopped");
}

} // namespace grimnumbers
