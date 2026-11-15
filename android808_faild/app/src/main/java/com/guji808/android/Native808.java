package com.guji808.android;

/**
 * core808 JNI 桥。所有方法都是无锁命令或只读快照；
 * 实时 DSP 只发生在 AudioEngine808 的音频线程里（renderS16 / renderF32）。
 */
public final class Native808 {
    public static final int TRACKS = 16;
    public static final int STEPS = 64;
    public static final int PATTERNS = 256;
    /* c808_field：与 C808_F_* 一致 */
    public static final int F_ON = 0, F_VEL = 1, F_ACCENT = 2, F_PROB = 3,
            F_RATCHET = 4, F_RATE = 5, F_COND = 6, F_CONDN = 7, F_MICRO = 8;
    /* 主参数 */
    public static final int M_VOLUME = 0, M_DRIVE = 1, M_HP = 2, M_LP = 3,
            M_RESO = 4, M_ACCENT = 5;

    static {
        System.loadLibrary("808core");
    }

    private Native808() {
    }

    public static native long create(double sampleRate);

    public static native void destroy(long e);

    public static native void renderS16(long e, byte[] out, int frames);

    public static native void renderF32(long e, float[] out, int frames);

    public static native void snapshot(long e, int pat, int[] meta, float[] params,
                                       byte[] steps, byte[] flags);

    public static native void status(long e, int[] ints, float[] meters, float[] misc);

    public static native void setStep(long e, int pat, int track, int step, int field, int value);

    public static native void setCond(long e, int pat, int track, int step, int cond, int n);

    public static native void setTrackParam(long e, int track, int param, float value);

    public static native void setMaster(long e, int param, float value);

    public static native void setBpm(long e, float bpm);

    public static native void setTimesig(long e, int num, int den);

    public static native void setClockSource(long e, int midiClock);

    public static native int getClockSource(long e);

    public static native void setPatternLen(long e, int pat, int len);

    public static native void setSwing(long e, int pat, int swing);

    public static native void selectPattern(long e, int pat);

    public static native void setPatternCount(long e, int count);

    public static native void setMute(long e, int track, int mute);

    public static native void setSolo(long e, int track, int solo);

    public static native void setTrackLen(long e, int track, int len);

    public static native void play(long e);

    public static native void stop(long e);

    public static native void stopReset(long e);

    public static native void noteOn(long e, int track, int vel);

    public static native void panic(long e);

    public static native void clearPattern(long e, int pat);

    public static native void clearTrack(long e, int pat, int track);

    public static native void copyPattern(long e, int src, int dst);

    public static native void setSongOn(long e, int on);

    public static native void setSongLen(long e, int len);

    public static native void setSongEntry(long e, int index, int pat, int reps);

    public static native int songLen(long e);

    public static native int songPat(long e, int index);

    public static native int songReps(long e, int index);

    public static native int pushMidi(long e, int type, int a, int b, int c);

    public static native int save(long e, String path);

    public static native int load(long e, String path);

    public static native int exportWav(long e, String path, int mode, int loops, int tail, int stems);

    public static native int exportMidi(long e, String path, int mode, int loops);
}
