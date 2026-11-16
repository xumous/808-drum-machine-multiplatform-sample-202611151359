/* ==========================================================================
 *  main.c —— XU-808 纯 C 版入口
 *
 *  工作很简单：创建工程、初始化 COM 相关对话框、把控制权交给 ui.c。
 *  徐某把它单独放一个文件，是为了让“程序入口”和“界面/引擎”彻底分开，
 *  以后要做命令行模式（例如 --render）时不用动 UI。
 * ========================================================================== */
#include "xu808.h"
#include <windows.h>
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev;
    (void)cmd;
    (void)show;

    /* 工程可能很大（16 patterns × 1024 steps），放堆上，避免栈溢出 */
    XuProject* proj = (XuProject*)calloc(1, sizeof(XuProject));
    if (!proj)
    {
        MessageBoxW(NULL, L"内存不足，无法启动 XU-808。", L"错误", MB_ICONERROR);
        return 1;
    }
    xu_project_default(proj);

    /* 无界面自检：在构建机上验证 DSP/音序器真的算出了声音。
       用法：XU808-C.exe --selftest   → 生成 selftest_c.wav 后退出 */
    if (cmd && strstr(cmd, "--selftest"))
    {
        int ok = xu_export_wav(proj, L"selftest_c.wav", 0, -1);
        /* 不弹窗：构建脚本要能在无人值守的情况下判断成功与否，
           因此把结果写进一个小文本文件。 */
        HANDLE h = CreateFileW(L"selftest_c.txt", GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD w;
            WriteFile(h, ok ? "SELFTEST OK\r\n" : "SELFTEST FAIL\r\n",
                      ok ? 13 : 15, &w, NULL);
            CloseHandle(h);
        }
        free(proj);
        return ok ? 0 : 1;
    }

    int rc = xu_ui_run(hInst, proj);
    free(proj);
    return rc;
}