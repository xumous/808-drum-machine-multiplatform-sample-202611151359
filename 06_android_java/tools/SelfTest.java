// ============================================================================
//  SelfTest.java —— 纯 Java 离线自检（不依赖 Android 运行时）
//
//  徐某：APK 只能在真机上跑，但 Model/DSP/导出全部是纯 Java，可以先用 JDK
//  跑一遍，确认「16 声部都出声、WAV/MIDI 文件头正确、工程存取一致」。
//  这不是交付功能，只是构建期的回归测试。
// ============================================================================

import com.xumou.xu808.XuDsp;
import com.xumou.xu808.XuEngine;
import com.xumou.xu808.XuProject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

public final class SelfTest {

    public static void main(String[] args) throws Exception {
        File out = new File(args.length > 0 ? args[0] : "selftest_out");
        out.mkdirs();

        // ---- 1. 单 Pattern 离线渲染 ----
        XuProject proj = new XuProject();
        XuEngine eng = new XuEngine(proj);
        float[] pat = eng.renderPattern();
        peak("renderPattern", pat);
        XuEngine.writeWav(new File(out, "selftest_pattern.wav"), pat);
        eng.exportMidi(new File(out, "selftest_pattern.mid"), false);

        // ---- 2. 整曲 Song 渲染 ----
        float[] song = eng.renderSong();
        peak("renderSong", song);
        XuEngine.writeWav(new File(out, "selftest_song.wav"), song);
        eng.exportMidi(new File(out, "selftest_song.mid"), true);

        // ---- 3. 16 分轨 WAV ----
        eng.exportStems(new File(out, "stems"), false);

        // ---- 4. 逐一检查 16 个声部都能出声 ----
        for (int pad = 0; pad < 16; pad++) {
            XuProject p2 = XuProject.fromMap(proj.toMap());
            XuProject.Pattern pattern = p2.patterns[0];
            for (int d = 0; d < 16; d++) {
                for (int s = 0; s < 64; s++) {
                    pattern.data[d][s].on = false;
                    pattern.data[d][s].velocity = 100;
                    pattern.data[d][s].ratchet = 1;
                    pattern.data[d][s].prob = 100;
                    pattern.data[d][s].cond = XuProject.COND_ALWAYS;
                }
                pattern.trackLen[d] = 16;
            }
            pattern.steps = 16;
            pattern.data[pad][0].on = true;
            pattern.data[pad][0].velocity = 120;
            XuEngine e2 = new XuEngine(p2);
            float[] one = e2.renderPattern();
            double pk = peakValue(one);
            if (!(pk > 0.01)) throw new IllegalStateException(
                    "声部 " + XuProject.CODES[pad] + " 没有声音，peak=" + pk);
            System.out.printf("  pad %02d %-3s peak=%.4f%n", pad, XuProject.CODES[pad], pk);
        }

        // ---- 5. CHOKE：闭镲必须掐断开镲 ----
        {
            XuProject p3 = XuProject.fromMap(proj.toMap());
            XuProject.Pattern pattern = p3.patterns[0];
            for (int d = 0; d < 16; d++)
                for (int s = 0; s < 64; s++) pattern.data[d][s].on = false;
            pattern.data[14][0].on = true;   // OH 开镲
            pattern.data[15][1].on = true;   // 下一 16 分闭镲 → 掐断
            XuEngine e3 = new XuEngine(p3);
            float[] b = e3.renderPattern();
            peak("choke(OH→CH)", b);
        }

        // ---- 6. 工程保存 / 载入一致性 ----
        File json = new File(out, "selftest_project.json");
        proj.master.bpm = 137f;
        proj.master.swing = 0.42f;
        proj.cur().data[0][3].velocity = 77;
        proj.cur().data[0][3].micro = -120;
        proj.cur().data[0][3].ratchet = 4;
        proj.cur().data[0][3].cond = XuProject.COND_EVERY2;
        proj.save(json);
        XuProject back = XuProject.load(json);
        if (Math.abs(back.master.bpm - 137f) > 1e-4) throw new IllegalStateException("BPM 还原失败");
        if (Math.abs(back.master.swing - 0.42f) > 1e-4) throw new IllegalStateException("Swing 还原失败");
        if (back.cur().data[0][3].velocity != 77) throw new IllegalStateException("力度还原失败");
        if (back.cur().data[0][3].micro != -120) throw new IllegalStateException("微时还原失败");
        if (back.cur().data[0][3].ratchet != 4) throw new IllegalStateException("Ratchet 还原失败");
        if (back.cur().data[0][3].cond != XuProject.COND_EVERY2) throw new IllegalStateException("条件还原失败");

        // ---- 7. 文件头检查 ----
        checkMagic(new File(out, "selftest_pattern.wav"), "RIFF");
        checkMagic(new File(out, "selftest_pattern.mid"), "MThd");
        checkMagic(new File(out, "stems/stem_00_BD.wav"), "RIFF");
        if (new File(out, "selftest_song.wav").length() < 10000)
            throw new IllegalStateException("song WAV 太小");

        System.out.println("SELFTEST OK");
    }

    private static void peak(String name, float[] buf) {
        double p = peakValue(buf);
        if (!(p > 0.01)) throw new IllegalStateException(name + " 输出静音");
        System.out.printf("%s: frames=%d peak=%.4f%n", name, buf.length / 2, p);
    }

    private static double peakValue(float[] buf) {
        double p = 0;
        for (float v : buf) {
            if (Float.isNaN(v) || Float.isInfinite(v))
                throw new IllegalStateException("出现 NaN/Inf");
            p = Math.max(p, Math.abs(v));
        }
        return p;
    }

    private static void checkMagic(File f, String magic) throws IOException {
        if (!f.exists() || f.length() < 12) throw new IllegalStateException("文件缺失: " + f);
        FileInputStream in = null;
        try {
            in = new FileInputStream(f);
            byte[] b = new byte[4];
            int n = in.read(b);
            String s = new String(b, 0, n, java.nio.charset.StandardCharsets.US_ASCII);
            if (!magic.equals(s)) throw new IllegalStateException(f + " 文件头错误: " + s);
        } finally {
            if (in != null) try {
                in.close();
            } catch (IOException ignored) {
            }
        }
        System.out.println("  ok " + f.getName() + " " + f.length() + " bytes");
    }
}
