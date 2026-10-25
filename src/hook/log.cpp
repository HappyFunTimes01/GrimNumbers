#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace grimnumbers {
namespace {
HANDLE          g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_lock;
bool            g_ready = false;
}

void LogInit()
{
    InitializeCriticalSection(&g_lock);
    g_ready = true;

    // GRIMNUMBERS_LOGFILE wins, but a game launched from Steam does not inherit
    // it, so fall back to %TEMP%\grimnumbers.log. The log is the only way to see
    // symbol resolution and hook installation without a debugger attached.
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("GRIMNUMBERS_LOGFILE", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 20)
            strncat_s(path, MAX_PATH, "\\grimnumbers.log", _TRUNCATE);
        else
            n = 0;
    }
    if (n > 0 && n < MAX_PATH) {
        g_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    LogWrite('I', "grimnumbers_hook loaded (pid=%lu)", GetCurrentProcessId());
}

void LogShutdown()
{
    if (!g_ready) return;
    if (g_file != INVALID_HANDLE_VALUE) { CloseHandle(g_file); g_file = INVALID_HANDLE_VALUE; }
    g_ready = false;
    DeleteCriticalSection(&g_lock);
}

void LogWrite(char level, const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[1152];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "[grimnumbers:%c] %s\n", level, body);
    if (len < 0) return;

    OutputDebugStringA(line);

    if (!g_ready) return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, (DWORD)len, &written, nullptr);
    }
    LeaveCriticalSection(&g_lock);
}

} // namespace grimnumbers
