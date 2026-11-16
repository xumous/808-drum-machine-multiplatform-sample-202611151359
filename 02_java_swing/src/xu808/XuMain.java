// ============================================================================
//  XuMain.java —— Java 版入口
//
//  用法：
//      java -jar XU808-Java.jar              图形界面
//      java -jar XU808-Java.jar --selftest   无界面渲染自检
// ============================================================================
package xu808;

import javax.swing.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

public final class XuMain {
    public static void main(String[] args) {
        boolean selftest = false;
        for (String a : args) if ("--selftest".equals(a)) selftest = true;

        if (selftest) {
            XuProject p = new XuProject();
            XuEngine e = new XuEngine(p);
            float[] audio = e.renderPattern(1);
            int frames = audio.length / 2;
            float peak = 0;
            double energy = 0;
            for (float v : audio) {
                peak = Math.max(peak, Math.abs(v));
                energy += v * v;
            }
            double rms = Math.sqrt(energy / Math.max(1, audio.length));
            boolean ok = peak > 0.01f && peak <= 1.0f && frames > XuProject.SR;
            try {
                XuEngine.writeWav("selftest_java.wav", audio);
                String text = "samples=" + frames + " peak=" + peak + " rms=" + rms + "\r\n"
                        + (ok ? "SELFTEST OK" : "SELFTEST FAIL") + "\r\n";
                Files.write(Paths.get("selftest_java.txt"), text.getBytes(StandardCharsets.UTF_8));
            } catch (IOException ex) {
                ex.printStackTrace();
                System.exit(2);
            }
            System.out.println("XU808 JAVA SELFTEST: frames=" + frames + " peak=" + peak + " rms=" + rms);
            System.exit(ok ? 0 : 1);
        }

        SwingUtilities.invokeLater(() -> {
            try {
                UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
            } catch (Exception ignored) {
            }
            XuUi ui = new XuUi(new XuProject());
            try {
                ImageIcon icon = new ImageIcon("app.ico");
                if (icon.getIconWidth() > 0) ui.setIconImage(icon.getImage());
            } catch (Exception ignored) {
            }
            ui.setVisible(true);
        });
    }
}
