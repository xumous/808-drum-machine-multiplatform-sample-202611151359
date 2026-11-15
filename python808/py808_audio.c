/*
 * py808_audio.c —— Python 版本专用音频后端
 * ---------------------------------------------------------------------------
 * 使用 miniaudio（单头文件 C 库，WASAPI/CoreAudio/ALSA 后端）承载真正的
 * 音频回调线程；Python 只通过 ctypes 调用，回调内没有任何 Python 执行，
 * 因此不受 GIL、GC 与 Tk 事件循环影响。
 */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#include "miniaudio.h"

#include "core808.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#endif

static ma_device g_device;
static int g_opened = 0;
static c808_engine *g_engine = NULL;
static double g_sr = 48000.0;

static void p808_callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)dev;
    (void)input;
    if (!g_engine)
    {
        memset(output, 0, (size_t)frames * 2 * sizeof(float));
        return;
    }
#ifdef _WIN32
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        c808_render_interleaved(g_engine, (float *)output, (int)frames);
        QueryPerformanceCounter(&b);
        {
            double sec = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
            double budget = (double)frames / g_sr;
            static float ema = 0.0f;
            float load = (float)(sec / (budget > 1e-9 ? budget : 1e-9));
            ema = ema * 0.85f + load * 0.15f;
            c808_report_cpu(g_engine, ema);
            if (load > 1.6f)
                c808_report_xrun(g_engine);
        }
    }
#else
    c808_render_interleaved(g_engine, (float *)output, (int)frames);
#endif
}

__declspec(dllexport) c808_engine *p808_open(int *out_sample_rate, char *err, int errcap)
{
    ma_device_config cfg;
    ma_result r;
    int sr;

    if (g_opened)
        return g_engine;

    cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 0; /* 使用设备原生采样率 */
    cfg.periodSizeInMilliseconds = 10;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback = p808_callback;

    r = ma_device_init(NULL, &cfg, &g_device);
    if (r != MA_SUCCESS)
    {
        if (err && errcap > 0)
            snprintf(err, (size_t)errcap, "ma_device_init: %s", ma_result_description(r));
        return NULL;
    }
    sr = (int)g_device.sampleRate;
    g_sr = (double)sr;
    g_engine = c808_create((double)sr);
    if (!g_engine)
    {
        ma_device_uninit(&g_device);
        if (err && errcap > 0)
            snprintf(err, (size_t)errcap, "c808_create failed");
        return NULL;
    }
    r = ma_device_start(&g_device);
    if (r != MA_SUCCESS)
    {
        if (err && errcap > 0)
            snprintf(err, (size_t)errcap, "ma_device_start: %s", ma_result_description(r));
        c808_destroy(g_engine);
        g_engine = NULL;
        ma_device_uninit(&g_device);
        return NULL;
    }
    g_opened = 1;
    if (out_sample_rate)
        *out_sample_rate = sr;
    return g_engine;
}

__declspec(dllexport) void p808_close(void)
{
    if (g_opened)
    {
        ma_device_uninit(&g_device);
        g_opened = 0;
    }
    if (g_engine)
    {
        c808_destroy(g_engine);
        g_engine = NULL;
    }
}

__declspec(dllexport) int p808_device_name(char *out, int cap)
{
    if (!g_opened)
        return -1;
    if (out && cap > 0)
    {
        snprintf(out, (size_t)cap, "%s", g_device.playback.name ? g_device.playback.name : "default");
    }
    return 0;
}