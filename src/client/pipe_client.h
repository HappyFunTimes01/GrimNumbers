// Reads the hook DLL's named pipe on its own thread and feeds a Stats object.
//
// Front ends never touch the pipe: they start one of these, then poll
// Stats::Take() at whatever rate they redraw. Framing errors and
// disconnections are handled here by dropping the connection and retrying, so
// a front end has nothing to recover from.
#pragma once

#include "stats.h"

#include <windows.h>
#include <string>

namespace grimnumbers {

class PipeClient {
public:
    // `notify`, if given, is posted to `notifyWnd` whenever something arrives,
    // so a UI can redraw on data rather than on a timer alone.
    PipeClient(Stats& stats, HWND notifyWnd = nullptr, UINT notifyMsg = 0);
    ~PipeClient();

    PipeClient(const PipeClient&)            = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    bool Start();
    void Stop();

    // Last error or state text, for a status line. Thread safe.
    std::string Status() const;

private:
    static DWORD WINAPI ThreadMain(void* self);
    void   Run();
    HANDLE Connect();
    void   SetStatus(const char* text);
    bool   ReadOnce(HANDLE pipe);

    Stats& stats_;
    HWND   notifyWnd_;
    UINT   notifyMsg_;

    HANDLE thread_ = nullptr;
    volatile LONG stop_ = 0;

    mutable CRITICAL_SECTION statusLock_;
    std::string status_;
};

} // namespace grimnumbers
