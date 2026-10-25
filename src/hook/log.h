// Debug logging for the in-process DLL. Writes to OutputDebugString (visible
// in DebugView) and appends to GRIMNUMBERS_LOGFILE, defaulting to
// %TEMP%\grimnumbers.log when that variable is not set.
#pragma once

namespace grimnumbers {
void LogInit();
void LogShutdown();
void LogWrite(char level, const char* fmt, ...);
}

#define LOGI(...) ::grimnumbers::LogWrite('I', __VA_ARGS__)
#define LOGW(...) ::grimnumbers::LogWrite('W', __VA_ARGS__)
#define LOGE(...) ::grimnumbers::LogWrite('E', __VA_ARGS__)
