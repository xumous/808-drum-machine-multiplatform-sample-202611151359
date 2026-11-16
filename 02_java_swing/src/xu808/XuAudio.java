// ============================================================================
//  XuAudio.java —— 独立音频线程（Java Sound）
//
//  规格书要求：音频播放绝不能在 Swing 的事件线程（EDT）里做。
//  徐某因此起了一条最高优先级的专用线程：
//      render() → 转 int16 → SourceDataLine.write()
//  线程里不碰任何 Swing 组件，也不做文件/网络 IO。
//  Java Sound 的延迟约 30–100ms，这是 JVM 音频栈的固有代价；
//  对鼓机来说手感略“肉”，但功能与采样级音序仍然完整。
// ============================================================================
package xu808;

import javax.sound.sampled.*;

public final class XuAudio {
    private final XuEngine engine;
    private final byte[] bytes = new byte[XuEngine.BLOCK * 4];
    private final float[] L = new float[XuEngine.BLOCK];
    private final float[] R = new float[XuEngine.BLOCK];
    private SourceDataLine line;
    private Thread thread;
    private volatile boolean running = false;

    public XuAudio(XuEngine e) {
        engine = e;
    }

    public boolean start() {
        try {
            AudioFormat fmt = new AudioFormat(48000f, 16, 2, true, false);
            DataLine.Info info = new DataLine.Info(SourceDataLine.class, fmt);
            line = (SourceDataLine) AudioSystem.getLine(info);
            line.open(fmt, 8192);      // 约 2048 帧 ≈ 43ms 硬件缓冲
            line.start();
        } catch (Exception ex) {
            line = null;
            return false;
        }
        running = true;
        thread = new Thread(this::loop, "XU808-Audio");
        thread.setPriority(Thread.MAX_PRIORITY);   // 尽可能接近实时
        thread.setDaemon(true);
        thread.start();
        return true;
    }

    private void loop() {
        while (running) {
            long t0 = System.nanoTime();
            engine.render(XuEngine.BLOCK, L, R);
            int p = 0;
            for (int i = 0; i < XuEngine.BLOCK; i++) {
                int l = (int) (Math.max(-1f, Math.min(1f, L[i])) * 32767f);
                int r = (int) (Math.max(-1f, Math.min(1f, R[i])) * 32767f);
                bytes[p++] = (byte) (l & 0xFF);
                bytes[p++] = (byte) ((l >> 8) & 0xFF);
                bytes[p++] = (byte) (r & 0xFF);
                bytes[p++] = (byte) ((r >> 8) & 0xFF);
            }
            line.write(bytes, 0, p);
            long dt = System.nanoTime() - t0;
            double ideal = XuEngine.BLOCK / 48000.0;
            engine.cpu = engine.cpu * 0.95f + (float) ((dt / 1e9) / ideal) * 0.05f;
        }
    }

    public void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(500);
            } catch (InterruptedException ignored) {
            }
            thread = null;
        }
        if (line != null) {
            try {
                line.stop();
                line.flush();
                line.close();
            } catch (Exception ignored) {
            }
            line = null;
        }
    }
}
