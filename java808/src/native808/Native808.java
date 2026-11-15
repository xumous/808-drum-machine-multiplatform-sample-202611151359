package native808;

import java.io.File;

/**
 * JNI 桥（808jni.dll -> core808.c）。
 * 所有 native 方法均为无锁命令或只读快照，实时渲染只发生在 Java 音频线程。
 */
public final class Native808 {
    static {
        String appPath = System.getProperty("jpackage.app-path");
        boolean loaded = false;
        if (appPath != null) {
            File root = new File(appPath).getParentFile();
            File dll = new File(root, "app/808jni.dll");
            if (dll.isFile()) {
                System.load(dll.getAbsolutePath());
                loaded = true;
            }
        }
        if (!loaded) {
            try {
                System.loadLibrary("808jni");
            } catch (UnsatisfiedLinkError e) {
                String cp = System.getProperty("java.class.path");
                File dll = new File("808jni.dll");
                if (!dll.isFile() && cp != null) {
                    File jar = new File(cp.split(File.pathSeparator)[0]);
                    File dir = jar.getAbsoluteFile().getParentFile();
                    if (dir != null) dll = new File(dir, "808jni.dll");
                }
                System.load(dll.getAbsolutePath());
            }
        }
    }

    private Native808() {
    }

    public static native long create(double sampleRate);

    public static native void destroy(long e);

    public static native void renderS16(long e, byte[] out, int frames);

    public static native void snapshot(long e, int pat, int[] meta, float[] params,
                                       byte[] steps, byte[] flags);

    public static native void status(long e, int[] ints, float[] meters, float[] misc);

    public static native void setStep(long e, int pat, int track, int step, int field, int value);

    public static native void setCond(long e, int pat, int track, int step, int cond, int n);

    public static native void setTrackParam(long e, int track, int param, float value);

    public static native void setMaster(long e, int param, float value);

    public static native void setBpm(long e, float bpm);

    public static native void setPatternLen(long e, int pat, int len);

    public static native void setSwing(long e, int pat, int swing);

    public static native void setTimesig(long e, int num, int den);

    public static native void setClock(long e, int midiClock);

    public static native int clockSource(long e);

    public static native void setTrackLen(long e, int track, int len);

    public static native void selectPattern(long e, int pat);

    public static native void setMute(long e, int track, int mute);

    public static native void setSolo(long e, int track, int solo);

    public static native void play(long e);

    public static native void stop(long e);

    public static native void stopReset(long e);

    public static native void noteOn(long e, int track, int vel);

    public static native void clearPattern(long e, int pat);

    public static native void copyPattern(long e, int src, int dst);

    public static native void setSongOn(long e, int on);

    public static native void setSongLen(long e, int len);

    public static native void setSongEntry(long e, int index, int pat, int reps);

    public static native int songLen(long e);

    public static native int songPat(long e, int index);

    public static native int songReps(long e, int index);

    public static native int save(long e, String path);

    public static native int load(long e, String path);

    public static native int exportWav(long e, String path, int mode, int loops, int stems);

    public static native int exportMidi(long e, String path, int mode, int loops);

    public static native int midiCount();

    public static native String midiName(int index);

    public static native int midiStart(long e, int index);

    public static native void midiStop(long e);

    public static native void boostThread();
}
