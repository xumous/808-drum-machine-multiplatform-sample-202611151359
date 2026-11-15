/*
 * jni808.c —— Java 桌面版 JNI 桥
 * 音频线程由 Java 的 SourceDataLine 写线程驱动，本 JNI 只做零分配渲染与参数传递。
 */
#include <jni.h>
#include <windows.h>
#include <avrt.h>
#include <string.h>

#include "core808.h"

#define EXPORT __declspec(dllexport)

static double g_sample_rate = 48000.0;

static c808_engine* eng(jlong h) { return (c808_engine*)(intptr_t)h; }

EXPORT jlong JNICALL Java_native808_Native808_create(JNIEnv* env, jclass cls, jdouble sr)
{
    (void)env; (void)cls;
    g_sample_rate = sr > 0 ? (double)sr : 48000.0;
    return (jlong)(intptr_t)c808_create(g_sample_rate);
}

EXPORT void JNICALL Java_native808_Native808_destroy(JNIEnv* env, jclass cls, jlong h)
{
    (void)env; (void)cls;
    if (h) c808_destroy(eng(h));
}

/*
 * 渲染到 Java byte[]（16-bit little-endian stereo），使用临界区避免拷贝。
 * 音频线程调用；内部仅使用栈上缓冲，不分配堆内存。
 */
EXPORT void JNICALL Java_native808_Native808_renderS16(JNIEnv* env, jclass cls,
                                                      jlong h, jbyteArray out, jint frames)
{
    float l[1024], r[1024];
    jbyte* buf;
    int done = 0;
    (void)cls;
    if (!h || frames <= 0) return;
    buf = (jbyte*)(*env)->GetPrimitiveArrayCritical(env, out, NULL);
    if (!buf) return;
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    while (done < frames) {
        int n = frames - done;
        int i;
        if (n > 1024) n = 1024;
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
        QueryPerformanceCounter(&t1);
        {
            double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
            double budget = (double)frames / g_sample_rate;
            static float ema = 0.0f;
            float load = (float)(sec / (budget > 1e-9 ? budget : 1e-9));
            ema = ema * 0.85f + load * 0.15f;
            c808_report_cpu(eng(h), ema);
            if (load > 1.6f) c808_report_xrun(eng(h));
        }
    }
    (*env)->ReleasePrimitiveArrayCritical(env, out, buf, 0);
}

EXPORT void JNICALL Java_native808_Native808_snapshot(JNIEnv* env, jclass cls, jlong h,
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

EXPORT void JNICALL Java_native808_Native808_status(JNIEnv* env, jclass cls, jlong h,
                                                    jintArray ints, jfloatArray meters,
                                                    jfloatArray misc)
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

EXPORT void JNICALL Java_native808_Native808_setStep(JNIEnv* env, jclass cls, jlong h,
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

EXPORT void JNICALL Java_native808_Native808_setCond(JNIEnv* env, jclass cls, jlong h,
                                                     jint pat, jint t, jint s, jint cond, jint n)
{
    (void)env; (void)cls;
    c808_set_step_cond(eng(h), pat, t, s, cond, n);
}

EXPORT void JNICALL Java_native808_Native808_setTrackParam(JNIEnv* env, jclass cls, jlong h,
                                                           jint t, jint p, jfloat v)
{ (void)env; (void)cls; c808_set_track_param(eng(h), t, p, v); }

EXPORT void JNICALL Java_native808_Native808_setMaster(JNIEnv* env, jclass cls, jlong h,
                                                       jint p, jfloat v)
{ (void)env; (void)cls; c808_set_master(eng(h), p, v); }

EXPORT void JNICALL Java_native808_Native808_setBpm(JNIEnv* env, jclass cls, jlong h, jfloat v)
{ (void)env; (void)cls; c808_set_bpm(eng(h), v); }

EXPORT void JNICALL Java_native808_Native808_setPatternLen(JNIEnv* env, jclass cls, jlong h,
                                                           jint pat, jint len)
{ (void)env; (void)cls; c808_set_pattern_len(eng(h), pat, len); }

EXPORT void JNICALL Java_native808_Native808_setSwing(JNIEnv* env, jclass cls, jlong h,
                                                      jint pat, jint sw)
{ (void)env; (void)cls; c808_set_pattern_swing(eng(h), pat, sw); }

EXPORT void JNICALL Java_native808_Native808_setTimesig(JNIEnv* env, jclass cls, jlong h,
                                                        jint num, jint den)
{ (void)env; (void)cls; c808_set_timesig(eng(h), num, den); }

EXPORT void JNICALL Java_native808_Native808_setClock(JNIEnv* env, jclass cls, jlong h, jint midi)
{ (void)env; (void)cls; c808_set_clock_source(eng(h), midi); }

EXPORT jint JNICALL Java_native808_Native808_clockSource(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; return c808_get_clock_source(eng(h)); }

EXPORT void JNICALL Java_native808_Native808_setTrackLen(JNIEnv* env, jclass cls, jlong h,
                                                         jint t, jint len)
{ (void)env; (void)cls; c808_set_track_len(eng(h), t, len); }

EXPORT void JNICALL Java_native808_Native808_selectPattern(JNIEnv* env, jclass cls, jlong h, jint pat)
{ (void)env; (void)cls; c808_select_pattern(eng(h), pat); }

EXPORT void JNICALL Java_native808_Native808_setMute(JNIEnv* env, jclass cls, jlong h, jint t, jint v)
{ (void)env; (void)cls; c808_set_mute(eng(h), t, v); }

EXPORT void JNICALL Java_native808_Native808_setSolo(JNIEnv* env, jclass cls, jlong h, jint t, jint v)
{ (void)env; (void)cls; c808_set_solo(eng(h), t, v); }

EXPORT void JNICALL Java_native808_Native808_play(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_play(eng(h)); }

EXPORT void JNICALL Java_native808_Native808_stop(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_stop(eng(h)); }

EXPORT void JNICALL Java_native808_Native808_stopReset(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_transport_stop_reset(eng(h)); }

EXPORT void JNICALL Java_native808_Native808_noteOn(JNIEnv* env, jclass cls, jlong h, jint t, jint vel)
{ (void)env; (void)cls; c808_note_on(eng(h), t, vel, 0); }

EXPORT void JNICALL Java_native808_Native808_clearPattern(JNIEnv* env, jclass cls, jlong h, jint pat)
{ (void)env; (void)cls; c808_clear_pattern(eng(h), pat); }

EXPORT void JNICALL Java_native808_Native808_copyPattern(JNIEnv* env, jclass cls, jlong h,
                                                         jint src, jint dst)
{ (void)env; (void)cls; c808_copy_pattern(eng(h), src, dst); }

EXPORT void JNICALL Java_native808_Native808_setSongOn(JNIEnv* env, jclass cls, jlong h, jint on)
{ (void)env; (void)cls; c808_set_song_on(eng(h), on); }

EXPORT void JNICALL Java_native808_Native808_setSongLen(JNIEnv* env, jclass cls, jlong h, jint len)
{ (void)env; (void)cls; c808_set_song_len(eng(h), len); }

EXPORT void JNICALL Java_native808_Native808_setSongEntry(JNIEnv* env, jclass cls, jlong h,
                                                          jint i, jint pat, jint reps)
{ (void)env; (void)cls; c808_set_song_entry(eng(h), i, pat, reps); }

EXPORT jint JNICALL Java_native808_Native808_songLen(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; return c808_get_song_len(eng(h)); }

EXPORT jint JNICALL Java_native808_Native808_songPat(JNIEnv* env, jclass cls, jlong h, jint i)
{
    int pat = 0, reps = 1;
    (void)env; (void)cls;
    c808_get_song_entry(eng(h), i, &pat, &reps);
    return pat;
}

EXPORT jint JNICALL Java_native808_Native808_songReps(JNIEnv* env, jclass cls, jlong h, jint i)
{
    int pat = 0, reps = 1;
    (void)env; (void)cls;
    c808_get_song_entry(eng(h), i, &pat, &reps);
    return reps;
}

static jint export_call(JNIEnv* env, jlong h, jstring path, int kind, int mode, int loops, int stems)
{
    const char* p;
    int rc;
    if (!path) return -2;
    p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p) return -2;
    if (kind == 0) rc = c808_export_wav(eng(h), p, mode, loops, 2, stems);
    else rc = c808_export_midi(eng(h), p, mode, loops);
    (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL Java_native808_Native808_save(JNIEnv* env, jclass cls, jlong h, jstring path)
{
    const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    int rc = p ? c808_save_project(eng(h), p) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL Java_native808_Native808_load(JNIEnv* env, jclass cls, jlong h, jstring path)
{
    const char* p = (*env)->GetStringUTFChars(env, path, NULL);
    int rc = p ? c808_load_project(eng(h), p) : -2;
    (void)cls;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

EXPORT jint JNICALL Java_native808_Native808_exportWav(JNIEnv* env, jclass cls, jlong h,
                                                       jstring path, jint mode, jint loops, jint stems)
{ (void)cls; return export_call(env, h, path, 0, mode, loops, stems); }

EXPORT jint JNICALL Java_native808_Native808_exportMidi(JNIEnv* env, jclass cls, jlong h,
                                                        jstring path, jint mode, jint loops)
{ (void)cls; return export_call(env, h, path, 1, mode, loops, 0); }

EXPORT jint JNICALL Java_native808_Native808_midiCount(JNIEnv* env, jclass cls)
{ (void)env; (void)cls; return c808_midi_device_count(); }

EXPORT jstring JNICALL Java_native808_Native808_midiName(JNIEnv* env, jclass cls, jint idx)
{
    char buf[256];
    (void)cls;
    buf[0] = 0;
    c808_midi_device_name(idx, buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

EXPORT jint JNICALL Java_native808_Native808_midiStart(JNIEnv* env, jclass cls, jlong h, jint idx)
{ (void)env; (void)cls; return c808_midi_start(eng(h), idx); }

EXPORT void JNICALL Java_native808_Native808_midiStop(JNIEnv* env, jclass cls, jlong h)
{ (void)env; (void)cls; c808_midi_stop(eng(h)); }

EXPORT void JNICALL Java_native808_Native808_boostThread(JNIEnv* env, jclass cls)
{
    DWORD task = 0;
    (void)env; (void)cls;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}
