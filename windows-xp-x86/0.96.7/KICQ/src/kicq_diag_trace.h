// Diagnostic trace: minimal metadata, never writes UINs, passwords or message text.
// File: %TEMP%\PMLICQ-x86-Contact-Message-DIAG.txt
#pragma once
static void KicqDiagTrace(const char *stage)
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetTempPathW(_countof(path), path);
    static const wchar_t file[] = L"PMLICQ-x86-Contact-Message-DIAG.txt";
    if (len == 0 || len >= _countof(path) || len + _countof(file) > _countof(path)) return;
    wcscat_s(path, file);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[320] = {};
    mir_snprintf(line, "tick=%lu tid=%lu %s\r\n",
        (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), stage);
    DWORD written = 0;
    WriteFile(h, line, (DWORD)strlen(line), &written, nullptr);
    CloseHandle(h);
}
