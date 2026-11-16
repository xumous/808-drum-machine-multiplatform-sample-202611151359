// ============================================================================
//  XuAudio.java —— AudioTrack 独立音频线程（Android 版）
//
//  徐某说明：
//    · 规格书要求音频回调里只能做 DSP：本类把 AudioTrack 建在一条专用线程里，
//      用 MODE_STREAM + 阻塞写，绝不占用 UI 线程；
//    · 每块 512 样本（约 10.7ms），音序在 engine.render() 内按样本推进；
//    · 优先使用 ENCODING_PCM_FLOAT（float32 直通，与规格书内部格式一致），
//      老设备不支持时自动回落到 16bit PCM；
//    · 退出时先停线程、再 flush/release，保证没有残留音频进程。
// ============================================================================
package com.xumou.xu808;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Process;

public final class XuAudio {
    private final XuEngine engine;
    // 预分配缓冲：音频线程内不再申请堆内存
    private final float[] L = new float[XuEngine.BLOCK];
    private final float[] R = new float[XuEngine.BLOCK];
    private final float[] mix = new float[XuEngine.BLOCK * 2];
    private final short[] mix16 = new short[XuEngine.BLOCK * 2];
    private AudioTrack track;
    private Thread thread;
    private volatile boolean running = false;
    private volatile boolean floatMode = true;
    private volatile String lastError = "";

    public XuAudio(XuEngine e) {
        engine = e;
    }

    public String getLastError() {
        return lastError;
    }

    public boolean isRunning() {
        return running;
    }

    /**
     * 打开音频设备并启动渲染线程。
     * 返回值表示是否成功；失败原因放在 lastError 里供 UI 提示。
     */
    public boolean start() {
        if (running) return true;

        // 先试 float32；部分低端机 HAL 不支持，再退回 16bit
        if (!open(true)) {
            closeTrack();
            if (!open(false)) {
                closeTrack();
                return false;
            }
        }
        try {
            track.play();
        } catch (Throwable t) {
            lastError = "AudioTrack.play(): " + t.getMessage();
            closeTrack();
            return false;
        }

        running = true;
        // 徐某：用匿名 Runnable，不用方法引用（android.jar 引导路径下无 LambdaMetafactory）
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                loop();
            }
        }, "XU808-AudioTrack");
        thread.setDaemon(true);
        thread.start();
        return true;
    }

    /**
     * 按指定编码创建 AudioTrack；失败返回 false。
     */
    private boolean open(boolean useFloat) {
        try {
            int enc = useFloat ? AudioFormat.ENCODING_PCM_FLOAT : AudioFormat.ENCODING_PCM_16BIT;
            int minBytes = AudioTrack.getMinBufferSize(
                    XuProject.SR,
                    AudioFormat.CHANNEL_OUT_STEREO,
                    enc);
            if (minBytes <= 0) {
                lastError = "getMinBufferSize=" + minBytes;
                return false;
            }
            // 徐某：用两倍最小缓冲，兼顾低延迟与抗 xrun；最低不少于 4 个块
            int blockBytes = XuEngine.BLOCK * 2 * (useFloat ? 4 : 2);
            int bufBytes = Math.max(minBytes * 2, blockBytes * 4);

            AudioAttributes attrs = new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build();
            AudioFormat fmt = new AudioFormat.Builder()
                    .setEncoding(enc)
                    .setSampleRate(XuProject.SR)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build();

            track = new AudioTrack.Builder()
                    .setAudioAttributes(attrs)
                    .setAudioFormat(fmt)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufBytes)
                    .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                    .build();
            if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                lastError = "AudioTrack 未初始化";
                return false;
            }
            // 音乐流音量跟随系统媒体音量
            try {
                track.setVolume(1.0f);
            } catch (Throwable ignored) {
            }
            floatMode = useFloat;
            return true;
        } catch (Throwable t) {
            lastError = (useFloat ? "float32" : "int16") + " 打开失败: " + t.getMessage();
            return false;
        }
    }

    /**
     * 音频线程主循环：渲染 → 写设备；不做任何 IO/锁/UI 调用。
     */
    private void loop() {
        // 尽可能贴近实时优先级（失败也不影响功能）
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        } catch (Throwable ignored) {
        }

        try {
            while (running) {
                long t0 = System.nanoTime();
                engine.render(XuEngine.BLOCK, L, R);

                int written;
                if (floatMode) {
                    // 交织成 LRLR…，Float 直写，省去一次量化
                    for (int i = 0; i < XuEngine.BLOCK; i++) {
                        mix[i * 2] = L[i];
                        mix[i * 2 + 1] = R[i];
                    }
                    written = track.write(mix, 0, mix.length, AudioTrack.WRITE_BLOCKING);
                } else {
                    for (int i = 0; i < XuEngine.BLOCK; i++) {
                        int l = (int) (Math.max(-1f, Math.min(1f, L[i])) * 32767f);
                        int r = (int) (Math.max(-1f, Math.min(1f, R[i])) * 32767f);
                        mix16[i * 2] = (short) l;
                        mix16[i * 2 + 1] = (short) r;
                    }
                    written = track.write(mix16, 0, mix16.length, AudioTrack.WRITE_BLOCKING);
                }
                if (written < 0) engine.xruns++;

                // CPU 占用（相对理想块时长），UI 只读这个 volatile 值
                long dt = System.nanoTime() - t0;
                double ideal = XuEngine.BLOCK / 48000.0;
                engine.cpu = engine.cpu * 0.95f + (float) ((dt / 1e9) / ideal) * 0.05f;
            }
        } catch (Throwable t) {
            // 设备被抢占/拔出时 write 会抛异常：安静地停下，不拖垮整个进程
            lastError = "音频线程退出: " + t.getMessage();
            running = false;
        }
    }

    /**
     * 安全停止：先让循环退出，再释放设备。
     */
    public void stop() {
        running = false;
        Thread t = thread;
        thread = null;
        if (t != null) {
            try {
                t.join(800);
            } catch (InterruptedException ignored) {
            }
        }
        closeTrack();
    }

    private void closeTrack() {
        AudioTrack a = track;
        track = null;
        if (a != null) {
            try {
                a.pause();
            } catch (Throwable ignored) {
            }
            try {
                a.flush();
            } catch (Throwable ignored) {
            }
            try {
                a.stop();
            } catch (Throwable ignored) {
            }
            try {
                a.release();
            } catch (Throwable ignored) {
            }
        }
    }
}
