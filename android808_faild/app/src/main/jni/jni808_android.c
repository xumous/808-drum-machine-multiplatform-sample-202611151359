/*
 * jni808_android.c —— Android JNI 桥（core808.c / core808_io.c）
 * ---------------------------------------------------------------------------
 * 1. Java 音频线程调用 renderS16 / renderF32，JNI 内部分块渲染并直接写入
 *    Java 数组（GetPrimitiveArrayCritical），不分配堆内存、不加锁。
 * 2. 所有 UI -> 音频线程参数都走 core808 的无锁命令队列。
 * 3. 工程保存/加载、WAV/MIDI 导出在工作线程调用，core808_io.c 内部会创建
 *    独立的离线引擎，不干扰实时实例。
 */
#include <jni.h>
#include <string.h>
#include <time.h>

#include "core808.h"

#define EXPORT __attribute__((visibility("default")))

static c808_engine* eng(jlong h) { return (c808_engine*)(intptr_t)h; }

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

EXPORT jlong JNICALL
Java_com_guji808_android_Native808_create(JNIEnv* env, jclass cls, jdouble sr)
{
    (void)env; (void)cls;
    return (jlong)(intptr_t)c808_create(sr > 0.0 ? sr : 48000.0);
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_destroy(JNIEnv* env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    if (h) c808_destroy(eng(h));
}

static void report_load(jlong h, int frames, double t0)
{
    double sec = now_seconds() - t0;
    double budget = (double)frames / C808_SR_DEFAULT;
    float load = (float)(sec / (budget > 1e-9 ? budget : 1e-9));
    static float ema = 0.0f;
    ema = ema * 0.85f + load * 0.15f;
    c808_report_cpu(eng(h), ema);
    if (load > 1.6f) c808_report_xrun(eng(h));
}

/* 渲染到 Java byte[]：16-bit little-endian 交错立体声 */
EXPORT void JNICALL
Java_com_guji808_android_Native808_renderS16(JNIEnv* env, jclass cls,
                                            jlong h, jbyteArray out, jint frames)
{
    float l[512], r[512];
    jbyte* buf;
    int done = 0;
    double t0 = now_seconds();
    (void)cls;
    if (!h || !out || frames <= 0) return;
    buf = (jbyte*)(*env)->GetPrimitiveArrayCritical(env, out, NULL);
    if (!buf) return;
    while (done < frames) {
        int n = frames - done;
        int i;
        if (n > 512) n = 512;
        c808_render(eng(h), l, r, n);
        for (i = 0; i < n; ++i) {
            int v0 = (int)(l[i] * 32767.0f);
            int v1 = (int)(r[i] * 32767.0f);
            int base = (done + i) * 4;
            if (v0 > 32767) v0 = 32767;
            if (v0 < -32768) v0 = -32768;
            if (v1 > 32767) v1 = 32767;
            if (v1 < -32768) v1 = -32768;
            buf[base + 0] = (jbyte)(v0 & 0xff);
            buf[base + 1] = (jbyte)((v0 >> 8) & 0xff);
            buf[base + 2] = (jbyte)(v1 & 0xff);
            buf[base + 3] = (jbyte)((v1 >> 8) & 0xff);
        }
        done += n;
    }
    (*env)->ReleasePrimitiveArrayCritical(env, out, buf, 0);
    report_load(h, frames, t0);
}

/* 渲染到 Java float[]：32-bit float 交错立体声（AudioTrack ENCODING_PCM_FLOAT） */
EXPORT void JNICALL
Java_com_guji808_android_Native808_renderF32(JNIEnv* env, jclass cls,
                                            jlong h, jfloatArray out, jint frames)
{
    jfloat* buf;
    double t0 = now_seconds();
    (void)cls;
    if (!h || !out || frames <= 0) return;
    buf = (jfloat*)(*env)->GetPrimitiveArrayCritical(env, out, NULL);
    if (!buf) return;
    c808_render_interleaved(eng(h), (float*)buf, frames);
    (*env)->ReleasePrimitiveArrayCritical(env, out, buf, 0);
    report_load(h, frames, t0);
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_snapshot(JNIEnv* env, jclass cls, jlong h,
                                            jint pat, jintArray meta, jfloatArray params,
                                            jbyteArray steps, jbyteArray flags)
{
    jint* m = (*env)->GetPrimitiveArrayCritical(env, meta, NULL);
    jfloat* p = (*env)->GetPrimitiveArrayCritical(env, params, NULL);
    jbyte* s = (*env)->GetPrimitiveArrayCritical(env, steps, NULL);
    jbyte* f = (*env)->GetPrimitiveArrayCritical(env, flags, NULL);
    (void)cls;
    if (m && p && s && f) {
        c808_snapshot(eng(h), pat, (int32_t*)m, (float*)p, (uint8_t*)s, (uint8_t*)f);
    }
    if (f) (*env)->ReleasePrimitiveArrayCritical(env, flags, f, 0);
    if (s) (*env)->ReleasePrimitiveArrayCritical(env, steps, s, 0);
    if (p) (*env)->ReleasePrimitiveArrayCritical(env, params, p, 0);
    if (m) (*env)->ReleasePrimitiveArrayCritical(env, meta, m, 0);
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_status(JNIEnv* env, jclass cls, jlong h,
                                          jintArray ints, jfloatArray meters, jfloatArray misc)
{
    c808_status st;
    jint* ip = (*env)->GetPrimitiveArrayCritical(env, ints, NULL);
    jfloat* mp = (*env)->GetPrimitiveArrayCritical(env, meters, NULL);
    jfloat* xp = (*env)->GetPrimitiveArrayCritical(env, misc, NULL);
    (void)cls;
    memset(&st, 0, sizeof(st));
    c808_get_status(eng(h), &st);
    if (ip) {
        ip[0] = st.playing; ip[1] = st.pattern; ip[2] = st.step;
        ip[3] = st.songPos; ip[4] = st.loopCount; ip[5] = st.xruns;
    }
    if (mp) {
        int i;
        for (i = 0; i < C808_TRACKS; ++i) mp[i] = st.meter[i];
    }
    if (xp) { xp[0] = st.cpuLoad; xp[1] = st.masterL; xp[2] = st.masterR; }
    if (xp) (*env)->ReleasePrimitiveArrayCritical(env, misc, xp, 0);
    if (mp) (*env)->ReleasePrimitiveArrayCritical(env, meters, mp, 0);
    if (ip) (*env)->ReleasePrimitiveArrayCritical(env, ints, ip, 0);
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_setStep(JNIEnv* env, jclass cls, jlong h,
                                           jint pat, jint t, jint s, jint field, jint value)
{
    (void)env; (void)cls;
    switch (field) {
    case 0: c808_set_step_on(eng(h), pat, t, s, value); break;
    case 1: c808_set_step_vel(eng(h), pat, t, s, value); break;
    case 2: c808_set_step_accent(eng(h), pat, t, s, value); break;
    case 3: c808_set_step_prob(eng(h), pat, t, s, value); break;
    case 4: c808_set_step_ratchet(eng(h), pat, t, s, value); break;
    case 5: c808_set_step_rate(eng(h), pat, t, s, value); break;
    case 6: c808_set_step_cond(eng(h), pat, t, s, value, 2); break;
    case 7: c808_set_step_cond(eng(h), pat, t, s, 0, value); break;
    case 8: c808_set_step_micro(eng(h), pat, t, s, value); break;
    default: break;
    }
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_setCond(JNIEnv* env, jclass cls, jlong h,
                                           jint pat, jint t, jint s, jint cond, jint n)
{
    (void)env; (void)cls;
    c808_set_step_cond(eng(h), pat, t, s, cond, n);
}

EXPORT void JNICALL
Java_com_guji808_android_Native808_setTrackParam(JNIEnv* env, jclass cls, jlong h,
                                                 jint t, jint p, jfloat v)
{ (void)env; (void)cls; c808_set_track_param(eng(h), t, p, v); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setMaster(JNIEnv* env, jclass cls, jlong h, jint p, jfloat v)
{ (void)env; (void)cls; c808_set_master(eng(h), p, v); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setBpm(JNIEnv* env, jclass cls, jlong h, jfloat v)
{ (void)env; (void)cls; c808_set_bpm(eng(h), v); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setTimesig(JNIEnv* env, jclass cls, jlong h, jint n, jint d)
{ (void)env; (void)cls; c808_set_timesig(eng(h), n, d); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setClockSource(JNIEnv* env, jclass cls, jlong h, jint midi)
{ (void)env; (void)cls; c808_set_clock_source(eng(h), midi ? 1 : 0); }

EXPORT jint JNICALL
Java_com_guji808_android_Native808_getClockSource(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; return c808_get_clock_source(eng(h)); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setPatternLen(JNIEnv* env, jclass cls, jlong h, jint pat, jint len)
{ (void)env; (void)cls; c808_set_pattern_len(eng(h), pat, len); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setSwing(JNIEnv* env, jclass cls, jlong h, jint pat, jint sw)
{ (void)env; (void)cls; c808_set_pattern_swing(eng(h), pat, sw); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_selectPattern(JNIEnv* env, jclass cls, jlong h, jint pat)
{ (void)env; (void)cls; c808_select_pattern(eng(h), pat); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setPatternCount(JNIEnv* env, jclass cls, jlong h, jint n)
{ (void)env; (void)cls; c808_set_pattern_count(eng(h), n); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setMute(JNIEnv* env, jclass cls, jlong h, jint t, jint v)
{ (void)env; (void)cls; c808_set_mute(eng(h), t, v); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setSolo(JNIEnv* env, jclass cls, jlong h, jint t, jint v)
{ (void)env; (void)cls; c808_set_solo(eng(h), t, v); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setTrackLen(JNIEnv* env, jclass cls, jlong h, jint t, jint len)
{ (void)env; (void)cls; c808_set_track_len(eng(h), t, len); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_play(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_play(eng(h)); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_stop(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_stop(eng(h)); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_stopReset(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_stop_reset(eng(h)); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_noteOn(JNIEnv* env, jclass cls, jlong h, jint t, jint vel)
{ (void)env; (void)cls; c808_note_on(eng(h), t, vel, 0); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_panic(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_panic(eng(h)); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_clearPattern(JNIEnv* env, jclass cls, jlong h, jint pat)
{ (void)env; (void)cls; c808_clear_pattern(eng(h), pat); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_clearTrack(JNIEnv* env, jclass cls, jlong h, jint pat, jint t)
{ (void)env; (void)cls; c808_clear_track(eng(h), pat, t); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_copyPattern(JNIEnv* env, jclass cls, jlong h, jint src, jint dst)
{ (void)env; (void)cls; c808_copy_pattern(eng(h), src, dst); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setSongOn(JNIEnv* env, jclass cls, jlong h, jint on)
{ (void)env; (void)cls; c808_set_song_on(eng(h), on); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setSongLen(JNIEnv* env, jclass cls, jlong h, jint len)
{ (void)env; (void)cls; c808_set_song_len(eng(h), len); }

EXPORT void JNICALL
Java_com_guji808_android_Native808_setSongEntry(JNIEnv* env, jclass cls, jlong h,
                                                jint i, jint pat, jint reps)
{ (void)env; (void)cls; c808_set_song_entry(eng(h), i, pat, reps); }

EXPORT jint JNICALL
Java_com_guji808_android_Native808_songLen(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; return c808_get_song_len(eng(h)); }

EXPORT jint JNICALL
Java_com_guji808_android_Native808_songPat(JNIEnv* env, jclass cls, jlong h, jint i)
{
    int pat = 0, reps = 1;
    (void)env; (void)cls;
    c808_get_song_entry(eng(h), i, &pat, &reps);
    return pat;
}

EXPORT jint JNICALL
Java_com_guji808_android_Native808_songReps(JNIEnv* env, jclass cls, jlong h, jint i)
{
    int pat = 0, reps = 1;
    (void)env; (void)cls;
    c808_get_song_entry(eng(h), i, &pat, &reps);
    return reps;
}

EXPORT jint JNICALL
Java_com_guji808_android_Native808_pushMidi(JNIEnv* env, jclass cls, jlong h,
                                            jint type, jint a, jint b, jint c)
{ (void)env; (void)cls; return c808_push_midi(eng(h), type, a, b, c); }

/* ------------------------------------------------------------------------- */
/* 文件 I/O（工作线程）                                                       */
/* ------------------------------------------------------------------------- */
EXPORT jint JNICALL
Java_com_guji808_android_Native808_save(JNIEnv* env, jclass cls, jlong h, jstring path)
{
    const char* p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    int rc = p ? c808_save_project(eng(h), p) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL
Java_com_guji808_android_Native808_load(JNIEnv* env, jclass cls, jlong h, jstring path)
{
    const char* p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    int rc = p ? c808_load_project(eng(h), p) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL
Java_com_guji808_android_Native808_exportWav(JNIEnv* env, jclass cls, jlong h, jstring path,
                                             jint mode, jint loops, jint tail, jint stems)
{
    const char* p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    int rc = p ? c808_export_wav(eng(h), p, mode, loops, tail, stems) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL
Java_com_guji808_android_Native808_exportMidi(JNIEnv* env, jclass cls, jlong h, jstring path,
                                              jint mode, jint loops)
{
    const char* p = path ? (*env)->GetStringUTFChars(env, path, NULL) : NULL;
    int rc = p ? c808_export_midi(eng(h), p, mode, loops) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}
