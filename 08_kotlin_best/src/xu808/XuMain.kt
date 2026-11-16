// ============================================================================
//  XuMain.kt —— Kotlin 版入口  ·  徐某
//
//  用法：
//      java -jar XU808-Kotlin.jar              图形界面
//      java -jar XU808-Kotlin.jar --selftest   无界面渲染自检
// ============================================================================
package xu808

import java.io.File
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Paths
import javax.swing.SwingUtilities
import javax.swing.UIManager

fun main(args: Array<String>) {
    // 固定 1:1 像素缩放：Windows 的高 DPI 缩放会把整个界面放大，
    // 结果底部母线被挤出屏幕。机架式鼓机就按像素排布，这里显式关掉缩放。
    System.setProperty("sun.java2d.uiScale", "1.0")

    if (args.contains("--selftest")) {
        runSelfTest()
        return
    }
    try {
        // 统一用跨平台（Metal）外观：它的控件会老老实实使用我们指定的深色配色，
        // Windows 系统外观会强制画成浅色按钮，808 的味道就没了。
        UIManager.setLookAndFeel(UIManager.getCrossPlatformLookAndFeelClassName())
        UIManager.put("Button.background", XuUi.PANEL2)
        UIManager.put("Button.foreground", XuUi.TEXT)
        UIManager.put("Panel.background", XuUi.BG)
        UIManager.put("Label.foreground", XuUi.TEXT)
        UIManager.put("Slider.background", XuUi.PANEL)
        UIManager.put("Spinner.background", XuUi.PANEL2)
        UIManager.put("FormattedTextField.background", XuUi.PANEL2)
        UIManager.put("FormattedTextField.foreground", XuUi.TEXT)
        UIManager.put("ComboBox.background", XuUi.PANEL2)
        UIManager.put("ComboBox.foreground", XuUi.TEXT)
        UIManager.put("CheckBox.background", XuUi.PANEL)
        UIManager.put("CheckBox.foreground", XuUi.TEXT)
        UIManager.put("ScrollPane.background", XuUi.BG)
        UIManager.put("Viewport.background", XuUi.BG)
        UIManager.put("OptionPane.background", XuUi.PANEL)
        UIManager.put("OptionPane.messageForeground", XuUi.TEXT)
        UIManager.put("ToolTip.background", XuUi.PANEL2)
        UIManager.put("ToolTip.foreground", XuUi.TEXT)
    } catch (ignored: Exception) {
    }
    val dump = args.contains("--uidump")
    SwingUtilities.invokeLater {
        val ui = XuUi(XuProject())
        ui.isVisible = true
        if (dump) {
            // --uidump：窗口显示 2 秒后把布局尺寸与截图写到 build/，用于验收排版
            javax.swing.Timer(2000) { captureUi(ui) }.apply { isRepeats = false }.start()
        }
    }
}

private fun dumpComponent(c: java.awt.Component, depth: Int, sb: StringBuilder) {
    sb.append("  ".repeat(depth)).append(c.javaClass.simpleName)
            .append(" x=").append(c.x).append(" y=").append(c.y)
            .append(" w=").append(c.width).append(" h=").append(c.height).append('\n')
    if (depth < 3 && c is java.awt.Container)
        for (ch in c.components) dumpComponent(ch, depth + 1, sb)
}

private fun captureUi(ui: XuUi) {
    try {
        val sb = StringBuilder()
        sb.append("frame=").append(ui.bounds).append('\n')
        sb.append("content=").append(ui.contentPane.size).append('\n')
        dumpComponent(ui.contentPane, 0, sb)
        Files.write(Paths.get("build/ui_dump.txt"), sb.toString().toByteArray(StandardCharsets.UTF_8))
        val img = java.awt.Robot().createScreenCapture(ui.bounds)
        javax.imageio.ImageIO.write(img, "png", File("build/ui_frame.png"))
    } catch (ex: Exception) {
        ex.printStackTrace()
    }
    System.exit(0)
}

/** 单轨单独发声测试：返回该轨 1 秒内的峰值（用于自检 16 个声部都能响）。 */
private fun padPeak(proj: XuProject, pad: Int): Float {
    val e = XuEngine(proj)
    e.triggerPad(pad, 110, false)
    val L = FloatArray(XuEngine.BLOCK)
    val R = FloatArray(XuEngine.BLOCK)
    var peak = 0f
    // 1 秒 = 48000 / 512 ≈ 94 块
    for (b in 0 until 94) {
        e.render(XuEngine.BLOCK, L, R)
        for (i in 0 until XuEngine.BLOCK) {
            peak = Math.max(peak, Math.abs(L[i]))
            peak = Math.max(peak, Math.abs(R[i]))
        }
    }
    return peak
}

private fun runSelfTest() {
    var ok = true
    val sb = StringBuilder()

    // 1) 默认工程 + 单 Pattern 离线渲染（走与实时完全相同的 render 路径）
    val proj = XuProject()
    val eng = XuEngine(proj)
    val audio = eng.renderPattern(1)
    val frames = audio.size / 2
    var peak = 0f
    var energy = 0.0
    for (v in audio) {
        peak = Math.max(peak, Math.abs(v))
        energy += v.toDouble() * v
    }
    val rms = Math.sqrt(energy / Math.max(1, audio.size))
    ok = ok && peak > 0.01f && peak <= 1.0f && frames > XuProject.SR

    // 2) WAV（16bit/48k/立体声）
    XuEngine.writeWav("selftest_kotlin.wav", audio)
    val wav = File("selftest_kotlin.wav")
    ok = ok && wav.exists() && wav.length() > 44

    // 3) 16 个声部逐个发声
    var live = 0
    val peaks = StringBuilder()
    for (p in 0 until XuProject.PADS) {
        val pk = padPeak(proj, p)
        if (pk > 0.0005f) live++
        if (p > 0) peaks.append(',')
        peaks.append(String.format("%.3f", pk))
    }
    ok = ok && live == XuProject.PADS

    // 4) 工程 JSON 存/读往返
    var jsonOk = false
    try {
        proj.save("selftest_kotlin.json")
        val np = XuProject.open("selftest_kotlin.json")
        jsonOk = np.patterns.size == proj.patterns.size &&
                np.cur().steps == proj.cur().steps &&
                Math.abs(np.master.bpm - proj.master.bpm) < 1e-6 &&
                np.pads[0].decay == proj.pads[0].decay &&
                np.song.size == proj.song.size
        var sameSteps = true
        for (pad in 0 until XuProject.PADS)
            for (s in 0 until XuProject.MAX_STEPS) {
                val a = proj.patterns[0].data[pad][s]
                val b = np.patterns[0].data[pad][s]
                if (a.on != b.on || a.velocity != b.velocity || a.micro != b.micro ||
                        a.ratchet != b.ratchet || a.cond != b.cond || a.prob != b.prob
                ) sameSteps = false
            }
        jsonOk = jsonOk && sameSteps
    } catch (ex: Exception) {
        sb.append("JSON 异常: ").append(ex.message).append("\n")
    }
    ok = ok && jsonOk

    // 5) MIDI 导出
    var midiBytes = -1L
    try {
        eng.exportMidi("selftest_kotlin.mid", false)
        val mf = File("selftest_kotlin.mid")
        midiBytes = if (mf.exists()) mf.length() else -1
    } catch (ex: Exception) {
        sb.append("MIDI 异常: ").append(ex.message).append("\n")
    }
    ok = ok && midiBytes > 14

    // 6) 写自检报告（UTF-8），最后一行是验收标记
    sb.append("samples=").append(frames).append(" peak=")
            .append(String.format("%.6f", peak)).append(" rms=")
            .append(String.format("%.6f", rms)).append("\r\n")
    sb.append("wav_bytes=").append(wav.length()).append("\r\n")
    sb.append("voices_live=").append(live).append("/").append(XuProject.PADS)
            .append(" peaks=[").append(peaks).append("]\r\n")
    sb.append("json_roundtrip=").append(if (jsonOk) "OK" else "FAIL").append("\r\n")
    sb.append("midi_bytes=").append(midiBytes).append("\r\n")
    sb.append(if (ok) "SELFTEST OK" else "SELFTEST FAIL").append("\r\n")
    Files.write(Paths.get("selftest_kotlin.txt"), sb.toString().toByteArray(StandardCharsets.UTF_8))

    println("XU808 KOTLIN SELFTEST: frames=$frames peak=$peak rms=$rms voices=$live/16 " +
            "json=$jsonOk midi=$midiBytes")
    println(if (ok) "SELFTEST OK" else "SELFTEST FAIL")
    System.exit(if (ok) 0 else 1)
}
