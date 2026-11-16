/* ==========================================================================
 *  ui.c —— Win32 原生界面（纯 C 版）
 *
 *  徐某没有用任何 UI 框架，只用 GDI 手绘：
 *    · 顶部走带条：播放/停止/BPM/摇摆/Pattern/步数/歌曲/保存/导出；
 *    · 中部 16×N 步进网格：左键开关、右键选中并编辑；
 *    · 网格下方是“步进属性”条：力度/重音/概率/微时/滚奏/条件；
 *    · 再下方是当前声部的参数滑杆；
 *    · 最下方是主滤波/过载/音量/电平。
 *
 *  这样做的好处：编译出来只有一个 EXE，没有任何运行时依赖，
 *  而且 UI 与音频线程彻底分离——UI 卡住绝不影响鼓点。
 * ========================================================================== */
#include "xu808.h"
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winmm.lib")

/* ---------------- 颜色（与其它版本一致） ---------------- */
#define C_BG        RGB(18,18,18)
#define C_PANEL     RGB(28,28,28)
#define C_PANEL2    RGB(35,35,35)
#define C_LINE      RGB(46,46,46)
#define C_ORANGE    RGB(255,122,24)
#define C_ORANGED   RGB(168,74,12)
#define C_YELLOW    RGB(255,209,102)
#define C_CYAN      RGB(76,201,240)
#define C_TEXT      RGB(232,232,232)
#define C_DIM       RGB(138,138,138)
#define C_RED       RGB(229,72,77)

/* ---------------- 布局常量 ---------------- */
#define TOP_H       64
#define GX          8
#define GY          72
#define LABEL_W     124
#define HEAD_H      22
#define CELL_H      22
#define GRID_ROWS   16
#define STEP_ED_Y   (GY + HEAD_H + GRID_ROWS * CELL_H + 8)   /* 步进属性条 */
#define STEP_ED_H   64
#define PARAM_Y     (STEP_ED_Y + STEP_ED_H + 8)
#define PARAM_H     100
#define MASTER_Y    (PARAM_Y + PARAM_H + 8)
#define MASTER_H    100

static XuEngine* g_eng;
static XuProject* g_proj;
static HWND g_hwnd;
static HINSTANCE g_inst;
static HFONT g_fontSmall, g_fontNorm, g_fontBold, g_fontMono, g_fontLogo, g_fontTiny;
static HBRUSH g_brBg, g_brPanel, g_brPanel2, g_brOrange, g_brOrangeD;
static int g_selPad = 0;
static int g_selStep = 0;
static int g_dragParam = -1; /* 正在拖动的声部滑杆索引 */
static int g_dragMaster = -1; /* 正在拖动的主效果滑杆索引 */
static int g_cellW = 30; /* 由步数动态计算 */
static int g_steps = 16;
static volatile LONG g_midiCount = 0;
static unsigned char g_midiBuf[256][3];
static HMIDIIN g_midiHandle = NULL;
static int g_audioOk = 0;

/* 顶部按钮的矩形；在布局时计算，鼠标命中和绘制共用 */
static RECT g_rcPlay, g_rcStop, g_rcTap, g_rcBpmM, g_rcBpmP, g_rcBpmVal;
static RECT g_rcSwingM, g_rcSwingP, g_rcSwingVal;
static RECT g_rcPatM, g_rcPatP, g_rcPatVal;
static RECT g_rcStepM, g_rcStepP, g_rcStepVal;
static RECT g_rcSong, g_rcClock, g_rcSave, g_rcLoad, g_rcWav, g_rcMidi;
static RECT g_rcStepEd[7][3]; /* [属性][0=减 1=值 2=加] */

/* 参数滑杆定义：offset 是 XuPadParams 内 float 字段的字节偏移 */
typedef struct
{
    const wchar_t* label;
    int off;
    int max;
} XuParamDef;

static XuParamDef g_paramDefs[8];
static int g_paramCount = 0;

/* 主效果滑杆：值范围不同，单独处理 */
typedef struct
{
    const wchar_t* label;
    int kind;
} XuMasterDef;

static XuMasterDef g_masterDefs[5] = {
    {L"LP 低通", 0}, {L"RES 共鸣", 1}, {L"HP 高通", 2},
    {L"DRIVE 过载", 3}, {L"VOL 音量", 4}
};

/* 取某声部当前应显示的参数表（与其它版本一致） */
static void build_param_defs(int pad)
{
    g_paramCount = 0;
#define ADD(lbl, field, mx) do { \
    g_paramDefs[g_paramCount].label = lbl; \
    g_paramDefs[g_paramCount].off = (int)offsetof(XuPadParams, field); \
    g_paramDefs[g_paramCount].max = mx; \
    g_paramCount++; } while (0)

    if (pad == 0)
    {
        ADD(L"TUNE", tune, 100);
        ADD(L"DECAY", decay, 100);
        ADD(L"TONE", tone, 100);
    }
    else if (pad == 1)
    {
        ADD(L"TUNE", tune, 100);
        ADD(L"SNAP", snappy, 100);
        ADD(L"DECAY", decay, 100);
    }
    else if (pad >= 2 && pad <= 4)
    {
        ADD(L"TUNE", tune, 100);
        ADD(L"DECAY", decay, 100);
        ADD(L"COLOR", tone, 100);
    }
    else if (pad == 10)
    {
        ADD(L"DECAY", decay, 100);
        ADD(L"TONE", tone, 100);
        ADD(L"TAIL", tail, 100);
    }
    else if (pad == 11)
    {
        ADD(L"DECAY", decay, 100);
        ADD(L"TONE", tone, 100);
    }
    else if (pad == 12)
    {
        ADD(L"TUNE", tune, 100);
        ADD(L"DECAY", decay, 100);
    }
    else if (pad >= 13)
    {
        ADD(L"DECAY", decay, 100);
        ADD(L"TONE", tone, 100);
    }
    else
    {
        ADD(L"TUNE", tune, 100);
        ADD(L"DECAY", decay, 100);
    }
    ADD(L"LEVEL", level, 125);
    ADD(L"PAN", pan, 100);
#undef ADD
}

/* 计算网格单元宽度：步数越多格子越窄，保证 64 步也能一屏放下 */
static void layout_grid(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int avail = rc.right - 2 * GX - LABEL_W;
    g_steps = g_proj->patterns[g_proj->current].steps;
    if (g_steps < 1) g_steps = 1;
    g_cellW = avail / g_steps;
    if (g_cellW > 32) g_cellW = 32;
    if (g_cellW < 8) g_cellW = 8;
}

/* ---------------- 顶部按钮布局 ---------------- */
static void layout_top(void)
{
    int y = 12, h = 40;
    int x = 176;
    SetRect(&g_rcPlay, x, y, x + 78, y + h);
    x += 84;
    SetRect(&g_rcStop, x, y, x + 78, y + h);
    x += 92;
    SetRect(&g_rcBpmM, x, y + 8, x + 22, y + 32);
    SetRect(&g_rcBpmVal, x + 24, y + 8, x + 86, y + 32);
    SetRect(&g_rcBpmP, x + 88, y + 8, x + 110, y + 32);
    x += 118;
    SetRect(&g_rcTap, x, y, x + 48, y + h);
    x += 56;
    SetRect(&g_rcSwingM, x, y + 8, x + 22, y + 32);
    SetRect(&g_rcSwingVal, x + 24, y + 8, x + 86, y + 32);
    SetRect(&g_rcSwingP, x + 88, y + 8, x + 110, y + 32);
    x += 118;
    SetRect(&g_rcPatM, x, y + 8, x + 22, y + 32);
    SetRect(&g_rcPatVal, x + 24, y + 8, x + 118, y + 32);
    SetRect(&g_rcPatP, x + 120, y + 8, x + 142, y + 32);
    x += 150;
    SetRect(&g_rcStepM, x, y + 8, x + 22, y + 32);
    SetRect(&g_rcStepVal, x + 24, y + 8, x + 74, y + 32);
    SetRect(&g_rcStepP, x + 76, y + 8, x + 98, y + 32);
    x += 108;
    SetRect(&g_rcSong, x, y, x + 66, y + h);
    x += 72;
    SetRect(&g_rcClock, x, y, x + 76, y + h);
    x += 84;
    SetRect(&g_rcSave, x, y, x + 56, y + h);
    x += 60;
    SetRect(&g_rcLoad, x, y, x + 56, y + h);
    x += 60;
    SetRect(&g_rcWav, x, y, x + 76, y + h);
    x += 80;
    SetRect(&g_rcMidi, x, y, x + 80, y + h);
}

/* 辅助：填充矩形 */
static void fill(HDC dc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* 辅助：画 1px 边框 */
static void frame(HDC dc, RECT r, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ old = SelectObject(dc, p);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, old);
    DeleteObject(p);
}

/* 辅助：居中/左对齐文字 */
static void text_at(HDC dc, int x, int y, int w, int h, const wchar_t* s, COLORREF c, HFONT f, UINT fmt)
{
    RECT r = {x, y, x + w, y + h};
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_VCENTER);
}

/* 画一个“按钮” */
static void draw_btn(HDC dc, RECT r, const wchar_t* label, int active, int enabled)
{
    fill(dc, r, active ? C_ORANGED : C_PANEL2);
    frame(dc, r, active ? C_ORANGE : C_LINE);
    text_at(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, label,
            enabled ? (active ? C_TEXT : C_TEXT) : C_DIM, g_fontBold, DT_CENTER);
}

/* 画一个水平滑杆，返回手柄位置（供命中测试） */
static RECT draw_slider(HDC dc, int x, int y, int w, const wchar_t* label,
                        float v01, int showVal, int maxVal)
{
    RECT track, handle;
    text_at(dc, x, y, w, 14, label, C_DIM, g_fontTiny, DT_LEFT);
    SetRect(&track, x, y + 20, x + w, y + 28);
    fill(dc, track, RGB(16, 16, 16));
    int hx = x + (int)(v01 * (w - 12));
    if (hx < x) hx = x;
    if (hx > x + w - 12) hx = x + w - 12;
    SetRect(&handle, hx, y + 16, hx + 12, y + 32);
    fill(dc, handle, C_ORANGE);
    frame(dc, handle, C_LINE);
    wchar_t buf[32];
    if (showVal)
    {
        swprintf(buf, 32, L"%d", maxVal);
        text_at(dc, x, y + 34, w, 12, buf, C_ORANGE, g_fontTiny, DT_LEFT);
    }
    return track;
}

/* 根据鼠标 x 计算滑杆值 0..1 */
static float slider_value(RECT track, int mx)
{
    int w = track.right - track.left;
    if (w <= 0) return 0.0f;
    float v = (float)(mx - track.left) / (float)w;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return v;
}

/* ---------------- 绘制整个界面 ---------------- */
static void paint(HWND hwnd, HDC dc, int W, int H)
{
    (void)H;
    XuPattern* pat = &g_proj->patterns[g_proj->current];
    wchar_t buf[128];

    fill(dc, (RECT)
    {
        0, 0, W, H
    }
    ,
    C_BG
    )
    ;

    /* ---------- 顶部 ---------- */
    fill(dc, (RECT)
    {
        0, 0, W, TOP_H
    }
    ,
    C_PANEL
    )
    ;
    text_at(dc, 10, 2, 150, 36, L"XU-808", C_ORANGE, g_fontLogo, DT_LEFT);
    text_at(dc, 12, 40, 170, 16, L"RHYTHM COMPOSER · 徐某 · 16 VOICE",
            C_DIM, g_fontTiny, DT_LEFT);

    draw_btn(dc, g_rcPlay, L"▶ PLAY", g_eng->playing, 1);
    draw_btn(dc, g_rcStop, L"■ STOP", 0, 1);
    draw_btn(dc, g_rcTap, L"TAP", 0, 1);

    draw_btn(dc, g_rcBpmM, L"-", 0, 1);
    swprintf(buf, 128, L"%.1f", g_proj->master.bpm);
    text_at(dc, g_rcBpmVal.left, g_rcBpmVal.top, g_rcBpmVal.right - g_rcBpmVal.left,
            24, buf, C_ORANGE, g_fontBold, DT_CENTER);
    draw_btn(dc, g_rcBpmP, L"+", 0, 1);
    text_at(dc, g_rcBpmM.left, 1, 110, 12, L"BPM", C_DIM, g_fontTiny, DT_LEFT);

    draw_btn(dc, g_rcSwingM, L"-", 0, 1);
    swprintf(buf, 128, L"%d%%", (int)(g_proj->master.swing * 100));
    text_at(dc, g_rcSwingVal.left, g_rcSwingVal.top, g_rcSwingVal.right - g_rcSwingVal.left,
            24, buf, C_ORANGE, g_fontBold, DT_CENTER);
    draw_btn(dc, g_rcSwingP, L"+", 0, 1);
    text_at(dc, g_rcSwingM.left, 1, 110, 12, L"SWING", C_DIM, g_fontTiny, DT_LEFT);

    draw_btn(dc, g_rcPatM, L"<", 0, 1);
    swprintf(buf, 128, L"%02d %hs", g_proj->current + 1, pat->name);
    text_at(dc, g_rcPatVal.left, g_rcPatVal.top, g_rcPatVal.right - g_rcPatVal.left,
            24, buf, C_TEXT, g_fontNorm, DT_CENTER);
    draw_btn(dc, g_rcPatP, L">", 0, 1);
    text_at(dc, g_rcPatM.left, 1, 110, 12, L"PATTERN", C_DIM, g_fontTiny, DT_LEFT);

    draw_btn(dc, g_rcStepM, L"-", 0, 1);
    swprintf(buf, 128, L"%d", pat->steps);
    text_at(dc, g_rcStepVal.left, g_rcStepVal.top, g_rcStepVal.right - g_rcStepVal.left,
            24, buf, C_ORANGE, g_fontBold, DT_CENTER);
    draw_btn(dc, g_rcStepP, L"+", 0, 1);
    text_at(dc, g_rcStepM.left, 1, 80, 12, L"STEPS", C_DIM, g_fontTiny, DT_LEFT);

    draw_btn(dc, g_rcSong, L"SONG", g_proj->master.songMode, 1);
    draw_btn(dc, g_rcClock, L"MIDI CLK", g_eng->clockExternal, 1);
    draw_btn(dc, g_rcSave, L"保存", 0, 1);
    draw_btn(dc, g_rcLoad, L"打开", 0, 1);
    draw_btn(dc, g_rcWav, L"导出WAV", 0, 1);
    draw_btn(dc, g_rcMidi, L"导出MIDI", 0, 1);

    /* ---------- 网格 ---------- */
    int gx = GX, gy = GY;
    int gridW = LABEL_W + g_steps * g_cellW;
    fill(dc, (RECT)
    {
        gx, gy, gx + gridW, gy + HEAD_H
    }
    ,
    C_PANEL
    )
    ;
    for (int c = 0; c < g_steps; c++)
    {
        int x = gx + LABEL_W + c * g_cellW;
        if (c % 4 == 0) fill(dc, (RECT)
        {
            x, gy, x + 1, gy + HEAD_H + GRID_ROWS * CELL_H
        }
        ,
        RGB(58, 58, 58)
        )
        ;
        swprintf(buf, 32, L"%d", c + 1);
        text_at(dc, x, gy, g_cellW, HEAD_H, buf, C_DIM, g_fontTiny, DT_CENTER);
    }
    fill(dc, (RECT)
    {
        gx, gy + HEAD_H, gx + LABEL_W, gy + HEAD_H + GRID_ROWS * CELL_H
    }
    ,
    C_PANEL2
    )
    ;
    for (int r = 0; r < GRID_ROWS; r++)
    {
        int y = gy + HEAD_H + r * CELL_H;
        fill(dc, (RECT)
        {
            gx + LABEL_W, y, gx + gridW, y + CELL_H
        }
        ,
        (r % 2 == 0) ? RGB(22, 22, 22) : RGB(26, 26, 26)
        )
        ;
        text_at(dc, gx + 4, y, 34, CELL_H, L"" /* 占位 */, C_ORANGE, g_fontMono, DT_LEFT);
        {
            wchar_t code[8];
            MultiByteToWideChar(CP_UTF8, 0, XU_CODES[r], -1, code, 8);
            text_at(dc, gx + 4, y, 34, CELL_H, code, C_ORANGE, g_fontMono, DT_LEFT);
        }
        text_at(dc, gx + 38, y, 52, CELL_H, XU_NAMES_CN[r], C_TEXT, g_fontTiny, DT_LEFT);
        /* M / S */
        for (int j = 0; j < 2; j++)
        {
            int on = j == 0 ? g_proj->pads[r].mute : g_proj->pads[r].solo;
            RECT mb = {gx + 92 + j * 16, y + 4, gx + 107 + j * 16, y + CELL_H - 4};
            fill(dc, mb, on ? (j == 0 ? C_RED : C_YELLOW) : RGB(42, 42, 42));
            text_at(dc, mb.left, mb.top, 15, mb.bottom - mb.top,
                    j == 0 ? L"M" : L"S", on ? RGB(18, 18, 18) : C_DIM, g_fontTiny, DT_CENTER);
        }
        for (int c = 0; c < g_steps; c++)
        {
            XuStep* st = &pat->data[r][c];
            RECT cell = {
                gx + LABEL_W + c * g_cellW + 2, y + 2,
                gx + LABEL_W + (c + 1) * g_cellW - 2, y + CELL_H - 2
            };
            COLORREF col;
            if (st->on)
            {
                int a = 110 + (int)(145 * (st->velocity / 127.0));
                COLORREF base = st->accent ? C_YELLOW : C_ORANGE;
                col = RGB((GetRValue(base) * a + 18 * (255 - a)) / 255,
                          (GetGValue(base) * a + 18 * (255 - a)) / 255,
                          (GetBValue(base) * a + 18 * (255 - a)) / 255);
                fill(dc, cell, col);
                if (st->prob < 100 || st->ratchet > 1)
                    text_at(dc, cell.left, cell.top, cell.right - cell.left,
                            cell.bottom - cell.top, st->ratchet > 1 ? L"R" : L"%",
                            RGB(26, 16, 8), g_fontTiny, DT_CENTER);
            }
            else
            {
                fill(dc, cell, RGB(16, 16, 16));
                frame(dc, cell, RGB(39, 39, 39));
            }
            if (c == g_selStep && r == g_selPad)
            {
                HPEN pen = CreatePen(PS_SOLID, 1, C_CYAN);
                HGDIOBJ old = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, cell.left, cell.top, cell.right, cell.bottom);
                SelectObject(dc, ob);
                SelectObject(dc, old);
                DeleteObject(pen);
            }
        }
    }
    /* 播放头 */
    if (g_eng->playing)
    {
        int c = g_eng->stepIndex;
        if (c >= 0 && c < g_steps)
        {
            int x = gx + LABEL_W + c * g_cellW;
            HPEN pen = CreatePen(PS_SOLID, 2, C_CYAN);
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, x, gy + HEAD_H, NULL);
            LineTo(dc, x, gy + HEAD_H + GRID_ROWS * CELL_H);
            MoveToEx(dc, x + g_cellW, gy + HEAD_H, NULL);
            LineTo(dc, x + g_cellW, gy + HEAD_H + GRID_ROWS * CELL_H);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
    }

    /* ---------- 步进属性条 ---------- */
    {
        int y = STEP_ED_Y;
        fill(dc, (RECT)
        {
            GX, y, W - GX, y + STEP_ED_H
        }
        ,
        C_PANEL
        )
        ;
        XuStep* st = &pat->data[g_selPad][g_selStep];
        swprintf(buf, 128, L"STEP  %hs %02d", XU_CODES[g_selPad], g_selStep + 1);
        text_at(dc, GX + 8, y + 4, 160, 20, buf, C_ORANGE, g_fontBold, DT_LEFT);

        const wchar_t* labels[7] = {L"ON", L"ACC", L"VEL", L"PROB", L"MIC", L"RAT", L"COND"};
        int vals[7];
        vals[0] = st->on;
        vals[1] = st->accent;
        vals[2] = st->velocity;
        vals[3] = st->prob;
        vals[4] = st->micro;
        vals[5] = st->ratchet;
        vals[6] = st->cond;
        int x = GX + 8;
        for (int i = 0; i < 7; i++)
        {
            int boxW = (i == 6) ? 140 : 112;
            SetRect(&g_rcStepEd[i][0], x, y + 30, x + 20, y + 50);
            SetRect(&g_rcStepEd[i][1], x + 22, y + 30, x + boxW - 22, y + 50);
            SetRect(&g_rcStepEd[i][2], x + boxW - 20, y + 30, x + boxW, y + 50);
            text_at(dc, x, y + 14, boxW, 12, labels[i], C_DIM, g_fontTiny, DT_LEFT);
            draw_btn(dc, g_rcStepEd[i][0], L"-", 0, 1);
            draw_btn(dc, g_rcStepEd[i][2], L"+", 0, 1);
            if (i == 6)
                swprintf(buf, 64, L"%s",
                         vals[6] == 0
                             ? L"总是"
                             : vals[6] == 1
                             ? L"过门"
                             : vals[6] == 2
                             ? L"非过门"
                             : vals[6] == 3
                             ? L"每2"
                             : L"每4");
            else swprintf(buf, 64, L"%d", vals[i]);
            text_at(dc, g_rcStepEd[i][1].left, g_rcStepEd[i][1].top,
                    g_rcStepEd[i][1].right - g_rcStepEd[i][1].left, 20, buf,
                    C_TEXT, g_fontBold, DT_CENTER);
            x += boxW + 12;
        }
    }

    /* ---------- 声部参数 ---------- */
    {
        int y = PARAM_Y;
        fill(dc, (RECT)
        {
            GX, y, W - GX, y + PARAM_H
        }
        ,
        C_PANEL
        )
        ;
        wchar_t code[8];
        MultiByteToWideChar(CP_UTF8, 0, XU_CODES[g_selPad], -1, code, 8);
        swprintf(buf, 128, L"PAD %02d  %s  %s", g_selPad, code, XU_NAMES_CN[g_selPad]);
        text_at(dc, GX + 8, y + 2, 240, 16, buf, C_ORANGE, g_fontBold, DT_LEFT);
        XuPadParams* p = &g_proj->pads[g_selPad];
        int x = GX + 8;
        for (int i = 0; i < g_paramCount; i++)
        {
            float* fp = (float*)((char*)p + g_paramDefs[i].off);
            int mx = g_paramDefs[i].max;
            float v01 = (mx > 0) ? (*fp / (float)mx) : 0;
            draw_slider(dc, x, y + 22, 86, g_paramDefs[i].label, v01, 0, 0);
            /* 数值单独画在滑杆下方 */
            swprintf(buf, 32, L"%d", (int)(*fp * 100));
            text_at(dc, x, y + 60, 86, 14, buf, C_ORANGE, g_fontTiny, DT_LEFT);
            x += 94;
            if (x > W - 120) break;
        }
        /* LEVEL / PAN 的数值已经包含在循环里；右侧显示当前 pad 的 M/S 状态 */
        swprintf(buf, 64, L"%s%s", p->mute ? L"MUTE " : L"", p->solo ? L"SOLO" : L"");
        text_at(dc, W - 200, y + 4, 180, 16, buf, C_YELLOW, g_fontSmall, DT_RIGHT);
    }

    /* ---------- 主效果 ---------- */
    {
        int y = MASTER_Y;
        fill(dc, (RECT)
        {
            GX, y, W - GX, y + MASTER_H
        }
        ,
        C_PANEL
        )
        ;
        text_at(dc, GX + 8, y + 2, 200, 16, L"MASTER · 徐某的母线", C_ORANGE, g_fontBold, DT_LEFT);
        int x = GX + 8;
        for (int i = 0; i < 5; i++)
        {
            float v = 0.5f;
            int mx = 100;
            switch (i)
            {
            case 0: v = g_proj->master.lpCutoff / 20000.0f;
                mx = (int)g_proj->master.lpCutoff;
                break;
            case 1: v = g_proj->master.lpQ / 8.0f;
                mx = (int)(g_proj->master.lpQ * 100);
                break;
            case 2: v = g_proj->master.hpCutoff / 800.0f;
                mx = (int)g_proj->master.hpCutoff;
                break;
            case 3: v = g_proj->master.drive / 8.0f;
                mx = (int)(g_proj->master.drive * 100);
                break;
            case 4: v = g_proj->master.volume;
                mx = (int)(g_proj->master.volume * 100);
                break;
            }
            draw_slider(dc, x, y + 22, 130, g_masterDefs[i].label, v, 0, 0);
            swprintf(buf, 32, L"%d", mx);
            text_at(dc, x, y + 60, 130, 14, buf, C_ORANGE, g_fontTiny, DT_LEFT);
            x += 142;
        }
        /* 电平表 */
        int mx = W - 330;
        fill(dc, (RECT)
        {
            mx, y + 24, mx + 160, y + 34
        }
        ,
        RGB(12, 12, 12)
        )
        ;
        int lw = (int)(g_eng->meterL * 156);
        fill(dc, (RECT)
        {
            mx + 2, y + 26, mx + 2 + lw, y + 32
        }
        ,
        RGB(61, 220, 132)
        )
        ;
        fill(dc, (RECT)
        {
            mx, y + 40, mx + 160, y + 50
        }
        ,
        RGB(12, 12, 12)
        )
        ;
        int rw = (int)(g_eng->meterR * 156);
        fill(dc, (RECT)
        {
            mx + 2, y + 42, mx + 2 + rw, y + 48
        }
        ,
        RGB(61, 220, 132)
        )
        ;

        swprintf(buf, 128, L"CPU %.1f%%   XRUN %d   MIDI %s   %s",
                 g_eng->cpu * 100.0f, g_eng->xruns,
                 g_midiHandle ? L"ON" : L"-", g_audioOk ? L"48kHz" : L"无音频");
        text_at(dc, W - 330, y + 58, 320, 16, buf, C_DIM, g_fontTiny, DT_RIGHT);
    }
}

/* ---------------- 保存/打开对话框 ---------------- */
static int ask_save(wchar_t* file, const wchar_t* filter, const wchar_t* defExt)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn);
}

static int ask_open(wchar_t* file, const wchar_t* filter)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn);
}

/* ---------------- MIDI 输入 ---------------- */
static void CALLBACK midi_cb(HMIDIIN h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h;
    (void)inst;
    (void)p2;
    if (msg != MIM_DATA) return;
    LONG n = InterlockedCompareExchange(&g_midiCount, 0, 0);
    if (n >= 256) return;
    g_midiBuf[n][0] = (unsigned char)(p1 & 0xFF);
    g_midiBuf[n][1] = (unsigned char)((p1 >> 8) & 0xFF);
    g_midiBuf[n][2] = (unsigned char)((p1 >> 16) & 0xFF);
    InterlockedIncrement(&g_midiCount);
}

static void midi_start(void)
{
    if (midiInGetNumDevs() == 0) return;
    if (midiInOpen(&g_midiHandle, 0, (DWORD_PTR)midi_cb, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
        midiInStart(g_midiHandle);
}

static void midi_stop(void)
{
    if (g_midiHandle)
    {
        midiInStop(g_midiHandle);
        midiInReset(g_midiHandle);
        midiInClose(g_midiHandle);
        g_midiHandle = NULL;
    }
}

/* 处理 MIDI：Note On / CC / Program / 时钟 / 开始停止 */
static void handle_midi(void)
{
    LONG n = InterlockedExchange(&g_midiCount, 0);
    if (n <= 0) return;
    if (n > 256) n = 256;
    for (LONG i = 0; i < n; i++)
    {
        unsigned char st = g_midiBuf[i][0], d1 = g_midiBuf[i][1], d2 = g_midiBuf[i][2];
        unsigned char cmd = st & 0xF0;
        if (cmd == 0x90 && d2 > 0)
        {
            for (int pad = 0; pad < XU_PADS; pad++)
                if (XU_MIDI_NOTES[pad] == d1)
                {
                    xu_engine_trigger_pad(g_eng, pad, d2, d2 >= 120);
                    break;
                }
        }
        else if (cmd == 0xB0)
        {
            if (d1 == 7) g_proj->master.volume = d2 / 127.0f;
            else if (d1 == 74)
            {
                double f = 20.0 * pow(1000.0, d2 / 127.0);
                g_proj->master.lpCutoff = (float)f;
            }
        }
        else if (cmd == 0xC0)
        {
            xu_engine_request_pattern(g_eng, d2 % XU_PATTERNS);
        }
        else if (st == 0xFA)
        {
            xu_engine_play(g_eng);
        }
        else if (st == 0xFC)
        {
            xu_engine_stop(g_eng);
        }
        else if (st == 0xF8 && g_eng->clockExternal)
        {
            xu_engine_external_tick(g_eng);
        }
    }
}

/* ---------------- 鼠标处理 ---------------- */
static int in_rect(RECT r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void step_edit_adjust(int idx, int delta)
{
    XuStep* st = &g_proj->patterns[g_proj->current].data[g_selPad][g_selStep];
    switch (idx)
    {
    case 0: st->on = !st->on;
        break;
    case 1: st->accent = !st->accent;
        break;
    case 2: st->velocity = (unsigned char)(st->velocity + delta < 1
                                               ? 1
                                               : (st->velocity + delta > 127 ? 127 : st->velocity + delta));
        break;
    case 3: st->prob = (unsigned char)(st->prob + delta < 0 ? 0 : (st->prob + delta > 100 ? 100 : st->prob + delta));
        break;
    case 4: st->micro = (short)(st->micro + delta * 10 < -2400
                                    ? -2400
                                    : (st->micro + delta * 10 > 2400 ? 2400 : st->micro + delta * 10));
        break;
    case 5: st->ratchet = (unsigned char)(st->ratchet + delta < 1
                                              ? 1
                                              : (st->ratchet + delta > 8 ? 8 : st->ratchet + delta));
        break;
    case 6: st->cond = (unsigned char)((st->cond + delta + 5) % 5);
        break;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void on_lbutton(int x, int y)
{
    XuPattern* pat = &g_proj->patterns[g_proj->current];
    /* 顶部按钮 */
    if (in_rect(g_rcPlay, x, y))
    {
        xu_engine_play(g_eng);
        return;
    }
    if (in_rect(g_rcStop, x, y))
    {
        xu_engine_stop(g_eng);
        return;
    }
    if (in_rect(g_rcTap, x, y))
    {
        static DWORD last = 0;
        static double iv[8];
        static int n = 0;
        DWORD now = GetTickCount();
        if (last && now - last < 2000)
        {
            if (n < 8) iv[n++] = (now - last) / 1000.0;
            if (n >= 2)
            {
                double sum = 0;
                int cnt = 0;
                for (int i = 0; i < n; i++)
                {
                    sum += iv[i];
                    cnt++;
                }
                double bpm = 60.0 / (sum / cnt);
                if (bpm >= 40 && bpm <= 300) g_proj->master.bpm = (float)bpm;
            }
        }
        else n = 0;
        last = now;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (in_rect(g_rcBpmM, x, y))
    {
        g_proj->master.bpm -= 1;
        if (g_proj->master.bpm < 40) g_proj->master.bpm = 40;
        return;
    }
    if (in_rect(g_rcBpmP, x, y))
    {
        g_proj->master.bpm += 1;
        if (g_proj->master.bpm > 300) g_proj->master.bpm = 300;
        return;
    }
    if (in_rect(g_rcSwingM, x, y))
    {
        g_proj->master.swing -= 0.01f;
        if (g_proj->master.swing < 0) g_proj->master.swing = 0;
        return;
    }
    if (in_rect(g_rcSwingP, x, y))
    {
        g_proj->master.swing += 0.01f;
        if (g_proj->master.swing > 0.75f) g_proj->master.swing = 0.75f;
        return;
    }
    if (in_rect(g_rcPatM, x, y))
    {
        xu_engine_request_pattern(g_eng, g_proj->current - 1 + XU_PATTERNS);
        layout_grid(g_hwnd);
        return;
    }
    if (in_rect(g_rcPatP, x, y))
    {
        xu_engine_request_pattern(g_eng, (g_proj->current + 1) % XU_PATTERNS);
        layout_grid(g_hwnd);
        return;
    }
    if (in_rect(g_rcStepM, x, y))
    {
        if (pat->steps > 1) pat->steps--;
        layout_grid(g_hwnd);
        return;
    }
    if (in_rect(g_rcStepP, x, y))
    {
        if (pat->steps < 64) pat->steps++;
        layout_grid(g_hwnd);
        return;
    }
    if (in_rect(g_rcSong, x, y))
    {
        g_proj->master.songMode = !g_proj->master.songMode;
        return;
    }
    if (in_rect(g_rcClock, x, y))
    {
        g_eng->clockExternal = !g_eng->clockExternal;
        return;
    }

    if (in_rect(g_rcSave, x, y))
    {
        wchar_t file[MAX_PATH] = L"xu808.xu808";
        if (ask_save(file, L"XU-808 工程\0*.xu808\0所有文件\0*.*\0", L"xu808"))
            xu_project_save(g_proj, file);
        return;
    }
    if (in_rect(g_rcLoad, x, y))
    {
        wchar_t file[MAX_PATH] = L"";
        if (ask_open(file, L"XU-808 工程\0*.xu808\0所有文件\0*.*\0"))
        {
            /* 必须先停音频再改引擎：回调线程正在读 g_eng，
               如果直接 memset 会读到半截数据，轻则爆音重则崩溃。 */
            xu_audio_stop();
            xu_engine_stop(g_eng);
            xu_project_load(g_proj, file);
            xu_engine_init(g_eng, g_proj);
            xu_audio_start(g_eng);
            layout_grid(g_hwnd);
        }
        return;
    }
    if (in_rect(g_rcWav, x, y))
    {
        wchar_t file[MAX_PATH] = L"xu808.wav";
        if (ask_save(file, L"WAV 音频\0*.wav\0", L"wav"))
        {
            SetCursor(LoadCursor(NULL, IDC_WAIT));
            xu_export_wav(g_proj, file, g_proj->master.songMode, -1);
            SetCursor(LoadCursor(NULL, IDC_ARROW));
        }
        return;
    }
    if (in_rect(g_rcMidi, x, y))
    {
        wchar_t file[MAX_PATH] = L"xu808.mid";
        if (ask_save(file, L"MIDI\0*.mid\0", L"mid"))
            xu_export_midi(g_proj, file, g_proj->master.songMode);
        return;
    }

    /* 步进属性条 */
    for (int i = 0; i < 7; i++)
    {
        if (in_rect(g_rcStepEd[i][0], x, y))
        {
            step_edit_adjust(i, -1);
            return;
        }
        if (in_rect(g_rcStepEd[i][2], x, y))
        {
            step_edit_adjust(i, +1);
            return;
        }
        if (in_rect(g_rcStepEd[i][1], x, y) && (i == 0 || i == 1))
        {
            step_edit_adjust(i, +1);
            return;
        }
    }

    /* 网格 */
    int gy = GY + HEAD_H;
    if (x >= GX && x < GX + LABEL_W && y >= gy && y < gy + GRID_ROWS * CELL_H)
    {
        int r = (y - gy) / CELL_H;
        /* M / S */
        for (int j = 0; j < 2; j++)
        {
            RECT mb = {GX + 92 + j * 16, gy + r * CELL_H + 4, GX + 107 + j * 16, gy + r * CELL_H + CELL_H - 4};
            if (in_rect(mb, x, y))
            {
                if (j == 0) g_proj->pads[r].mute = !g_proj->pads[r].mute;
                else g_proj->pads[r].solo = !g_proj->pads[r].solo;
                InvalidateRect(g_hwnd, NULL, FALSE);
                return;
            }
        }
        g_selPad = r;
        build_param_defs(r);
        xu_engine_trigger_pad(g_eng, r, 112, 0);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    int gx = GX + LABEL_W;
    if (x >= gx && x < gx + g_steps * g_cellW && y >= gy && y < gy + GRID_ROWS * CELL_H)
    {
        int c = (x - gx) / g_cellW;
        int r = (y - gy) / CELL_H;
        if (c >= 0 && c < g_steps && r >= 0 && r < XU_PADS)
        {
            XuStep* st = &pat->data[r][c];
            st->on = !st->on;
            g_selPad = r;
            g_selStep = c;
            build_param_defs(r);
            if (st->on) xu_engine_trigger_pad(g_eng, r, st->velocity, st->accent);
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
        return;
    }

    /* 声部参数滑杆 */
    if (y >= PARAM_Y && y < PARAM_Y + PARAM_H)
    {
        int px = GX + 8;
        for (int i = 0; i < g_paramCount; i++)
        {
            RECT tr = {px, PARAM_Y + 42, px + 86, PARAM_Y + 50};
            if (x >= px && x < px + 86 && y >= PARAM_Y + 16 && y < PARAM_Y + 60)
            {
                XuPadParams* p = &g_proj->pads[g_selPad];
                float* fp = (float*)((char*)p + g_paramDefs[i].off);
                int mx = g_paramDefs[i].max;
                *fp = slider_value(tr, x) * mx;
                g_dragParam = i;
                SetCapture(g_hwnd);
                InvalidateRect(g_hwnd, NULL, FALSE);
                return;
            }
            px += 94;
        }
    }

    /* 主效果滑杆 */
    if (y >= MASTER_Y && y < MASTER_Y + MASTER_H)
    {
        int px = GX + 8;
        for (int i = 0; i < 5; i++)
        {
            RECT tr = {px, MASTER_Y + 42, px + 130, MASTER_Y + 50};
            if (x >= px && x < px + 130 && y >= MASTER_Y + 16 && y < MASTER_Y + 60)
            {
                float v = slider_value(tr, x);
                switch (i)
                {
                case 0: g_proj->master.lpCutoff = 200.0f + v * 19800.0f;
                    break;
                case 1: g_proj->master.lpQ = 0.5f + v * 7.5f;
                    break;
                case 2: g_proj->master.hpCutoff = 10.0f + v * 790.0f;
                    break;
                case 3: g_proj->master.drive = 1.0f + v * 7.0f;
                    break;
                case 4: g_proj->master.volume = v;
                    break;
                }
                g_dragMaster = i;
                SetCapture(g_hwnd);
                InvalidateRect(g_hwnd, NULL, FALSE);
                return;
            }
            px += 142;
        }
    }
}

static void on_rbutton(int x, int y)
{
    int gy = GY + HEAD_H;
    int gx = GX + LABEL_W;
    if (x >= gx && x < gx + g_steps * g_cellW && y >= gy && y < gy + GRID_ROWS * CELL_H)
    {
        int c = (x - gx) / g_cellW;
        int r = (y - gy) / CELL_H;
        if (c >= 0 && c < g_steps && r >= 0 && r < XU_PADS)
        {
            g_selPad = r;
            g_selStep = c;
            build_param_defs(r);
            InvalidateRect(g_hwnd, NULL, FALSE);
        }
    }
}

static void on_mouse_move(int x, int y)
{
    if (g_dragParam >= 0)
    {
        int px = GX + 8 + g_dragParam * 94;
        RECT tr = {px, PARAM_Y + 42, px + 86, PARAM_Y + 50};
        XuPadParams* p = &g_proj->pads[g_selPad];
        float* fp = (float*)((char*)p + g_paramDefs[g_dragParam].off);
        *fp = slider_value(tr, x) * g_paramDefs[g_dragParam].max;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    else if (g_dragMaster >= 0)
    {
        int px = GX + 8 + g_dragMaster * 142;
        RECT tr = {px, MASTER_Y + 42, px + 130, MASTER_Y + 50};
        float v = slider_value(tr, x);
        switch (g_dragMaster)
        {
        case 0: g_proj->master.lpCutoff = 200.0f + v * 19800.0f;
            break;
        case 1: g_proj->master.lpQ = 0.5f + v * 7.5f;
            break;
        case 2: g_proj->master.hpCutoff = 10.0f + v * 790.0f;
            break;
        case 3: g_proj->master.drive = 1.0f + v * 7.0f;
            break;
        case 4: g_proj->master.volume = v;
            break;
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* ---------------- 窗口过程 ---------------- */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        SetTimer(hwnd, 1, 33, NULL); /* 30fps 刷新；只用于 UI */
        return 0;

    case WM_SIZE:
        layout_grid(hwnd);
        return 0;

    case WM_TIMER:
        handle_midi();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            /* 双缓冲：先画到内存位图，避免闪烁 */
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            paint(hwnd, mem, rc.right, rc.bottom);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }

    case WM_LBUTTONDOWN:
        on_lbutton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_RBUTTONDOWN:
        on_rbutton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEMOVE:
        on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        if (g_dragParam >= 0 || g_dragMaster >= 0)
        {
            g_dragParam = -1;
            g_dragMaster = -1;
            ReleaseCapture();
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_SPACE)
        {
            if (g_eng->playing) xu_engine_stop(g_eng);
            else xu_engine_play(g_eng);
            return 0;
        }
        if (wp >= '1' && wp <= '8')
        {
            xu_engine_request_pattern(g_eng, wp - '1');
            layout_grid(hwnd);
            return 0;
        }
        /* 16 键鼓垫：Z X C V B N M A S D F G H J K L */
        {
            const char* keys = "ZXCVBNMASDFGHJKL";
            char ch = (char)toupper((int)wp);
            const char* p = strchr(keys, ch);
            if (p) xu_engine_trigger_pad(g_eng, (int)(p - keys), 110, 0);
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, 1);
        xu_engine_stop(g_eng);
        xu_audio_stop();
        midi_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------- 入口 ---------------- */
int xu_ui_run(HINSTANCE hInst, XuProject* p)
{
    g_inst = hInst;
    g_proj = p;

    /* 字体：只用系统自带的，绝不打包字体文件 */
    g_fontTiny = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontNorm = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontMono = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Consolas");
    g_fontLogo = CreateFontW(-34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Consolas");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"XU808_C_WINDOW";
    wc.hIcon = LoadIconW(hInst, L"APPICON");
    RegisterClassW(&wc);

    /* 固定窗口大小：布局是按 1500×900 设计的 */
    RECT rc = {0, 0, 1500, 900};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"XU808_C_WINDOW", L"XU-808 鼓机 - 徐某 (C 版)",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    g_eng = (XuEngine*)calloc(1, sizeof(XuEngine));
    if (!g_eng) return 1;
    xu_engine_init(g_eng, p);
    build_param_defs(0);
    layout_top();
    layout_grid(hwnd);

    g_audioOk = xu_audio_start(g_eng);
    midi_start();

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free(g_eng);
    return 0;
}