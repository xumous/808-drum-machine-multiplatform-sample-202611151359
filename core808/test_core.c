/*
 * test_core.c —— 核心离线自检：
 *   1. 采样级步进时序（2 小节触发数必须精确）
 *   2. 数值稳定性（无 NaN/Inf、峰值不失控）
 *   3. 满负载压力渲染（16 声部 + ratchet + 饱和/滤波）
 *   4. WAV / MIDI / 工程存取
 */
#include "core808.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); failures++; } \
} while (0)

static int render_and_check(c808_engine* e, int frames, float* peakOut)
{
    float* L = (float*)malloc(sizeof(float) * 512);
    float* R = (float*)malloc(sizeof(float) * 512);
    float peak = 0.0f;
    int done = 0;
    int bad = 0;
    while (done < frames) {
        int n = frames - done > 512 ? 512 : frames - done;
        int i;
        c808_render(e, L, R, n);
        for (i = 0; i < n; ++i) {
            float a = fabsf(L[i]);
            float b = fabsf(R[i]);
            if (!(L[i] == L[i]) || !(R[i] == R[i]) || a > 100.0f || b > 100.0f) bad = 1;
            if (a > peak) peak = a;
            if (b > peak) peak = b;
        }
        done += n;
    }
    free(L); free(R);
    if (peakOut) *peakOut = peak;
    return bad;
}

static void peak_lr(c808_engine* e, int frames, float* lp, float* rp)
{
    float L[512];
    float R[512];
    int done = 0;
    float a = 0.0f, b = 0.0f;
    while (done < frames) {
        int n = frames - done > 512 ? 512 : frames - done;
        int i;
        c808_render(e, L, R, n);
        for (i = 0; i < n; ++i) {
            if (fabsf(L[i]) > a) a = fabsf(L[i]);
            if (fabsf(R[i]) > b) b = fabsf(R[i]);
        }
        done += n;
    }
    if (lp) *lp = a;
    if (rp) *rp = b;
}

int main(void)
{
    c808_engine* e;
    float peak = 0.0f;
    int t, s;
    clock_t c0, c1;

    printf("=== core808 self test ===\n");
    e = c808_create(48000.0);
    CHECK(e != NULL, "engine create");

    /* 1) 两小节时序：默认 pattern 共 16 个触发/16 步 */
    c808_transport_play(e);
    render_and_check(e, 6000 * 16 * 2, &peak);
    CHECK(c808_trigger_count(e) == 32, "sample-accurate 2-bar trigger count == 32");
    CHECK(peak > 0.02f && peak <= 1.0f, "audio is audible and peak <= 1.0");
    CHECK(peak > 0.2f, "default beat has healthy level");
    /* 等功率声像回归：默认所有轨 PAN=50%，左右声道都必须有信号 */
    {
        float lp = 0.0f, rp = 0.0f;
        peak_lr(e, 6000 * 16, &lp, &rp);
        printf("[INFO] center pan peaks L=%.3f R=%.3f\n", lp, rp);
        CHECK(lp > 0.05f && rp > 0.05f, "center pan produces signal on both L and R");
        CHECK(fabsf(lp - rp) < lp * 0.25f + 0.05f, "center pan is roughly balanced");
    }

    /* 2) 停止、清空后重新加载默认工程 */
    c808_transport_stop_reset(e);
    render_and_check(e, 512, NULL);

    /* 3) 满负载：16 轨全部步进 on，ratchet=2，重音/概率 100 */
    for (t = 0; t < C808_TRACKS; ++t) {
        for (s = 0; s < 16; ++s) {
            c808_set_step_on(e, 0, t, s, 1);
            c808_set_step_vel(e, 0, t, s, 110);
            c808_set_step_ratchet(e, 0, t, s, 2);
        }
    }
    c808_set_master(e, C808_M_DRIVE, 0.65f);
    c808_set_master(e, C808_M_RESO, 0.6f);
    c808_transport_play(e);
    c0 = clock();
    {
        int bad = render_and_check(e, 48000 * 20, &peak); /* 20 秒 */
        c1 = clock();
        CHECK(!bad, "stress render finite & bounded");
        printf("[INFO] stress peak=%.3f, render 20s took %.2fs (%.1fx realtime)\n",
               peak, (double)(c1 - c0) / CLOCKS_PER_SEC,
               20.0 / (((double)(c1 - c0) / CLOCKS_PER_SEC) + 1e-9));
        CHECK(((double)(c1 - c0) / CLOCKS_PER_SEC) < 20.0, "stress render faster than realtime");
        CHECK(c808_trigger_count(e) > 3000, "stress produced dense triggers");
    }
    c808_transport_stop_reset(e);

    /* 4) 工程保存/加载 */
    c808_set_bpm(e, 141.0f);
    render_and_check(e, 256, NULL);
    CHECK(c808_save_project(e, "test_project.808") == 0, "project save");
    c808_set_bpm(e, 90.0f);
    render_and_check(e, 256, NULL);
    CHECK(c808_load_project(e, "test_project.808") == 0, "project load");
    render_and_check(e, 256, NULL);
    CHECK(fabsf(c808_project_ro(e)->bpm - 141.0f) < 0.01f, "project BPM restored");

    /* 5) 导出 WAV / MIDI */
    {
        FILE* f;
        long sz;
        CHECK(c808_export_wav(e, "test_out.wav", 0, 1, 1, 0) == 0, "export WAV");
        f = fopen("test_out.wav", "rb");
        CHECK(f != NULL, "WAV exists");
        if (f) {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
            printf("[INFO] WAV size = %ld bytes\n", sz);
            CHECK(sz > 44 + 48000, "WAV contains audio data");
        }
        CHECK(c808_export_midi(e, "test_out.mid", 0, 2) == 0, "export MIDI");
        f = fopen("test_out.mid", "rb");
        CHECK(f != NULL, "MIDI exists");
        if (f) {
            char m[4];
            fread(m, 1, 4, f);
            fclose(f);
            CHECK(memcmp(m, "MThd", 4) == 0, "MIDI header valid");
        }
        CHECK(c808_export_wav(e, "test_stem.wav", 0, 1, 0, 1) == 0, "export stems");
    }

    c808_destroy(e);
    printf("=== %s (%d failures) ===\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
