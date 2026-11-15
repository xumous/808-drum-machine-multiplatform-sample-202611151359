/*
 * selfextract.c —— 单文件便携启动器
 * ---------------------------------------------------------------------------
 * 资源 1 中嵌入无压缩归档；首次运行解压到 %LOCALAPPDATA%\808Machine\<app>\，
 * 之后直接启动。用于把 Qt / Java 的整套运行时打包成一个 EXE。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#define IDR_PAYLOAD 1
#define IDI_APP 2

typedef struct {
    char magic[8];
    unsigned int count;
    char app[32];
    char mainExe[64];
} Header;

static void fail(const wchar_t* msg)
{
    MessageBoxW(NULL, msg, L"808 Portable Launcher", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static unsigned int rd32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long rd64(const unsigned char* p)
{
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32);
}

static void make_dirs(const wchar_t* path)
{
    wchar_t tmp[MAX_PATH * 2];
    int i;
    wcsncpy(tmp, path, MAX_PATH * 2 - 1);
    tmp[MAX_PATH * 2 - 1] = 0;
    for (i = 3; tmp[i]; ++i) {
        if (tmp[i] == L'\\' || tmp[i] == L'/') {
            wchar_t c = tmp[i];
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    HRSRC res;
    HGLOBAL glob;
    const unsigned char* data;
    DWORD size;
    const Header* hdr;
    const unsigned char* table;
    const unsigned char* tableEnd;
    wchar_t base[MAX_PATH * 2];
    wchar_t root[MAX_PATH * 2];
    wchar_t marker[MAX_PATH * 2];
    wchar_t exe[MAX_PATH * 2];
    wchar_t wpath[MAX_PATH * 2];
    char markerData[32];
    char sizeText[32];
    int needExtract = 1;
    FILE* mf;
    unsigned int i;
    (void)inst; (void)prev; (void)cmd; (void)show;

    res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!res) fail(L"缺少嵌入资源");
    glob = LoadResource(NULL, res);
    if (!glob) fail(L"无法加载嵌入资源");
    data = (const unsigned char*)LockResource(glob);
    size = SizeofResource(NULL, res);
    if (!data || size < sizeof(Header)) fail(L"归档损坏");

    hdr = (const Header*)data;
    if (memcmp(hdr->magic, "SX808001", 8) != 0) fail(L"归档版本不匹配");

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        fail(L"无法定位 LOCALAPPDATA");
    swprintf(root, MAX_PATH * 2, L"%ls\\808Machine\\%hs", base, hdr->app);
    swprintf(marker, MAX_PATH * 2, L"%ls\\.extract", root);
    sprintf(sizeText, "%u", (unsigned int)size);

    mf = _wfopen(marker, L"rb");
    if (mf) {
        size_t n = fread(markerData, 1, sizeof(markerData) - 1, mf);
        fclose(mf);
        markerData[n] = 0;
        if (strcmp(markerData, sizeText) == 0) needExtract = 0;
    }

    if (needExtract) {
        table = data + sizeof(Header);
        tableEnd = data + size;
        make_dirs(root);
        for (i = 0; i < hdr->count; ++i) {
            unsigned short plen;
            unsigned int fsize;
            unsigned long long off;
            char name[1024];
            char full[2048];
            const unsigned char* payload;
            FILE* f;
            int j;
            if (table + 2 > tableEnd) break;
            plen = (unsigned short)(table[0] | (table[1] << 8));
            table += 2;
            if (table + 4 + 8 + plen > tableEnd) break;
            fsize = rd32(table);
            off = rd64(table + 4);
            table += 12;
            if (plen >= sizeof(name)) plen = sizeof(name) - 1;
            memcpy(name, table, plen);
            name[plen] = 0;
            table += plen;
            if (off + fsize > size) break;
            payload = data + off;

            MultiByteToWideChar(CP_UTF8, 0, name, -1, wpath, MAX_PATH * 2);
            for (j = 0; wpath[j]; ++j) if (wpath[j] == L'/') wpath[j] = L'\\';
            swprintf(exe, MAX_PATH * 2, L"%ls\\%ls", root, wpath);
            {
                wchar_t dir[MAX_PATH * 2];
                wcsncpy(dir, exe, MAX_PATH * 2 - 1);
                dir[MAX_PATH * 2 - 1] = 0;
                {
                    wchar_t* slash = wcsrchr(dir, L'\\');
                    if (slash) { *slash = 0; make_dirs(dir); }
                }
            }
            f = _wfopen(exe, L"wb");
            if (!f) continue;
            fwrite(payload, 1, fsize, f);
            fclose(f);
        }
        mf = _wfopen(marker, L"wb");
        if (mf) { fwrite(sizeText, 1, strlen(sizeText), mf); fclose(mf); }
    }

    swprintf(exe, MAX_PATH * 2, L"%ls\\%hs", root, hdr->mainExe);
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t cmdline[MAX_PATH * 3];
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        memset(&pi, 0, sizeof(pi));
        swprintf(cmdline, MAX_PATH * 3, L"\"%ls\"", exe);
        if (!CreateProcessW(exe, cmdline, NULL, NULL, FALSE, 0, NULL, root, &si, &pi)) {
            fail(L"启动失败：文件可能被占用或损坏");
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}
