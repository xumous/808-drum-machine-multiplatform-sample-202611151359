package native808;

import javax.sound.sampled.*;
import javax.swing.*;
import java.awt.*;
import java.awt.event.WindowAdapter;
import java.awt.event.WindowEvent;
import java.awt.image.BufferedImage;
import java.io.InputStream;

/**
 * Java 桌面版入口：
 * - SourceDataLine 独立线程以阻塞 write 驱动音频，JNI 渲染 16-bit PCM
 * - Swing 自定义绘制 UI，33ms 轮询快照，与音频线程完全隔离
 */
public class Main {
    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            AudioEngine808 audio = new AudioEngine808();
            long engine = audio.start();
            if (engine == 0) {
                engine = Native808.create(48000.0);
                audio.setFallbackInfo("AUDIO FAILED · SILENT MODE");
            }
            MachinePanel panel = new MachinePanel(engine, audio);
            JFrame frame = new JFrame("808 · JAVA SYNTHETIC DRUM MACHINE");
            frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
            frame.setContentPane(panel);
            frame.setSize(960, 580);
            frame.setMinimumSize(new Dimension(820, 520));
            frame.setLocation(10, 10);
            try (InputStream in = Main.class.getResourceAsStream("/icon.png")) {
                if (in != null) {
                    BufferedImage img = javax.imageio.ImageIO.read(in);
                    if (img != null) frame.setIconImage(img);
                }
            } catch (Exception ignored) {
            }
            final long eng = engine;
            frame.addWindowListener(new WindowAdapter() {
                @Override
                public void windowClosing(WindowEvent e) {
                    panel.shutdown();
                    audio.stop();
                    Native808.destroy(eng);
                    frame.dispose();
                    System.exit(0);
                }
            });
            frame.setVisible(true);
            panel.requestFocusInWindow();
            for (String a : args) {
                if ("--autoplay".equals(a)) {
                    final long e2 = eng;
                    Timer t = new Timer(1200, ev -> Native808.play(e2));
                    t.setRepeats(false);
                    t.start();
                }
            }
        });
    }
}

class AudioEngine808 {
    private SourceDataLine line;
    private Thread thread;
    private volatile boolean running;
    private long engine;
    private String info = "NO AUDIO";
    private boolean ok;

    long start() {
        AudioFormat fmt = new AudioFormat(AudioFormat.Encoding.PCM_SIGNED, 48000f, 16, 2, 4, 48000f, false);
        DataLine.Info infoLine = new DataLine.Info(SourceDataLine.class, fmt);
        try {
            if (!AudioSystem.isLineSupported(infoLine)) {
                fmt = new AudioFormat(44100f, 16, 2, true, false);
                infoLine = new DataLine.Info(SourceDataLine.class, fmt);
            }
            line = (SourceDataLine) AudioSystem.getLine(infoLine);
            line.open(fmt, 8192);
            line.start();
            engine = Native808.create(fmt.getSampleRate());
            if (engine == 0) return 0;
            info = String.format("SourceDataLine %.0f Hz · 2 ch · 16-bit · buffer %d ms",
                    fmt.getSampleRate(), line.getBufferSize() * 1000 /
                            (int) (fmt.getSampleRate() * fmt.getFrameSize()));
            ok = true;
            running = true;
            thread = new Thread(this::loop, "808-audio");
            thread.setDaemon(true);
            thread.setPriority(Thread.MAX_PRIORITY);
            thread.start();
            return engine;
        } catch (Exception ex) {
            info = "AUDIO FAILED: " + ex.getMessage();
            return 0;
        }
    }

    private void loop() {
        Native808.boostThread();
        byte[] buf = new byte[512 * 4];
        while (running) {
            Native808.renderS16(engine, buf, 512);
            line.write(buf, 0, buf.length);
        }
    }

    void stop() {
        running = false;
        if (thread != null) {
            try {
                thread.join(800);
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

    void setFallbackInfo(String s) {
        info = s;
    }

    boolean isRunning() {
        return ok && running;
    }

    String info() {
        return info;
    }
}
