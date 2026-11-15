/*
 * launcher.c —— Java 便携启动器（jlink 运行时 + jar + JNI DLL）
 * 不显示控制台窗口，工作目录固定为自身目录。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    wchar_t dir[MAX_PATH * 2];
    wchar_t exe[MAX_PATH * 2];
    wchar_t line[MAX_PATH * 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD n;
    (void)inst; (void)prev; (void)cmd; (void)show;

    n = GetModuleFileNameW(NULL, dir, MAX_PATH * 2);
    if (n == 0) return 1;
    while (n > 0 && dir[n - 1] != L'\\') n--;
    dir[n ? n - 1 : 0] = 0;

    swprintf(exe, MAX_PATH * 2, L"%ls\\jre\\bin\\java.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"缺少 jre 运行时目录", L"808 Java", MB_ICONERROR);
        return 1;
    }
    swprintf(line, MAX_PATH * 4,
             L"\"%ls\" -Djava.library.path=\"%ls\\app\" -cp \"%ls\\app\\808.jar\" native808.Main",
             exe, dir, dir);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    {
        SECURITY_ATTRIBUTES sa;
        wchar_t logPath[MAX_PATH * 2];
        HANDLE hlog;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        swprintf(logPath, MAX_PATH * 2, L"%ls\\java.log", dir);
        {
            FILE* lf = _wfopen(logPath, L"w, ccs=UTF-8");
            if (lf) {
                fwprintf(lf, L"DIR=%ls\nCMD=%ls\n", dir, line);
                fclose(lf);
            }
        }
        hlog = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hlog != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = hlog;
            si.hStdError = hlog;
            si.hStdInput = hlog;
        }
        if (!CreateProcessW(NULL, line, NULL, NULL, hlog != INVALID_HANDLE_VALUE,
                            CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
            if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
            MessageBoxW(NULL, L"无法启动 Java 运行时", L"808 Java", MB_ICONERROR);
            return 1;
        }
        if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
