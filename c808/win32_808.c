/*
 * win32_808.c —— 纯 C + Win32 GDI + WASAPI 的 808 鼓机
 * ---------------------------------------------------------------------------
 * - 音频：WASAPI 共享模式 + 事件驱动 + MMCSS("Pro Audio") 高优先级线程
 * - UI  ：单窗口自绘（GDI 双缓冲），固定布局，33ms 刷新，与音频线程零耦合
 * - 所有音序/DSP 在 core808.c，UI 到音频线程的参数通过无锁命令队列
 * - 纯 Win32 API，无第三方运行时；MinGW 静态链接生成单个 EXE
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "core808.h"

/* ------------------------------------------------------------------------- */
/* 资源与常量                                                                 */
/* ------------------------------------------------------------------------- */
#define IDI_APP 101
#define WM_EXPORT_DONE (WM_APP + 1)
#define TIMER_UI 1

#define COL_BG      RGB(14, 16, 20)
#define COL_PANEL   RGB(26, 29, 35)
#define COL_PANEL2  RGB(36, 40, 48)
#define COL_PANEL3  RGB(47, 52, 62)
#define COL_TEXT    RGB(228, 231, 237)
#define COL_DIM     RGB(139, 146, 160)
#define COL_ORANGE  RGB(255, 122, 48)
#define COL_ORANGE2 RGB(122, 58, 24)
#define COL_GREEN   RGB(70, 196, 106)
#define COL_RED     RGB(229, 72, 77)
#define COL_BLUE    RGB(86, 156, 245)
#define COL_CELL    RGB(42, 46, 55)

static const wchar_t* TRACK_NAMES[C808_TRACKS] = {
    L"KICK", L"SNARE", L"CLAP", L"RIM", L"TOM LO", L"TOM MID", L"TOM HI", L"CONGA",
    L"COWBELL", L"SHAKER", L"TAMB", L"CRASH", L"RIDE", L"HAT OPEN", L"HAT CLOSED", L"CLAVE"
};
static const wchar_t* KNOB_NAMES[C808_P_COUNT] = {
    L"TUNE", L"DECAY", L"TONE", L"CHAR", L"LEVEL", L"PAN"
};
static const wchar_t* MASTER_NAMES[C808_M_COUNT] = {
    L"VOLUME", L"DRIVE", L"HP", L"LP", L"RESO", L"ACCENT"
};
static const wchar_t* COND_NAMES[] = {
    L"ALWAYS", L"1:2", L"1:3", L"1:4", L"FILL", L"NOT FILL", L"EVERY N"
};

/* ------------------------------------------------------------------------- */
/* 全局状态                                                                   */
/* ------------------------------------------------------------------------- */
static HINSTANCE g_inst;
static HWND g_hwnd;
static c808_engine* g_engine;
static int g_scale = 100;

static HFONT g_fTitle, g_fLabel, g_fSmall, g_fMono, g_fBig, g_fTiny;

/* 音频 */
static int g_audioOk;
static HANDLE g_audioThread;
static HANDLE g_audioEvent;
static IAudioClient* g_client;
static IAudioRenderClient* g_renderClient;
static WAVEFORMATEX* g_devFormat;
static UINT32 g_bufFrames;
static int g_devChannels, g_devBits, g_devFloat;
static double g_devSr;
static float* g_audioL;
static float* g_audioR;
static volatile LONG g_audioStop;
static wchar_t g_audioInfo[160];

/* UI */
static c808_status g_status;
static int g_selTrack = 0;
static int g_selStep = 0;
static int g_page = 0;
static int g_hoverStep = -1, g_hoverTrack = -1;
static int g_dragId = -1;
static int g_dragY;
static float g_dragStart;
static wchar_t g_msg[256] = L"READY";
static volatile LONG g_exportBusy;
static volatile LONG g_exportResult;
static int g_midiIndex = -1;
static wchar_t g_midiName[128] = L"MIDI: OFF";

/* 布局矩形 */
static RECT rcPlay, rcStop, rcTap, rcBpm, rcBpmMinus, rcBpmPlus, rcSwing;
static RECT rcPatPrev, rcPatNext, rcPatDisp, rcLenMinus, rcLenPlus, rcLenDisp;
static RECT rcSong, rcSave, rcLoad, rcWav, rcMidi, rcStems, rcClear, rcCopy, rcSongEdit, rcMidiIn;
static RECT rcSync, rcTimeSig, rcTrkLenM, rcTrkLenDisp, rcTrkLenP;
static RECT rcGrid, rcGridSteps, rcPanel, rcMaster, rcPads, rcSongStrip, rcStatus;
static RECT rcVoiceKnobs[C808_P_COUNT];
static RECT rcMasterKnobs[C808_M_COUNT];
static RECT rcStepSliders[5];
static RECT rcCond, rcAccent, rcTrigger, rcPad[16], rcPatQuick[16];
static RECT rcPagePrev, rcPageNext;

static int S(int v) { return (int)((long long)v * g_scale / 100); }

/* ------------------------------------------------------------------------- */
/* 基础绘图                                                                   */
/* ------------------------------------------------------------------------- */
static void fill_rect(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void fill_round(HDC dc, RECT r, int rad, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void frame_round(HDC dc, RECT r, int rad, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, S(1), c);
    HBRUSH b = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, b);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void draw_text(HDC dc, RECT r, const wchar_t* s, COLORREF c, HFONT f, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void draw_text_center(HDC dc, RECT r, const wchar_t* s, COLORREF c, HFONT f)
{
    draw_text(dc, r, s, c, f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_button(HDC dc, RECT r, const wchar_t* label, int active, HFONT f)
{
    RECT t = r;
    if (active)
    {
        fill_round(dc, r, S(7), COL_ORANGE);
        frame_round(dc, r, S(7), COL_ORANGE);
        draw_text_center(dc, r, label, RGB(20, 16, 12), f);
    }
    else
    {
        fill_round(dc, r, S(7), COL_PANEL2);
        frame_round(dc, r, S(7), COL_PANEL3);
        draw_text_center(dc, r, label, COL_TEXT, f);
    }
    (void)t;
}

static float value_from_knob_rect(RECT r, int y)
{
    float t = (float)(r.bottom - S(6) - y) / (float)(r.bottom - r.top - S(20));
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

static void draw_knob(HDC dc, RECT r, const wchar_t* label, float v)
{
    int cx = (r.left + r.right) / 2;
    int rad = (r.bottom - r.top - S(38)) / 2;
    int cy;
    int i;
    POINT pts[40];
    int n = 0;
    HBRUSH b;
    HPEN p;
    HGDIOBJ ob, op;
    float ang;
    wchar_t buf[32];

    if (rad > S(20)) rad = S(20);
    if (rad < S(8)) rad = S(8);
    cy = r.top + S(4) + rad;
    b = CreateSolidBrush(COL_PANEL3);
    p = CreatePen(PS_SOLID, S(4), COL_ORANGE2);
    ob = SelectObject(dc, b);
    op = SelectObject(dc, p);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);

    p = CreatePen(PS_SOLID, S(3), COL_ORANGE);
    op = SelectObject(dc, p);
    for (i = 0; i <= 32; ++i)
    {
        double a = (135.0 + 270.0 * i / 32.0) * 3.14159265358979 / 180.0;
        pts[n].x = cx + (int)(cos(a) * (rad + S(5)));
        pts[n].y = cy - (int)(sin(a) * (rad + S(5)));
        n++;
    }
    Polyline(dc, pts, n);
    SelectObject(dc, op);
    DeleteObject(p);

    ang = (float)((135.0 + 270.0 * v) * 3.14159265358979 / 180.0);
    p = CreatePen(PS_SOLID, S(3), COL_TEXT);
    op = SelectObject(dc, p);
    MoveToEx(dc, cx, cy, NULL);
    LineTo(dc, cx + (int)(cos(ang) * (rad - S(4))), cy - (int)(sin(ang) * (rad - S(4))));
    SelectObject(dc, op);
    DeleteObject(p);

    swprintf(buf, 32, L"%d", (int)(v * 100.0f + 0.5f));
    {
        RECT lr = {r.left, cy + rad + S(1), r.right, cy + rad + S(15)};
        draw_text_center(dc, lr, label, COL_DIM, g_fTiny);
        lr.top = lr.bottom;
        lr.bottom += S(14);
        draw_text_center(dc, lr, buf, COL_TEXT, g_fSmall);
    }
}

static void draw_slider(HDC dc, RECT r, const wchar_t* label, float v, const wchar_t* val,
                        int bipolar)
{
    RECT bar = r;
    RECT lab = r;
    RECT valr = r;
    int w;
    lab.right = lab.left + S(72);
    valr.left = valr.right - S(56);
    bar.left += S(78);
    bar.right -= S(60);
    bar.top += S(7);
    bar.bottom -= S(7);
    draw_text(dc, lab, label, COL_DIM, g_fSmall, DT_VCENTER | DT_SINGLELINE);
    draw_text(dc, valr, val, COL_TEXT, g_fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fill_round(dc, bar, S(4), COL_PANEL3);
    w = bar.right - bar.left;
    if (bipolar)
    {
        int mid = (bar.left + bar.right) / 2;
        int x = bar.left + (int)(v * w);
        RECT f;
        f.top = bar.top;
        f.bottom = bar.bottom;
        if (x >= mid)
        {
            f.left = mid;
            f.right = x;
        }
        else
        {
            f.left = x;
            f.right = mid;
        }
        if (f.right > f.left) fill_rect(dc, f, COL_ORANGE);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, COL_DIM);
        {
            HPEN p = CreatePen(PS_SOLID, S(1), COL_DIM);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, mid, bar.top - S(2), NULL);
            LineTo(dc, mid, bar.bottom + S(2));
            SelectObject(dc, op);
            DeleteObject(p);
        }
    }
    else
    {
        RECT f = bar;
        f.right = bar.left + (int)(v * w);
        fill_rect(dc, f, COL_ORANGE);
    }
}

static void draw_meter(HDC dc, RECT r, float v)
{
    int h = r.bottom - r.top;
    int fh = (int)(v * h);
    fill_rect(dc, r, COL_PANEL3);
    if (fh > 0)
    {
        RECT f = r;
        f.top = r.bottom - fh;
        fill_rect(dc, f, v > 0.9f ? COL_RED : (v > 0.6f ? COL_ORANGE : COL_GREEN));
    }
}

/* ------------------------------------------------------------------------- */
/* 布局                                                                       */
/* ------------------------------------------------------------------------- */
static void create_fonts(void)
{
    if (g_fTitle) DeleteObject(g_fTitle);
    if (g_fLabel) DeleteObject(g_fLabel);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fTiny) DeleteObject(g_fTiny);
    if (g_fMono) DeleteObject(g_fMono);
    if (g_fBig) DeleteObject(g_fBig);

    g_fTitle = CreateFontW(-S(20), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    g_fLabel = CreateFontW(-S(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    g_fSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    g_fTiny = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    g_fMono = CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          FIXED_PITCH, L"Consolas");
    g_fBig = CreateFontW(-S(28), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
}

/* 根据客户区大小重新计算整体缩放并布局（窗口可自由缩放，始终完整显示） */
static void build_layout(HWND hwnd);

static void update_layout(HWND hwnd)
{
    RECT cr;
    int sx, sy, ns;
    GetClientRect(hwnd, &cr);
    if (cr.right < 10 || cr.bottom < 10) return;
    sx = cr.right * 100 / 1280;
    sy = cr.bottom * 100 / 720;
    ns = sx < sy ? sx : sy;
    if (ns < 45) ns = 45;
    if (ns > 300) ns = 300;
    if (ns != g_scale)
    {
        g_scale = ns;
        create_fonts();
    }
    build_layout(hwnd);
}

static void set_rect(RECT* r, int x, int y, int w, int h)
{
    r->left = S(x);
    r->top = S(y);
    r->right = S(x + w);
    r->bottom = S(y + h);
}

static void build_layout(HWND hwnd)
{
    RECT cr;
    int W, H, i;
    GetClientRect(hwnd, &cr);
    W = cr.right;
    H = cr.bottom;

    set_rect(&rcPlay, 12, 8, 72, 30);
    set_rect(&rcStop, 90, 8, 72, 30);
    set_rect(&rcTap, 168, 8, 60, 30);
    set_rect(&rcBpmMinus, 234, 8, 30, 30);
    set_rect(&rcBpm, 268, 8, 86, 30);
    set_rect(&rcBpmPlus, 358, 8, 30, 30);
    set_rect(&rcSwing, 400, 8, 190, 30);
    set_rect(&rcPatPrev, 600, 8, 30, 30);
    set_rect(&rcPatDisp, 634, 8, 92, 30);
    set_rect(&rcPatNext, 730, 8, 30, 30);
    set_rect(&rcLenMinus, 772, 8, 30, 30);
    set_rect(&rcLenDisp, 806, 8, 70, 30);
    set_rect(&rcLenPlus, 880, 8, 30, 30);
    set_rect(&rcSong, 922, 8, 72, 30);
    set_rect(&rcSongEdit, 1000, 8, 84, 30);

    set_rect(&rcSave, 12, 44, 70, 28);
    set_rect(&rcLoad, 86, 44, 70, 28);
    set_rect(&rcWav, 160, 44, 70, 28);
    set_rect(&rcMidi, 234, 44, 70, 28);
    set_rect(&rcStems, 308, 44, 76, 28);
    set_rect(&rcClear, 388, 44, 64, 28);
    set_rect(&rcCopy, 456, 44, 64, 28);
    set_rect(&rcMidiIn, 524, 44, 240, 28);
    set_rect(&rcSync, 772, 44, 76, 28);
    set_rect(&rcTimeSig, 854, 44, 70, 28);
    set_rect(&rcTrkLenM, 860, 486, 32, 26);
    set_rect(&rcTrkLenDisp, 896, 486, 128, 26);
    set_rect(&rcTrkLenP, 1028, 486, 32, 26);

    set_rect(&rcGrid, 12, 84, 830, 432);
    set_rect(&rcPanel, 852, 84, 416, 432);
    set_rect(&rcMaster, 12, 526, 830, 106);
    set_rect(&rcPads, 852, 526, 416, 106);

    set_rect(&rcSongStrip, 12, 640, 1256, 40);
    set_rect(&rcStatus, 0, 686, W, 34);

    for (i = 0; i < C808_P_COUNT; ++i)
    {
        int col = i % 3, row = i / 3;
        set_rect(&rcVoiceKnobs[i], 862 + col * 134, 118 + row * 80, 128, 78);
    }
    for (i = 0; i < C808_M_COUNT; ++i)
    {
        set_rect(&rcMasterKnobs[i], 30 + i * 134, 542, 128, 86);
    }
    for (i = 0; i < 5; ++i)
    {
        set_rect(&rcStepSliders[i], 860, 316 + i * 25, 400, 24);
    }
    set_rect(&rcCond, 860, 452, 210, 28);
    set_rect(&rcAccent, 1080, 452, 180, 28);
    set_rect(&rcTrigger, 1090, 94, 168, 24);

    for (i = 0; i < 16; ++i)
    {
        int col = i % 4, row = i / 4;
        set_rect(&rcPad[i], 858 + col * 102, 530 + row * 25, 98, 22);
    }
    for (i = 0; i < 16; ++i)
    {
        set_rect(&rcPatQuick[i], 34 + i * 76, 644, 70, 30);
    }
    set_rect(&rcPagePrev, 14, 644, 16, 30);
    set_rect(&rcPageNext, 1252, 644, 16, 30);
    set_rect(&rcGridSteps, rcGrid.left + S(126), rcGrid.top + S(22),
             rcGrid.right - S(8), rcGrid.bottom - S(6));
    (void)H;
}

/* ------------------------------------------------------------------------- */
/* 音频：WASAPI                                                               */
/* ------------------------------------------------------------------------- */
static void device_info_from_format(WAVEFORMATEX* wf)
{
    g_devChannels = wf->nChannels;
    g_devBits = wf->wBitsPerSample;
    g_devFloat = 0;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) g_devFloat = 1;
    else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        WAVEFORMATEXTENSIBLE* ex = (WAVEFORMATEXTENSIBLE*)wf;
        /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT 的 Data1 == 3，避免额外链接 GUID 库 */
        if (ex->SubFormat.Data1 == 3) g_devFloat = 1;
    }
    g_devSr = (double)wf->nSamplesPerSec;
}

static void write_device(BYTE* out, const float* L, const float* R, int n)
{
    int ch = g_devChannels;
    int i, c;
    if (ch < 1) ch = 2;
    if (g_devFloat && g_devBits == 32)
    {
        float* o = (float*)out;
        for (i = 0; i < n; ++i)
        {
            o[i * ch] = L[i];
            if (ch > 1) o[i * ch + 1] = R[i];
            for (c = 2; c < ch; ++c) o[i * ch + c] = 0.0f;
        }
    }
    else if (g_devBits == 16)
    {
        short* o = (short*)out;
        for (i = 0; i < n; ++i)
        {
            o[i * ch] = (short)(L[i] * 32767.0f);
            if (ch > 1) o[i * ch + 1] = (short)(R[i] * 32767.0f);
            for (c = 2; c < ch; ++c) o[i * ch + c] = 0;
        }
    }
    else if (g_devBits == 32)
    {
        int* o = (int*)out;
        for (i = 0; i < n; ++i)
        {
            o[i * ch] = (int)(L[i] * 2147483647.0f);
            if (ch > 1) o[i * ch + 1] = (int)(R[i] * 2147483647.0f);
            for (c = 2; c < ch; ++c) o[i * ch + c] = 0;
        }
    }
    else if (g_devBits == 24)
    {
        unsigned char* o = out;
        for (i = 0; i < n; ++i)
        {
            int v0 = (int)(L[i] * 8388607.0f);
            int v1 = (int)(R[i] * 8388607.0f);
            int base = i * ch * 3;
            int k;
            for (k = 0; k < 3; ++k)
            {
                o[base + k] = (unsigned char)((v0 >> (8 * k)) & 0xff);
                if (ch > 1) o[base + 3 + k] = (unsigned char)((v1 >> (8 * k)) & 0xff);
            }
            for (c = 2; c < ch; ++c)
            {
                o[base + c * 3] = 0;
                o[base + c * 3 + 1] = 0;
                o[base + c * 3 + 2] = 0;
            }
        }
    }
}

static DWORD WINAPI audio_thread(LPVOID param)
{
    HANDLE task;
    DWORD taskIndex = 0;
    LARGE_INTEGER freq, t0, t1;
    (void)param;
    task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    QueryPerformanceFrequency(&freq);
    while (!g_audioStop)
    {
        DWORD w = WaitForSingleObject(g_audioEvent, 200);
        UINT32 padding = 0, avail;
        BYTE* data = NULL;
        if (w != WAIT_OBJECT_0) continue;
        if (FAILED(IAudioClient_GetCurrentPadding(g_client, &padding))) continue;
        avail = g_bufFrames - padding;
        if (avail == 0) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(g_renderClient, avail, &data))) continue;
        QueryPerformanceCounter(&t0);
        c808_render(g_engine, g_audioL, g_audioR, (int)avail);
        write_device(data, g_audioL, g_audioR, (int)avail);
        IAudioRenderClient_ReleaseBuffer(g_renderClient, avail, 0);
        QueryPerformanceCounter(&t1);
        {
            double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            double budget = (double)g_bufFrames / (g_devSr > 0 ? g_devSr : 48000.0);
            float load = (float)(sec / (budget > 1e-9 ? budget : 1e-9));
            c808_report_cpu(g_engine, load);
            if (load > 0.98f) c808_report_xrun(g_engine);
        }
    }
    if (task) AvRevertMmThreadCharacteristics(task);
    return 0;
}

static void audio_shutdown(void)
{
    if (g_audioThread)
    {
        InterlockedExchange(&g_audioStop, 1);
        if (g_audioEvent) SetEvent(g_audioEvent);
        WaitForSingleObject(g_audioThread, 2000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    if (g_renderClient)
    {
        IAudioRenderClient_Release(g_renderClient);
        g_renderClient = NULL;
    }
    if (g_client)
    {
        IAudioClient_Stop(g_client);
        IAudioClient_Release(g_client);
        g_client = NULL;
    }
    if (g_audioEvent)
    {
        CloseHandle(g_audioEvent);
        g_audioEvent = NULL;
    }
    if (g_devFormat)
    {
        CoTaskMemFree(g_devFormat);
        g_devFormat = NULL;
    }
    if (g_audioL)
    {
        free(g_audioL);
        g_audioL = NULL;
    }
    if (g_audioR)
    {
        free(g_audioR);
        g_audioR = NULL;
    }
}

static int audio_init(HWND hwnd)
{
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    HRESULT hr;
    REFERENCE_TIME dur = 100000; /* 10ms */
    UINT32 i;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) goto fail;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) goto fail;
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&g_client);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetMixFormat(g_client, &g_devFormat);
    if (FAILED(hr)) goto fail;
    device_info_from_format(g_devFormat);

    hr = IAudioClient_Initialize(g_client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 dur, 0, g_devFormat, NULL);
    if (FAILED(hr)) goto fail;
    g_audioEvent = CreateEventW(NULL, 0, FALSE, NULL);
    if (!g_audioEvent) goto fail;
    hr = IAudioClient_SetEventHandle(g_client, g_audioEvent);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetBufferSize(g_client, &g_bufFrames);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetService(g_client, &IID_IAudioRenderClient, (void**)&g_renderClient);
    if (FAILED(hr)) goto fail;

    g_engine = c808_create(g_devSr);
    if (!g_engine) goto fail;
    g_audioL = (float*)malloc(sizeof(float) * (g_bufFrames + 8));
    g_audioR = (float*)malloc(sizeof(float) * (g_bufFrames + 8));
    if (!g_audioL || !g_audioR) goto fail;

    /* 预填充静音，避免起始欠载 */
    {
        BYTE* data = NULL;
        for (i = 0; i < 2; ++i)
        {
            if (SUCCEEDED(IAudioRenderClient_GetBuffer(g_renderClient, g_bufFrames, &data)))
            {
                memset(data, 0, (size_t)g_bufFrames * g_devFormat->nBlockAlign);
                IAudioRenderClient_ReleaseBuffer(g_renderClient, g_bufFrames, 0);
            }
        }
    }

    hr = IAudioClient_Start(g_client);
    if (FAILED(hr)) goto fail;
    g_audioThread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (!g_audioThread) goto fail;

    swprintf(g_audioInfo, 160, L"WASAPI %.0f Hz · %d ch · %d-bit · %u frames",
             g_devSr, g_devChannels, g_devBits, g_bufFrames);
    g_audioOk = 1;
    if (en) IMMDeviceEnumerator_Release(en);
    if (dev) IMMDevice_Release(dev);
    return 0;

fail:
    if (g_audioThread)
    {
        InterlockedExchange(&g_audioStop, 1);
        WaitForSingleObject(g_audioThread, 1000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    if (g_renderClient)
    {
        IAudioRenderClient_Release(g_renderClient);
        g_renderClient = NULL;
    }
    if (g_client)
    {
        IAudioClient_Release(g_client);
        g_client = NULL;
    }
    if (g_audioEvent)
    {
        CloseHandle(g_audioEvent);
        g_audioEvent = NULL;
    }
    if (g_devFormat)
    {
        CoTaskMemFree(g_devFormat);
        g_devFormat = NULL;
    }
    if (en) IMMDeviceEnumerator_Release(en);
    if (dev) IMMDevice_Release(dev);
    if (g_engine)
    {
        c808_destroy(g_engine);
        g_engine = NULL;
    }
    g_audioOk = 0;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* 工程/导出对话框与后台线程                                                  */
/* ------------------------------------------------------------------------- */
typedef struct
{
    char path[1024];
    int kind; /* 0=wav, 1=midi, 2=stems */
    int mode;
    int loops;
} ExportJob;

static DWORD WINAPI export_thread(LPVOID p)
{
    ExportJob* j = (ExportJob*)p;
    int rc = 0;
    if (j->kind == 1) rc = c808_export_midi(g_engine, j->path, j->mode, j->loops);
    else rc = c808_export_wav(g_engine, j->path, j->mode, j->loops, 2, j->kind == 2);
    free(j);
    InterlockedExchange(&g_exportResult, rc);
    InterlockedExchange(&g_exportBusy, 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_EXPORT_DONE, 0, 0);
    return 0;
}

static void utf8_from_wide(const wchar_t* w, char* out, int cap)
{
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

static void do_save(void)
{
    OPENFILENAMEW o;
    wchar_t file[MAX_PATH] = L"808-project.808";
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"808 Project (*.808)\0*.808\0All Files\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    o.lpstrDefExt = L"808";
    if (GetSaveFileNameW(&o))
    {
        char u8[MAX_PATH * 3];
        utf8_from_wide(file, u8, sizeof(u8));
        c808_save_project(g_engine, u8);
        swprintf(g_msg, 256, L"SAVED: %ls", file);
    }
}

static void do_load(void)
{
    OPENFILENAMEW o;
    wchar_t file[MAX_PATH] = L"";
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"808 Project (*.808)\0*.808\0All Files\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&o))
    {
        char u8[MAX_PATH * 3];
        utf8_from_wide(file, u8, sizeof(u8));
        if (c808_load_project(g_engine, u8) == 0) swprintf(g_msg, 256, L"LOADED: %ls", file);
        else swprintf(g_msg, 256, L"LOAD FAILED: %ls", file);
    }
}

static void start_export(int kind)
{
    OPENFILENAMEW o;
    wchar_t file[MAX_PATH];
    ExportJob* j;
    char u8[MAX_PATH * 3];
    HANDLE th;
    if (InterlockedCompareExchange(&g_exportBusy, 1, 0) != 0)
    {
        swprintf(g_msg, 256, L"EXPORT ALREADY RUNNING");
        return;
    }
    if (kind == 1) wcscpy(file, L"808-pattern.mid");
    else if (kind == 2) wcscpy(file, L"808-stems.wav");
    else wcscpy(file, L"808-pattern.wav");

    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = kind == 1 ? L"MIDI (*.mid)\0*.mid\0" : L"WAV (*.wav)\0*.wav\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    o.lpstrDefExt = kind == 1 ? L"mid" : L"wav";
    if (!GetSaveFileNameW(&o))
    {
        InterlockedExchange(&g_exportBusy, 0);
        return;
    }

    j = (ExportJob*)calloc(1, sizeof(ExportJob));
    if (!j)
    {
        InterlockedExchange(&g_exportBusy, 0);
        return;
    }
    utf8_from_wide(file, u8, sizeof(u8));
    strncpy(j->path, u8, sizeof(j->path) - 1);
    j->kind = kind;
    j->mode = g_engine ? (c808_project_ro(g_engine)->songOn ? 1 : 0) : 0;
    j->loops = 2;
    swprintf(g_msg, 256, L"EXPORTING...");
    th = CreateThread(NULL, 0, export_thread, j, 0, NULL);
    if (th) CloseHandle(th);
    else
    {
        free(j);
        InterlockedExchange(&g_exportBusy, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* Song 编辑器（模态）                                                        */
/* ------------------------------------------------------------------------- */
typedef struct
{
    int pat, reps;
} SongItem;

static SongItem g_song[512];
static int g_songLen;
static HWND g_songDlg, g_songList, g_songParent;

static void song_refresh(void)
{
    int i;
    SendMessageW(g_songList, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_songLen; ++i)
    {
        wchar_t b[64];
        swprintf(b, 64, L"%02d   PATTERN %3d   x%d", i + 1, g_song[i].pat, g_song[i].reps);
        SendMessageW(g_songList, LB_ADDSTRING, 0, (LPARAM)b);
    }
}

static void song_add(int pat)
{
    if (g_songLen >= 512) return;
    g_song[g_songLen].pat = pat;
    g_song[g_songLen].reps = 4;
    g_songLen++;
    song_refresh();
}

static LRESULT CALLBACK song_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_COMMAND:
        {
            int id = LOWORD(w);
            int sel = (int)SendMessageW(g_songList, LB_GETCURSEL, 0, 0);
            if (id == 1001)
            {
                /* add */
                int pat = g_songLen ? g_song[g_songLen - 1].pat : 0;
                song_add(pat);
            }
            else if (id == 1002 && sel >= 0 && g_songLen > 0)
            {
                /* delete */
                int i;
                for (i = sel; i < g_songLen - 1; ++i) g_song[i] = g_song[i + 1];
                g_songLen--;
                song_refresh();
            }
            else if (id == 1003 && sel > 0)
            {
                /* up */
                SongItem t = g_song[sel];
                g_song[sel] = g_song[sel - 1];
                g_song[sel - 1] = t;
                SendMessageW(g_songList, LB_SETCURSEL, sel - 1, 0);
                song_refresh();
            }
            else if (id == 1004 && sel >= 0 && sel < g_songLen - 1)
            {
                /* down */
                SongItem t = g_song[sel];
                g_song[sel] = g_song[sel + 1];
                g_song[sel + 1] = t;
                SendMessageW(g_songList, LB_SETCURSEL, sel + 1, 0);
                song_refresh();
            }
            else if (id == 1005 && sel >= 0 && sel < g_songLen)
            {
                /* pat - */
                if (--g_song[sel].pat < 0) g_song[sel].pat = C808_PATTERNS - 1;
                song_refresh();
            }
            else if (id == 1006 && sel >= 0 && sel < g_songLen)
            {
                /* pat + */
                g_song[sel].pat = (g_song[sel].pat + 1) % C808_PATTERNS;
                song_refresh();
            }
            else if (id == 1007 && sel >= 0 && sel < g_songLen)
            {
                /* reps - */
                if (g_song[sel].reps > 1) g_song[sel].reps--;
                song_refresh();
            }
            else if (id == 1008 && sel >= 0 && sel < g_songLen)
            {
                /* reps + */
                if (g_song[sel].reps < 64) g_song[sel].reps++;
                song_refresh();
            }
            else if (id == 1010)
            {
                /* ok */
                int i;
                c808_set_song_len(g_engine, g_songLen);
                for (i = 0; i < g_songLen; ++i)
                    c808_set_song_entry(g_engine, i, g_song[i].pat, g_song[i].reps);
                DestroyWindow(g_songDlg);
            }
            else if (id == 1011)
            {
                DestroyWindow(g_songDlg);
            }
            return 0;
        }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        EnableWindow(g_songParent, TRUE);
        g_songDlg = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void open_song_editor(void)
{
    static const wchar_t* btn[] = {
        L"+ ADD", L"- DEL", L"UP", L"DOWN", L"PAT -", L"PAT +", L"REP -", L"REP +"
    };
    static const int ids[] = {1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008};
    int i;
    WNDCLASSW wc;
    RECT cr;
    const c808_project* p = c808_project_ro(g_engine);
    if (g_songDlg)
    {
        SetForegroundWindow(g_songDlg);
        return;
    }

    g_songLen = p ? p->songLen : 1;
    if (g_songLen < 1) g_songLen = 1;
    if (g_songLen > 512) g_songLen = 512;
    for (i = 0; i < g_songLen; ++i)
    {
        g_song[i].pat = p->song[i].pat;
        g_song[i].reps = p->song[i].reps ? p->song[i].reps : 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = song_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COL_PANEL);
    wc.lpszClassName = L"Song808Dlg";
    RegisterClassW(&wc);

    g_songParent = g_hwnd;
    g_songDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Song808Dlg", L"SONG CHAIN EDITOR",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, S(560), S(520),
                                g_hwnd, NULL, g_inst, NULL);
    if (!g_songDlg) return;
    EnableWindow(g_hwnd, FALSE);
    GetClientRect(g_songDlg, &cr);

    g_songList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                                 S(14), S(14), S(530), S(360), g_songDlg, (HMENU)2000, g_inst, NULL);
    for (i = 0; i < 8; ++i)
    {
        CreateWindowW(L"BUTTON", btn[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      S(14 + (i % 4) * 132), S(384 + (i / 4) * 38), S(124), S(32),
                      g_songDlg, (HMENU)(INT_PTR)ids[i], g_inst, NULL);
    }
    CreateWindowW(L"BUTTON", L"APPLY & CLOSE", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                  S(14), S(464), S(250), S(36), g_songDlg, (HMENU)1010, g_inst, NULL);
    CreateWindowW(L"BUTTON", L"CANCEL", WS_CHILD | WS_VISIBLE,
                  S(274), S(464), S(150), S(36), g_songDlg, (HMENU)1011, g_inst, NULL);
    song_refresh();
    ShowWindow(g_songDlg, SW_SHOW);
    UpdateWindow(g_songDlg);
    (void)cr;
}

/* ------------------------------------------------------------------------- */
/* MIDI 设备循环                                                              */
/* ------------------------------------------------------------------------- */
static void cycle_midi_device(void)
{
    int n = c808_midi_device_count();
    if (n <= 0)
    {
        g_midiIndex = -1;
        wcscpy(g_midiName, L"MIDI: NONE");
        swprintf(g_msg, 256, L"NO MIDI INPUT DEVICE");
        return;
    }
    g_midiIndex++;
    if (g_midiIndex >= n)
    {
        g_midiIndex = -1;
        c808_midi_stop(g_engine);
        wcscpy(g_midiName, L"MIDI: OFF");
        swprintf(g_msg, 256, L"MIDI INPUT DISABLED");
        return;
    }
    if (c808_midi_start(g_engine, g_midiIndex) == 0)
    {
        wchar_t nm[128];
        char u8[256];
        c808_midi_device_name(g_midiIndex, u8, sizeof(u8));
        MultiByteToWideChar(CP_UTF8, 0, u8, -1, nm, 128);
        swprintf(g_midiName, 128, L"MIDI: %ls", nm);
        swprintf(g_msg, 256, L"MIDI INPUT: %ls", nm);
    }
    else
    {
        wcscpy(g_midiName, L"MIDI: ERROR");
    }
}

/* ------------------------------------------------------------------------- */
/* 主人机交互                                                                 */
/* ------------------------------------------------------------------------- */
static int point_in(RECT r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void cycle_prob(int track, int step)
{
    const c808_pattern* pat = &c808_project_ro(g_engine)->patterns[c808_project_ro(g_engine)->currentPattern];
    static const int seq[] = {100, 75, 50, 25, 10, 0};
    int cur = pat->prob[track][step];
    int i, next = 100;
    for (i = 0; i < 6; ++i)
    {
        if (seq[i] == cur)
        {
            next = seq[(i + 1) % 6];
            break;
        }
    }
    c808_set_step_prob(g_engine, c808_project_ro(g_engine)->currentPattern, track, step, next);
}

static void cycle_ratchet(int track, int step, int delta)
{
    const c808_project* p = c808_project_ro(g_engine);
    int cur = p->patterns[p->currentPattern].ratchet[track][step];
    cur += delta;
    if (cur < 1) cur = 8;
    if (cur > 8) cur = 1;
    c808_set_step_ratchet(g_engine, p->currentPattern, track, step, cur);
}

static void grid_step_at(int x, int y, int* track, int* step)
{
    const c808_project* p = c808_project_ro(g_engine);
    int len = p ? p->patterns[p->currentPattern].len : 16;
    int rowH = (rcGridSteps.bottom - rcGridSteps.top) / 16;
    int colW;
    int t, s;
    *track = -1;
    *step = -1;
    if (len < 1) len = 16;
    colW = (rcGridSteps.right - rcGridSteps.left) / len;
    if (!point_in(rcGridSteps, x, y)) return;
    t = (y - rcGridSteps.top) / rowH;
    s = (x - rcGridSteps.left) / colW;
    if (t < 0 || t >= 16) return;
    if (s < 0 || s >= len) return;
    *track = t;
    *step = s;
}

static void set_step_slider(int idx, int x)
{
    const c808_project* p = c808_project_ro(g_engine);
    int pat = p->currentPattern;
    RECT r = rcStepSliders[idx];
    float t;
    if (p == NULL) return;
    t = (float)(x - (r.left + S(78))) / (float)(r.right - r.left - S(138));
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (idx)
    {
    case 0: c808_set_step_vel(g_engine, pat, g_selTrack, g_selStep, (int)(1 + t * 126));
        break;
    case 1: c808_set_step_prob(g_engine, pat, g_selTrack, g_selStep, (int)(t * 100));
        break;
    case 2: c808_set_step_ratchet(g_engine, pat, g_selTrack, g_selStep, (int)(1 + t * 7));
        break;
    case 3: c808_set_step_rate(g_engine, pat, g_selTrack, g_selStep, (int)(10 + t * 90));
        break;
    case 4: c808_set_step_micro(g_engine, pat, g_selTrack, g_selStep, (int)((t - 0.5f) * 4800.0f));
        break;
    default: break;
    }
}

static void click_handle(int x, int y, int shift, int ctrl, int alt)
{
    const c808_project* p = c808_project_ro(g_engine);
    int t, s;
    if (!p) return;

    if (point_in(rcPlay, x, y))
    {
        if (g_status.playing) c808_transport_stop(g_engine);
        else c808_transport_play(g_engine);
        return;
    }
    if (point_in(rcStop, x, y))
    {
        c808_transport_stop_reset(g_engine);
        return;
    }
    if (point_in(rcTap, x, y))
    {
        static DWORD lastTap;
        static DWORD taps[8];
        static int tapCount, tapPos;
        DWORD now = GetTickCount();
        if (tapCount > 0 && now - lastTap > 2000)
        {
            tapCount = 0;
            tapPos = 0;
        }
        if (tapCount > 0)
        {
            DWORD dt = now - lastTap;
            if (dt > 150 && dt < 1500)
            {
                taps[tapPos % 8] = dt;
                tapPos++;
                tapCount++;
            }
        }
        else { tapCount = 1; }
        lastTap = now;
        if (tapPos >= 2)
        {
            int n = tapPos > 8 ? 8 : tapPos;
            int i;
            DWORD sum = 0;
            for (i = 0; i < n; ++i) sum += taps[i];
            {
                float bpm = 60000.0f / ((float)sum / (float)n);
                c808_set_bpm(g_engine, bpm);
                swprintf(g_msg, 256, L"TAP TEMPO %.1f BPM", bpm);
            }
        }
        return;
    }
    if (point_in(rcBpmMinus, x, y))
    {
        c808_set_bpm(g_engine, p->bpm - 1.0f);
        return;
    }
    if (point_in(rcBpmPlus, x, y))
    {
        c808_set_bpm(g_engine, p->bpm + 1.0f);
        return;
    }
    if (point_in(rcBpm, x, y))
    {
        g_dragId = 100;
        g_dragY = y;
        g_dragStart = p->bpm;
        return;
    }
    if (point_in(rcSwing, x, y))
    {
        int v = (x - rcSwing.left) * 100 / (rcSwing.right - rcSwing.left);
        c808_set_pattern_swing(g_engine, p->currentPattern, v);
        return;
    }
    if (point_in(rcPatPrev, x, y))
    {
        c808_select_pattern(g_engine, (p->currentPattern + C808_PATTERNS - 1) % C808_PATTERNS);
        return;
    }
    if (point_in(rcPatNext, x, y))
    {
        c808_select_pattern(g_engine, (p->currentPattern + 1) % C808_PATTERNS);
        return;
    }
    if (point_in(rcLenMinus, x, y))
    {
        c808_set_pattern_len(g_engine, p->currentPattern, p->patterns[p->currentPattern].len - 1);
        return;
    }
    if (point_in(rcLenPlus, x, y))
    {
        c808_set_pattern_len(g_engine, p->currentPattern, p->patterns[p->currentPattern].len + 1);
        return;
    }
    if (point_in(rcSong, x, y))
    {
        c808_set_song_on(g_engine, !p->songOn);
        return;
    }
    if (point_in(rcSongEdit, x, y))
    {
        open_song_editor();
        return;
    }
    if (point_in(rcSave, x, y))
    {
        do_save();
        return;
    }
    if (point_in(rcLoad, x, y))
    {
        do_load();
        return;
    }
    if (point_in(rcWav, x, y))
    {
        start_export(0);
        return;
    }
    if (point_in(rcMidi, x, y))
    {
        start_export(1);
        return;
    }
    if (point_in(rcStems, x, y))
    {
        start_export(2);
        return;
    }
    if (point_in(rcClear, x, y))
    {
        c808_clear_pattern(g_engine, p->currentPattern);
        return;
    }
    if (point_in(rcCopy, x, y))
    {
        int dst = (p->currentPattern + 1) % C808_PATTERNS;
        c808_copy_pattern(g_engine, p->currentPattern, dst);
        c808_select_pattern(g_engine, dst);
        swprintf(g_msg, 256, L"COPIED PATTERN %d -> %d", p->currentPattern, dst);
        return;
    }
    if (point_in(rcMidiIn, x, y))
    {
        cycle_midi_device();
        return;
    }
    if (point_in(rcSync, x, y))
    {
        c808_set_clock_source(g_engine, !p->clockSource);
        return;
    }
    if (point_in(rcTimeSig, x, y))
    {
        static const int sigs[][2] = {{4, 4}, {3, 4}, {5, 4}, {7, 8}, {6, 8}, {12, 8}};
        int i, n = 6, next = 0;
        for (i = 0; i < n; ++i)
        {
            if (sigs[i][0] == p->tsNum && sigs[i][1] == p->tsDen)
            {
                next = (i + 1) % n;
                break;
            }
        }
        c808_set_timesig(g_engine, sigs[next][0], sigs[next][1]);
        return;
    }

    /* 快捷 pattern */
    {
        int i;
        for (i = 0; i < 16; ++i)
        {
            if (point_in(rcPatQuick[i], x, y))
            {
                c808_select_pattern(g_engine, g_page * 16 + i);
                return;
            }
        }
    }
    if (point_in(rcPagePrev, x, y))
    {
        if (g_page > 0) g_page--;
        return;
    }
    if (point_in(rcPageNext, x, y))
    {
        if (g_page < C808_PATTERNS / 16 - 1) g_page++;
        return;
    }

    /* 声部参数旋钮 */
    {
        int i;
        for (i = 0; i < C808_P_COUNT; ++i)
        {
            if (point_in(rcVoiceKnobs[i], x, y))
            {
                float v = value_from_knob_rect(rcVoiceKnobs[i], y);
                c808_set_track_param(g_engine, g_selTrack, i, v);
                g_dragId = i;
                g_dragY = y;
                g_dragStart = v;
                return;
            }
        }
        for (i = 0; i < C808_M_COUNT; ++i)
        {
            if (point_in(rcMasterKnobs[i], x, y))
            {
                float v = value_from_knob_rect(rcMasterKnobs[i], y);
                c808_set_master(g_engine, i, v);
                g_dragId = 200 + i;
                g_dragY = y;
                g_dragStart = v;
                return;
            }
        }
    }

    /* step 属性滑条 */
    {
        int i;
        for (i = 0; i < 5; ++i)
        {
            if (point_in(rcStepSliders[i], x, y))
            {
                g_dragId = 300 + i;
                set_step_slider(i, x);
                return;
            }
        }
    }
    if (point_in(rcCond, x, y))
    {
        int c = (p->patterns[p->currentPattern].cond[g_selTrack][g_selStep] + 1) % 7;
        c808_set_step_cond(g_engine, p->currentPattern, g_selTrack, g_selStep, c,
                           p->patterns[p->currentPattern].condN[g_selTrack][g_selStep]);
        return;
    }
    if (point_in(rcAccent, x, y))
    {
        int a = !p->patterns[p->currentPattern].accent[g_selTrack][g_selStep];
        c808_set_step_accent(g_engine, p->currentPattern, g_selTrack, g_selStep, a);
        return;
    }
    if (point_in(rcTrkLenM, x, y))
    {
        c808_set_track_len(g_engine, g_selTrack, p->trackLen[g_selTrack] - 1);
        return;
    }
    if (point_in(rcTrkLenP, x, y))
    {
        c808_set_track_len(g_engine, g_selTrack, p->trackLen[g_selTrack] + 1);
        return;
    }
    if (point_in(rcTrigger, x, y))
    {
        c808_note_on(g_engine, g_selTrack, 112, 0);
        return;
    }

    /* 打击垫 */
    {
        int i;
        for (i = 0; i < 16; ++i)
        {
            if (point_in(rcPad[i], x, y))
            {
                c808_note_on(g_engine, i, 112, 0);
                g_selTrack = i;
                return;
            }
        }
    }

    /* 网格 */
    grid_step_at(x, y, &t, &s);
    if (t >= 0 && s >= 0)
    {
        g_selTrack = t;
        g_selStep = s;
        if (ctrl)
        {
            int a = !p->patterns[p->currentPattern].accent[t][s];
            c808_set_step_accent(g_engine, p->currentPattern, t, s, a);
        }
        else if (shift)
        {
            cycle_prob(t, s);
        }
        else if (alt)
        {
            cycle_ratchet(t, s, 1);
        }
        else
        {
            int on = !p->patterns[p->currentPattern].on[t][s];
            c808_set_step_on(g_engine, p->currentPattern, t, s, on);
        }
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* 绘制                                                                       */
/* ------------------------------------------------------------------------- */
static void paint_window(HDC target, RECT client)
{
    const c808_project* p = c808_project_ro(g_engine);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    int i, t, s;
    RECT r;
    wchar_t buf[128];
    int patIdx, len;
    int rowH, colW;

    fill_rect(dc, client, COL_BG);
    /* 顶部 */
    draw_button(dc, rcPlay, g_status.playing ? L"PLAYING" : L"PLAY", g_status.playing, g_fLabel);
    draw_button(dc, rcStop, L"STOP", 0, g_fLabel);
    draw_button(dc, rcTap, L"TAP", 0, g_fLabel);
    draw_button(dc, rcBpmMinus, L"-", 0, g_fLabel);
    draw_button(dc, rcBpmPlus, L"+", 0, g_fLabel);
    fill_round(dc, rcBpm, S(7), COL_PANEL2);
    swprintf(buf, 128, L"%.1f BPM", p ? p->bpm : 120.0f);
    draw_text_center(dc, rcBpm, buf, COL_ORANGE, g_fLabel);

    /* swing 滑条 */
    {
        RECT sr = rcSwing;
        RECT txt = sr;
        sr.left += S(54);
        sr.top += S(9);
        sr.bottom -= S(9);
        txt.right = txt.left + S(52);
        draw_text(dc, txt, L"SWING", COL_DIM, g_fSmall, DT_VCENTER | DT_SINGLELINE);
        fill_round(dc, sr, S(4), COL_PANEL3);
        if (p)
        {
            RECT f = sr;
            f.right = sr.left + (sr.right - sr.left) * p->patterns[p->currentPattern].swing / 100;
            fill_rect(dc, f, COL_ORANGE);
            swprintf(buf, 128, L"%d%%", p->patterns[p->currentPattern].swing);
            draw_text_center(dc, txt, buf, COL_TEXT, g_fSmall);
        }
    }
    draw_button(dc, rcPatPrev, L"<", 0, g_fLabel);
    draw_button(dc, rcPatNext, L">", 0, g_fLabel);
    fill_round(dc, rcPatDisp, S(7), COL_PANEL2);
    swprintf(buf, 128, L"PAT %03d", p ? p->currentPattern : 0);
    draw_text_center(dc, rcPatDisp, buf, COL_TEXT, g_fLabel);
    draw_button(dc, rcLenMinus, L"-", 0, g_fLabel);
    draw_button(dc, rcLenPlus, L"+", 0, g_fLabel);
    fill_round(dc, rcLenDisp, S(7), COL_PANEL2);
    swprintf(buf, 128, L"LEN %d", p ? p->patterns[p->currentPattern].len : 16);
    draw_text_center(dc, rcLenDisp, buf, COL_TEXT, g_fLabel);
    draw_button(dc, rcSong, p && p->songOn ? L"SONG ON" : L"SONG OFF", p && p->songOn, g_fLabel);
    draw_button(dc, rcSongEdit, L"EDIT SONG", 0, g_fLabel);

    draw_button(dc, rcSave, L"SAVE", 0, g_fLabel);
    draw_button(dc, rcLoad, L"LOAD", 0, g_fLabel);
    draw_button(dc, rcWav, L"WAV", 0, g_fLabel);
    draw_button(dc, rcMidi, L"MIDI", 0, g_fLabel);
    draw_button(dc, rcStems, L"STEMS", 0, g_fLabel);
    draw_button(dc, rcClear, L"CLEAR", 0, g_fLabel);
    draw_button(dc, rcCopy, L"COPY>", 0, g_fLabel);
    fill_round(dc, rcMidiIn, S(7), COL_PANEL2);
    draw_text(dc, rcMidiIn, g_midiName, COL_DIM, g_fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_button(dc, rcSync, (p && p->clockSource) ? L"SYNC MIDI" : L"SYNC INT",
                p && p->clockSource, g_fSmall);
    {
        wchar_t ts[24];
        swprintf(ts, 24, L"%d/%d", p ? p->tsNum : 4, p ? p->tsDen : 4);
        draw_button(dc, rcTimeSig, ts, 0, g_fSmall);
    }

    /* 网格 */
    fill_round(dc, rcGrid, S(8), COL_PANEL);
    patIdx = p ? p->currentPattern : 0;
    len = p ? p->patterns[patIdx].len : 16;
    if (len < 1) len = 1;
    rowH = (rcGridSteps.bottom - rcGridSteps.top) / 16;
    colW = (rcGridSteps.right - rcGridSteps.left) / len;

    draw_text(dc, (RECT)
    {
        rcGrid.left + S(12), rcGrid.top + S(4), rcGrid.right - S(12), rcGrid.top + S(24)
    }
    ,
    L"TRACK", COL_DIM, g_fTiny, DT_LEFT | DT_SINGLELINE
    )
    ;
    for (s = 0; s < len; s += 4)
    {
        RECT nr = {
            rcGridSteps.left + s * colW, rcGrid.top + S(4),
            rcGridSteps.left + (s + 1) * colW, rcGrid.top + S(24)
        };
        swprintf(buf, 128, L"%d", s + 1);
        draw_text_center(dc, nr, buf, COL_DIM, g_fTiny);
    }

    for (t = 0; t < 16; ++t)
    {
        int y = rcGridSteps.top + t * rowH;
        RECT row = {rcGrid.left, y, rcGridSteps.left, y + rowH};
        RECT name = {rcGrid.left + S(4), y, rcGrid.left + S(70), y + rowH};
        int soloAny = 0;
        int j;
        for (j = 0; j < 16; ++j) if (p && p->solo[j]) soloAny = 1;
        fill_rect(dc, row, t == g_selTrack ? RGB(45, 39, 34) : ((t & 1) ? RGB(24, 27, 33) : RGB(22, 25, 30)));
        draw_text(dc, name, TRACK_NAMES[t], t == g_selTrack ? COL_ORANGE : COL_TEXT, g_fTiny,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        /* mute / solo */
        {
            RECT mr = {rcGrid.left + S(72), y + S(2), rcGrid.left + S(94), y + rowH - S(2)};
            RECT sr2 = {rcGrid.left + S(96), y + S(2), rcGrid.left + S(118), y + rowH - S(2)};
            int mute = p ? p->mute[t] : 0;
            int solo = p ? p->solo[t] : 0;
            fill_round(dc, mr, S(3), mute ? COL_RED : COL_PANEL3);
            draw_text_center(dc, mr, L"M", mute ? COL_TEXT : COL_DIM, g_fTiny);
            fill_round(dc, sr2, S(3), solo ? COL_GREEN : COL_PANEL3);
            draw_text_center(dc, sr2, L"S", solo ? COL_TEXT : COL_DIM, g_fTiny);
            (void)soloAny;
        }
        /* 电平 */
        {
            RECT met = {rcGrid.left + S(120), y + S(2), rcGrid.left + S(124), y + rowH - S(2)};
            draw_meter(dc, met, g_status.meter[t]);
        }
        for (s = 0; s < len; ++s)
        {
            RECT cell = {
                rcGridSteps.left + s * colW + S(1), y + S(2),
                rcGridSteps.left + (s + 1) * colW - S(1), y + rowH - S(2)
            };
            int on = p ? p->patterns[patIdx].on[t][s] : 0;
            int vel = p ? p->patterns[patIdx].vel[t][s] : 100;
            int acc = p ? p->patterns[patIdx].accent[t][s] : 0;
            int pr = p ? p->patterns[patIdx].prob[t][s] : 100;
            int rt = p ? p->patterns[patIdx].ratchet[t][s] : 1;
            int mi = p ? p->patterns[patIdx].micro[t][s] : 0;
            COLORREF c = COL_CELL;
            if (on)
            {
                int v = vel * 2;
                c = RGB(120 + v * 130 / 254, 55 + v * 90 / 254, 25);
                if (acc) c = RGB(255, 196, 120);
            }
            fill_round(dc, cell, S(3), c);
            if (on && pr < 100)
            {
                RECT dot = {cell.left + S(2), cell.top + S(2), cell.left + S(5), cell.top + S(5)};
                fill_rect(dc, dot, COL_BLUE);
            }
            if (on && rt > 1)
            {
                int k;
                for (k = 0; k < rt && k < 4; ++k)
                {
                    RECT rb = {
                        cell.left + S(3) + k * S(3), cell.bottom - S(5), cell.left + S(4) + k * S(3), cell.bottom - S(2)
                    };
                    fill_rect(dc, rb, COL_TEXT);
                }
            }
            if (on && mi != 0)
            {
                RECT mb = {cell.right - S(4), cell.top, cell.right - S(1), cell.top + S(4)};
                fill_rect(dc, mb, mi > 0 ? COL_GREEN : COL_RED);
            }
            if (t == g_selTrack && s == g_selStep) frame_round(dc, cell, S(3), COL_TEXT);
            /* 播放头：每轨按自身 trackLen 循环 */
            {
                int tl = p ? p->trackLen[t] : len;
                if (tl < 1) tl = 1;
                if (g_status.playing && g_status.step % tl == s)
                {
                    RECT pb = {cell.left, cell.top, cell.right, cell.bottom};
                    frame_round(dc, pb, S(3), COL_BLUE);
                }
            }
        }
    }

    /* 右侧声部面板 */
    fill_round(dc, rcPanel, S(8), COL_PANEL);
    swprintf(buf, 128, L"VOICE EDIT  ·  %ls", TRACK_NAMES[g_selTrack]);
    draw_text(dc, (RECT)
    {
        rcPanel.left + S(12), rcPanel.top + S(8), rcPanel.right - S(180), rcPanel.top + S(28)
    }
    ,
    buf, COL_ORANGE, g_fLabel, DT_LEFT | DT_SINGLELINE
    )
    ;
    draw_button(dc, rcTrigger, L"TRIGGER", 0, g_fSmall);
    for (i = 0; i < C808_P_COUNT; ++i)
    {
        float v = p
                      ? (i == C808_P_TUNE
                             ? p->tune[g_selTrack]
                             : i == C808_P_DECAY
                             ? p->decay[g_selTrack]
                             : i == C808_P_TONE
                             ? p->tone[g_selTrack]
                             : i == C808_P_CHAR
                             ? p->chara[g_selTrack]
                             : i == C808_P_LEVEL
                             ? p->level[g_selTrack]
                             : p->pan[g_selTrack])
                      : 0.5f;
        draw_knob(dc, rcVoiceKnobs[i], KNOB_NAMES[i], v);
    }

    /* step 属性 */
    swprintf(buf, 128, L"STEP %02d  ·  %ls", g_selStep + 1, TRACK_NAMES[g_selTrack]);
    draw_text(dc, (RECT)
    {
        rcPanel.left + S(10), rcPanel.top + S(202), rcPanel.right - S(10), rcPanel.top + S(222)
    }
    ,
    buf, COL_TEXT, g_fLabel, DT_LEFT | DT_SINGLELINE
    )
    ;
    if (p)
    {
        const c808_pattern* q = &p->patterns[patIdx];
        int vel = q->vel[g_selTrack][g_selStep];
        int prob = q->prob[g_selTrack][g_selStep];
        int rat = q->ratchet[g_selTrack][g_selStep];
        int rate = q->rate[g_selTrack][g_selStep];
        int micro = q->micro[g_selTrack][g_selStep];
        int cond = q->cond[g_selTrack][g_selStep];
        int acc = q->accent[g_selTrack][g_selStep];
        swprintf(buf, 128, L"%d", vel);
        draw_slider(dc, rcStepSliders[0], L"VELOCITY", (vel - 1) / 126.0f, buf, 0);
        swprintf(buf, 128, L"%d%%", prob);
        draw_slider(dc, rcStepSliders[1], L"PROB", prob / 100.0f, buf, 0);
        swprintf(buf, 128, L"x%d", rat);
        draw_slider(dc, rcStepSliders[2], L"RATCHET", (rat - 1) / 7.0f, buf, 0);
        swprintf(buf, 128, L"%d%%", rate);
        draw_slider(dc, rcStepSliders[3], L"RATE", (rate - 10) / 90.0f, buf, 0);
        swprintf(buf, 128, L"%+d smp", micro);
        draw_slider(dc, rcStepSliders[4], L"MICRO", micro / 4800.0f + 0.5f, buf, 1);
        swprintf(buf, 128, L"COND: %ls", COND_NAMES[cond <= 6 ? cond : 0]);
        draw_button(dc, rcCond, buf, cond != 0, g_fSmall);
        draw_button(dc, rcAccent, acc ? L"ACCENT ON" : L"ACCENT", acc, g_fSmall);
        draw_button(dc, rcTrkLenM, L"-", 0, g_fSmall);
        swprintf(buf, 128, L"TRACK LEN %d", p->trackLen[g_selTrack]);
        draw_button(dc, rcTrkLenDisp, buf, 0, g_fSmall);
        draw_button(dc, rcTrkLenP, L"+", 0, g_fSmall);
    }

    /* 主控 */
    fill_round(dc, rcMaster, S(8), COL_PANEL);
    draw_text(dc, (RECT)
    {
        rcMaster.left + S(16), rcMaster.top + S(6), rcMaster.right, rcMaster.top + S(26)
    }
    ,
    L"MASTER  ·  FILTER / SATURATION / ACCENT", COL_DIM, g_fTiny, DT_LEFT | DT_SINGLELINE
    )
    ;
    for (i = 0; i < C808_M_COUNT; ++i)
    {
        draw_knob(dc, rcMasterKnobs[i], MASTER_NAMES[i], p ? p->master[i] : 0.5f);
    }

    /* 打击垫 */
    fill_round(dc, rcPads, S(8), COL_PANEL);
    for (i = 0; i < 16; ++i)
    {
        int isSel = (i == g_selTrack);
        fill_round(dc, rcPad[i], S(5), isSel ? COL_ORANGE2 : COL_PANEL2);
        frame_round(dc, rcPad[i], S(5), isSel ? COL_ORANGE : COL_PANEL3);
        draw_text_center(dc, rcPad[i], TRACK_NAMES[i], isSel ? COL_TEXT : COL_DIM, g_fTiny);
    }

    /* pattern 快捷条 */
    fill_round(dc, rcSongStrip, S(8), COL_PANEL);
    draw_button(dc, rcPagePrev, L"<", 0, g_fTiny);
    draw_button(dc, rcPageNext, L">", 0, g_fTiny);
    for (i = 0; i < 16; ++i)
    {
        int idx = g_page * 16 + i;
        int active = p && p->currentPattern == idx;
        swprintf(buf, 128, L"%d", idx);
        draw_button(dc, rcPatQuick[i], buf, active, g_fSmall);
    }

    /* 状态栏 */
    fill_rect(dc, rcStatus, COL_PANEL);
    swprintf(buf, 128, L"%ls   ·   CPU %d%%   ·   XRUN %d   ·   %ls   ·   %ls",
             g_audioInfo, (int)(g_status.cpuLoad * 100.0f), g_status.xruns, g_msg,
             g_status.playing ? L"PLAY" : L"STOP");
    draw_text(dc, (RECT)
    {
        rcStatus.left + S(16), rcStatus.top, rcStatus.right - S(16), rcStatus.bottom
    }
    ,
    buf, g_audioOk ? COL_DIM : COL_RED, g_fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS
    )
    ;
    if (g_exportBusy)
    {
        RECT er = {rcStatus.right - S(220), rcStatus.top, rcStatus.right - S(16), rcStatus.bottom};
        draw_text(dc, er, L"EXPORTING...", COL_ORANGE, g_fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    (void)r;
}

/* ------------------------------------------------------------------------- */
/* 窗口过程                                                                   */
/* ------------------------------------------------------------------------- */
static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_CREATE:
        g_hwnd = h;
        {
            RECT cr;
            int sx, sy;
            GetClientRect(h, &cr);
            sx = cr.right * 100 / 1280;
            sy = cr.bottom * 100 / 720;
            g_scale = sx < sy ? sx : sy;
            if (g_scale < 45) g_scale = 45;
        }
        create_fonts();
        build_layout(h);
        if (audio_init(h) != 0)
        {
            g_engine = c808_create(48000.0);
            wcscpy(g_audioInfo, L"NO AUDIO DEVICE · SILENT MODE");
            swprintf(g_msg, 256, L"WASAPI INIT FAILED - CHECK OUTPUT DEVICE");
        }
        c808_midi_start(g_engine, 0);
        if (c808_midi_device_count() > 0)
        {
            char u8[256];
            wchar_t nm[128];
            c808_midi_device_name(0, u8, sizeof(u8));
            MultiByteToWideChar(CP_UTF8, 0, u8, -1, nm, 128);
            g_midiIndex = 0;
            swprintf(g_midiName, 128, L"MIDI: %ls", nm);
        }
        SetTimer(h, TIMER_UI, 33, NULL);
        return 0;

    case WM_SIZE:
        update_layout(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mi = (MINMAXINFO*)l;
            mi->ptMinTrackSize.x = 760;
            mi->ptMinTrackSize.y = 480;
            return 0;
        }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT cr;
            GetClientRect(h, &cr);
            paint_window(dc, cr);
            EndPaint(h, &ps);
            return 0;
        }

    case WM_TIMER:
        if (w == TIMER_UI)
        {
            if (!g_audioOk && g_engine)
            {
                float l[256], r[256];
                c808_render(g_engine, l, r, 256);
            }
            c808_get_status(g_engine, &g_status);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            SetCapture(h);
            click_handle(x, y, shift, ctrl, alt);
            return 0;
        }
    case WM_MOUSEMOVE:
        {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (g_dragId >= 0)
            {
                float delta = (float)(g_dragY - y) / (float)S(110);
                if (g_dragId == 100)
                {
                    c808_set_bpm(g_engine, g_dragStart + delta * 120.0f);
                }
                else if (g_dragId >= 200)
                {
                    c808_set_master(g_engine, g_dragId - 200, g_dragStart + delta);
                }
                else if (g_dragId >= 300)
                {
                    set_step_slider(g_dragId - 300, x);
                }
                else
                {
                    c808_set_track_param(g_engine, g_selTrack, g_dragId, g_dragStart + delta);
                }
            }
            else
            {
                int t, s;
                grid_step_at(x, y, &t, &s);
                g_hoverTrack = t;
                g_hoverStep = s;
            }
            return 0;
        }
    case WM_LBUTTONUP:
        g_dragId = -1;
        ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL:
        {
            POINT pt;
            int t, s;
            pt.x = GET_X_LPARAM(l);
            pt.y = GET_Y_LPARAM(l);
            ScreenToClient(h, &pt);
            grid_step_at(pt.x, pt.y, &t, &s);
            if (t >= 0 && s >= 0)
            {
                const c808_project* p = c808_project_ro(g_engine);
                int v = p->patterns[p->currentPattern].vel[t][s];
                v += (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 4 : -4);
                if (v < 1) v = 1;
                if (v > 127) v = 127;
                c808_set_step_vel(g_engine, p->currentPattern, t, s, v);
            }
            else if (point_in(rcBpm, pt.x, pt.y))
            {
                c808_set_bpm(g_engine, c808_project_ro(g_engine)->bpm +
                             (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.0f : -1.0f));
            }
            return 0;
        }

    case WM_KEYDOWN:
        if (w == VK_SPACE)
        {
            if (g_status.playing) c808_transport_stop(g_engine);
            else c808_transport_play(g_engine);
            return 0;
        }
        if (w == VK_LEFT)
        {
            c808_select_pattern(
                g_engine, (c808_project_ro(g_engine)->currentPattern + C808_PATTERNS - 1) % C808_PATTERNS);
            return 0;
        }
        if (w == VK_RIGHT)
        {
            c808_select_pattern(g_engine, (c808_project_ro(g_engine)->currentPattern + 1) % C808_PATTERNS);
            return 0;
        }
        if (w == 'T')
        {
            click_handle(rcTap.left + 2, rcTap.top + 2, 0, 0, 0);
            return 0;
        }
        {
            static const int keys[16] = {
                '1', '2', '3', '4', 'Q', 'W', 'E', 'R', 'A', 'S', 'D', 'F', 'Z', 'X', 'C', 'V'
            };
            int i;
            for (i = 0; i < 16; ++i)
            {
                if (w == keys[i])
                {
                    c808_note_on(g_engine, i, 112, 0);
                    g_selTrack = i;
                    return 0;
                }
            }
        }
        break;

    case WM_EXPORT_DONE:
        if (InterlockedCompareExchange(&g_exportResult, 0, 0) == 0)
            swprintf(g_msg, 256, L"EXPORT COMPLETE");
        else
            swprintf(g_msg, 256, L"EXPORT FAILED (code %ld)", (long)g_exportResult);
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        if (g_exportBusy)
        {
            swprintf(g_msg, 256, L"EXPORT IN PROGRESS - PLEASE WAIT");
            return 0;
        }
        KillTimer(h, TIMER_UI);
        audio_shutdown();
        c808_midi_stop(g_engine);
        if (g_engine)
        {
            c808_destroy(g_engine);
            g_engine = NULL;
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------------- */
/* 程序入口                                                                   */
/* ------------------------------------------------------------------------- */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    RECT work, wr;
    int winW, winH;
    (void)prev;
    (void)cmd;

    g_inst = inst;
    SetProcessDPIAware();
    g_scale = 100;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    winW = (work.right - work.left) * 92 / 100;
    winH = (work.bottom - work.top) * 92 / 100;
    /* 兼容高 DPI 虚拟化环境：限制初始窗口不超过 1400x740，确保完整可见 */
    if (winW > 1400) winW = 1400;
    if (winH > 740) winH = 740;
    if (winW < 900) winW = 900;
    if (winH < 560) winH = 560;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(COL_BG);
    wc.lpszClassName = L"Gui808Window";
    RegisterClassW(&wc);

    wr.left = 0;
    wr.top = 0;
    wr.right = winW;
    wr.bottom = winH;
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, L"Gui808Window", L"808 · SYNTHETIC DRUM MACHINE",
                           WS_OVERLAPPEDWINDOW,
                           20, 20,
                           wr.right - wr.left, wr.bottom - wr.top,
                           NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_fTitle);
    DeleteObject(g_fLabel);
    DeleteObject(g_fSmall);
    DeleteObject(g_fTiny);
    DeleteObject(g_fMono);
    DeleteObject(g_fBig);
    CoUninitialize();
    return (int)msg.wParam;
}