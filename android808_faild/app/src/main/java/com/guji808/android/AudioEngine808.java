package com.guji808.android;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Process;
import android.util.Log;

/**
 * 专用高优先级音频线程：
 * - AudioTrack（优先 ENCODING_PCM_FLOAT，失败回退 PCM16）48 kHz 立体声
 * - 优先 PERFORMANCE_MODE_LOW_LATENCY，失败回退普通模式
 * - 阻塞写驱动，每次渲染 256 帧；所有 DSP 在 core808 的 c808_render 中完成
 * - 所有 AudioTrack 组合都失败时进入“静音泵”模式：仍然在音频线程渲染，
 * 保证命令队列持续推进、界面可编辑，只是没有声音输出。
 * - UI 线程只通过无锁命令队列与引擎交互。
 */
public final class AudioEngine808 {
    public static final int SAMPLE_RATE = 48000;
    private static final String TAG = "Android808-Audio";
    private static final int BLOCK_FRAMES = 256;

    private final long engine;
    private AudioTrack track;
    private Thread thread;
    private volatile boolean running;
    private volatile boolean floatMode;
    private volatile boolean silentMode;
    private volatile String info = "NO AUDIO";

    public AudioEngine808(long engine) {
        this.engine = engine;
    }

    public synchronized boolean start() {
        if (running) return true;
        if (engine == 0) {
            info = "NO AUDIO · ENGINE NULL";
            return false;
        }
        silentMode = false;
        boolean opened = openTrack(true, true)
                || openTrack(false, true)
                || openTrack(true, false)
                || openTrack(false, false);
        if (!opened) {
            silentMode = true;
            track = null;
            info = "NO AUDIO · SILENT PUMP (engine thread active)";
        }
        running = true;
        thread = new Thread(this::loop, "808-audio");
        thread.setDaemon(true);
        thread.start();
        return true;
    }

    private boolean openTrack(boolean useFloat, boolean lowLatency) {
        int encoding = useFloat ? AudioFormat.ENCODING_PCM_FLOAT : AudioFormat.ENCODING_PCM_16BIT;
        int bytes = useFloat ? 4 : 2;
        AudioTrack t = null;
        try {
            int min = AudioTrack.getMinBufferSize(SAMPLE_RATE,
                    AudioFormat.CHANNEL_OUT_STEREO, encoding);
            if (min <= 0) return false;
            int bufferBytes = Math.max(min * 2, BLOCK_FRAMES * 2 * bytes * 4);

            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            AudioFormat format = new AudioFormat.Builder()
                    .setEncoding(encoding)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build();
            AudioTrack.Builder builder = new AudioTrack.Builder()
                    .setAudioAttributes(attrs)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bufferBytes)
                    .setTransferMode(AudioTrack.MODE_STREAM);
            if (lowLatency) {
                builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
            }
            t = builder.build();
            if (t.getState() != AudioTrack.STATE_INITIALIZED) {
                t.release();
                return false;
            }
            t.play();
            track = t;
            floatMode = useFloat;
            info = String.format("AudioTrack 48k · 2ch · %s · %d f%s",
                    useFloat ? "float32" : "pcm16", BLOCK_FRAMES,
                    lowLatency ? " · low-latency" : "");
            return true;
        } catch (Exception ex) {
            Log.w(TAG, "openTrack(float=" + useFloat + ", ll=" + lowLatency + ") failed: "
                    + ex.getMessage());
            if (t != null) {
                try {
                    t.release();
                } catch (Exception ignored) {
                }
            }
            return false;
        }
    }

    private void loop() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        if (silentMode) {
            byte[] buf = new byte[BLOCK_FRAMES * 4];
            while (running) {
                Native808.renderS16(engine, buf, BLOCK_FRAMES);
                try {
                    Thread.sleep(4);
                } catch (InterruptedException e) {
                    break;
                }
            }
            running = false;
            return;
        }
        final AudioTrack t = track;
        if (t == null) {
            running = false;
            return;
        }
        if (floatMode) {
            float[] buf = new float[BLOCK_FRAMES * 2];
            while (running) {
                Native808.renderF32(engine, buf, BLOCK_FRAMES);
                int w = t.write(buf, 0, buf.length, AudioTrack.WRITE_BLOCKING);
                if (w < 0) {
                    Log.w(TAG, "AudioTrack.write(float) = " + w);
                    break;
                }
            }
        } else {
            byte[] buf = new byte[BLOCK_FRAMES * 4];
            while (running) {
                Native808.renderS16(engine, buf, BLOCK_FRAMES);
                int w = t.write(buf, 0, buf.length, AudioTrack.WRITE_BLOCKING);
                if (w < 0) {
                    Log.w(TAG, "AudioTrack.write(pcm16) = " + w);
                    break;
                }
            }
        }
        running = false;
    }

    public synchronized void stop() {
        if (!running && thread == null) {
            releaseTrack();
            return;
        }
        running = false;
        AudioTrack t = track;
        if (t != null) {
            try {
                t.pause();
                t.flush();
            } catch (Exception ignored) {
            }
        }
        Thread th = thread;
        thread = null;
        if (th != null && th != Thread.currentThread()) {
            try {
                th.join(1200);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
        releaseTrack();
    }

    private void releaseTrack() {
        AudioTrack t = track;
        track = null;
        if (t != null) {
            try {
                t.stop();
            } catch (Exception ignored) {
            }
            try {
                t.release();
            } catch (Exception ignored) {
            }
        }
    }

    public boolean isRunning() {
        return running;
    }

    public String info() {
        return info;
    }
}
