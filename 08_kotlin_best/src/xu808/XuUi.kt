// ============================================================================
//  XuUi.kt —— Swing 界面（Kotlin 版）  ·  徐某
//
//  界面尽量贴近硬件 808：
//    顶部走转条、16×N 步进网格、16 条通道条、主效果与电平表。
//  Swing 的 EDT 只负责画界面；音频在 XuAudio 的独立线程里跑。
//  所有定时器都只刷新 UI，绝不参与音序——时钟永远在音频线程里数样本。
// ============================================================================
package xu808

import java.awt.BorderLayout
import java.awt.Color
import java.awt.Component
import java.awt.Cursor
import java.awt.Dimension
import java.awt.FlowLayout
import java.awt.Font
import java.awt.Graphics
import java.awt.Graphics2D
import java.awt.GridLayout
import java.awt.event.KeyAdapter
import java.awt.event.KeyEvent
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.awt.event.WindowAdapter
import java.awt.event.WindowEvent
import java.io.File
import java.util.LinkedHashMap
import java.util.concurrent.ConcurrentLinkedQueue
import javax.sound.midi.MidiDevice
import javax.sound.midi.MidiMessage
import javax.sound.midi.MidiSystem
import javax.sound.midi.Receiver
import javax.sound.midi.Transmitter
import javax.swing.BorderFactory
import javax.swing.Box
import javax.swing.BoxLayout
import javax.swing.JButton
import javax.swing.JCheckBox
import javax.swing.JComboBox
import javax.swing.JFileChooser
import javax.swing.JFrame
import javax.swing.JLabel
import javax.swing.JOptionPane
import javax.swing.JPanel
import javax.swing.JScrollPane
import javax.swing.JSlider
import javax.swing.JSpinner
import javax.swing.JTextArea
import javax.swing.JToggleButton
import javax.swing.SpinnerNumberModel
import javax.swing.SwingUtilities
import javax.swing.Timer
import javax.swing.UIManager
import javax.swing.border.TitledBorder

class XuUi(private val proj: XuProject) : JFrame("XU-808 鼓机 - 徐某") {

    companion object {
        // 规格书第 9 节的配色，所有平台版本一致
        val BG = Color(18, 18, 18)
        val PANEL = Color(28, 28, 28)
        val PANEL2 = Color(35, 35, 35)
        val LINE = Color(46, 46, 46)
        val ORANGE = Color(255, 122, 24)
        val YELLOW = Color(255, 209, 102)
        val CYAN = Color(76, 201, 240)
        val TEXT = Color(232, 232, 232)
        val DIM = Color(138, 138, 138)
        val RED = Color(229, 72, 77)

        fun ensureExt(path: String, ext: String): String =
                if (path.lowercase().endsWith(ext)) path else path + ext
    }

    private val eng = XuEngine(proj)
    private var audio: XuAudio? = null

    private lateinit var grid: GridPanel
    private val strips = ArrayList<Strip>()
    private lateinit var meter: MeterPanel
    private lateinit var lbStatus: JLabel
    private lateinit var lbMidi: JLabel
    private lateinit var cbPattern: JComboBox<String>
    private lateinit var spBpm: JSpinner
    private lateinit var spSteps: JSpinner
    private lateinit var slBpm: JSlider
    private lateinit var slSwing: JSlider
    private lateinit var lbSwing: JLabel
    private var timer: Timer? = null
    private var audioOk = true

    // MIDI 接收线程 → EDT 的无锁队列
    private val midiQ = ConcurrentLinkedQueue<IntArray>()
    private var midiDev: MidiDevice? = null
    private var midiTransmitter: Transmitter? = null

    // Tap Tempo：最近 8 次点击
    private val taps = ArrayList<Long>()

    init {
        defaultCloseOperation = EXIT_ON_CLOSE
        // 按屏幕工作区自适应：小屏幕上也要保证母线/电平表能看见
        val wa = java.awt.GraphicsEnvironment.getLocalGraphicsEnvironment().maximumWindowBounds
        val w = Math.min(1520, wa.width - 16)
        val h = Math.min(940, wa.height - 24)
        size = Dimension(w, h)
        minimumSize = Dimension(980, 620)
        setLocation(wa.x + (wa.width - w) / 2, wa.y + (wa.height - h) / 2)
        contentPane.background = BG
        layout = BorderLayout()
        buildUi()
        loadIcon()
        startAudio()
        startMidi()

        // 仅刷新 UI，约 30fps
        timer = Timer(33) { onTick() }
        timer?.start()

        addWindowListener(object : WindowAdapter() {
            override fun windowClosing(e: WindowEvent?) {
                // 关窗安全退出：先停音序、再停音频线程与 MIDI
                eng.stop()
                audio?.stop()
                stopMidi()
                timer?.stop()
            }
        })

        isFocusable = true
        addKeyListener(object : KeyAdapter() {
            override fun keyPressed(e: KeyEvent) {
                if (e.keyCode == KeyEvent.VK_SPACE) {
                    eng.toggle()
                    return
                }
                if (e.keyCode in KeyEvent.VK_1..KeyEvent.VK_8) {
                    cbPattern.selectedIndex = e.keyCode - KeyEvent.VK_1
                    return
                }
                // 电脑键盘当鼓垫用：ZXCVBNMASDFGHJKL → 16 声部
                val keys = "ZXCVBNMASDFGHJKL"
                val idx = keys.indexOf(e.keyChar.uppercaseChar())
                if (idx >= 0) eng.triggerPad(idx, 110, false)
            }
        })
    }

    private fun loadIcon() {
        try {
            val stream = javaClass.getResourceAsStream("/app.png")
            if (stream != null) {
                val img = javax.imageio.ImageIO.read(stream)
                if (img != null) iconImage = img
            }
        } catch (ignored: Exception) {
        }
    }

    // ==================================================================
    // 构建界面
    // ==================================================================
    private fun buildUi() {
        add(buildTop(), BorderLayout.NORTH)

        // 上部：步进网格（固定高度、横向可滚动）
        grid = GridPanel()
        grid.rebuild()
        val gs = JScrollPane(
                grid,
                JScrollPane.VERTICAL_SCROLLBAR_NEVER,
                JScrollPane.HORIZONTAL_SCROLLBAR_AS_NEEDED
        )
        gs.preferredSize = Dimension(1500, 328)
        gs.border = null
        gs.background = BG
        gs.viewport.background = BG
        gs.horizontalScrollBar.unitIncrement = 24

        // 中部：16 条通道条（同样横向滚动）
        val stripHolder = JPanel(FlowLayout(FlowLayout.LEFT, 2, 2))
        stripHolder.background = BG
        for (i in 0 until XuProject.PADS) {
            val s = Strip(i)
            strips.add(s)
            stripHolder.add(s)
        }
        val ss = JScrollPane(
                stripHolder,
                JScrollPane.VERTICAL_SCROLLBAR_NEVER,
                JScrollPane.HORIZONTAL_SCROLLBAR_AS_NEEDED
        )
        ss.preferredSize = Dimension(1500, 300)
        ss.border = null
        ss.background = BG
        ss.viewport.background = BG
        ss.horizontalScrollBar.unitIncrement = 24

        // 网格 + 通道条放进同一个纵向滚动容器：
        // 屏幕再矮也不会把控件裁掉，只会出现滚动条（母线永远固定在底部）。
        val main = JPanel(BorderLayout())
        main.background = BG
        main.add(gs, BorderLayout.NORTH)
        main.add(ss, BorderLayout.CENTER)

        val outer = JScrollPane(
                main,
                JScrollPane.VERTICAL_SCROLLBAR_AS_NEEDED,
                JScrollPane.HORIZONTAL_SCROLLBAR_NEVER
        )
        outer.border = null
        outer.background = BG
        outer.viewport.background = BG
        outer.verticalScrollBar.unitIncrement = 24
        add(outer, BorderLayout.CENTER)

        // 底部：主效果 / 电平表，固定不滚动
        add(buildMaster(), BorderLayout.SOUTH)
    }

    private fun buildTop(): JPanel {
        val top = JPanel()
        top.background = PANEL
        top.layout = BoxLayout(top, BoxLayout.Y_AXIS)

        // ---- 第一行：走带 / BPM / Swing ----
        val row1 = JPanel(FlowLayout(FlowLayout.LEFT, 6, 2))
        row1.background = PANEL

        val logoBox = JPanel()
        logoBox.isOpaque = false
        logoBox.layout = BoxLayout(logoBox, BoxLayout.Y_AXIS)
        logoBox.preferredSize = Dimension(160, 34)
        logoBox.maximumSize = Dimension(160, 34)
        val logo = JLabel("XU-808")
        logo.foreground = ORANGE
        logo.font = Font("Consolas", Font.BOLD, 19)
        val sub = JLabel("RHYTHM COMPOSER · 徐某 · 16 VOICE")
        sub.foreground = DIM
        sub.font = Font("Microsoft YaHei", Font.PLAIN, 7)
        logoBox.add(logo)
        logoBox.add(sub)
        row1.add(logoBox)

        val bPlay = mkButton("▶ PLAY", 82)
        val bStop = mkButton("■ STOP", 82)
        bPlay.addActionListener { eng.play() }
        bStop.addActionListener {
            eng.stop()
            grid.setPlayhead(-1)
        }
        row1.add(bPlay)
        row1.add(bStop)

        row1.add(mkLabel("BPM", DIM))
        spBpm = JSpinner(SpinnerNumberModel(proj.master.bpm, 40.0, 300.0, 1.0))
        spBpm.preferredSize = Dimension(66, 28)
        row1.add(spBpm)
        slBpm = JSlider(40, 300, proj.master.bpm.toInt())
        slBpm.preferredSize = Dimension(88, 28)
        slBpm.background = PANEL
        row1.add(slBpm)
        spBpm.addChangeListener {
            val d = (spBpm.value as? Number)?.toDouble() ?: 120.0
            proj.master.bpm = d
            slBpm.value = d.toInt()
        }
        slBpm.addChangeListener { spBpm.value = slBpm.value.toDouble() }

        val bTap = mkButton("TAP", 46)
        bTap.addActionListener { onTap() }
        row1.add(bTap)

        row1.add(mkLabel("SWING", DIM))
        slSwing = JSlider(0, 75, (proj.master.swing * 100).toInt())
        slSwing.preferredSize = Dimension(88, 28)
        slSwing.background = PANEL
        lbSwing = mkLabel("${slSwing.value}%", ORANGE)
        slSwing.addChangeListener {
            proj.master.swing = slSwing.value / 100.0
            lbSwing.text = "${slSwing.value}%"
        }
        row1.add(slSwing)
        row1.add(lbSwing)

        // ---- 第二行：Pattern / 步数 / Song / 工程文件 ----
        val row2 = JPanel(FlowLayout(FlowLayout.LEFT, 6, 2))
        row2.background = PANEL

        row2.add(mkLabel("PATTERN", DIM))
        cbPattern = JComboBox()
        for (i in 0 until XuProject.PATTERNS)
            cbPattern.addItem(String.format("%02d %s", i + 1, proj.patterns[i].name))
        cbPattern.selectedIndex = proj.current
        cbPattern.preferredSize = Dimension(140, 28)
        cbPattern.addActionListener {
            eng.requestPattern(cbPattern.selectedIndex)
            spSteps.value = proj.cur().steps
            grid.rebuild()
            syncStrips()
        }
        row2.add(cbPattern)

        spSteps = JSpinner(SpinnerNumberModel(proj.cur().steps, 1, 64, 1))
        spSteps.preferredSize = Dimension(56, 28)
        spSteps.addChangeListener {
            proj.cur().steps = (spSteps.value as Number).toInt()
            grid.rebuild()
        }
        row2.add(spSteps)

        val cbSong = JCheckBox("SONG")
        cbSong.foreground = TEXT
        cbSong.isOpaque = false
        cbSong.isSelected = proj.master.songMode
        cbSong.addActionListener { proj.master.songMode = cbSong.isSelected }
        row2.add(cbSong)

        val bSong = mkButton("歌曲链…", 80)
        bSong.addActionListener { editSong() }
        row2.add(bSong)

        val bSave = mkButton("保存", 62)
        val bLoad = mkButton("打开", 62)
        bSave.addActionListener { saveProject() }
        bLoad.addActionListener { loadProject() }
        row2.add(bSave)
        row2.add(bLoad)

        // ---- 第三行：导出 / 设置 / 状态 ----
        val row3 = JPanel(FlowLayout(FlowLayout.LEFT, 6, 2))
        row3.background = PANEL

        val bWav = mkButton("导出 WAV", 86)
        val bMidi = mkButton("导出 MIDI", 94)
        val bStems = mkButton("分轨 WAV", 86)
        val bSet = mkButton("设置…", 68)
        bWav.addActionListener { exportWav() }
        bMidi.addActionListener { exportMidi() }
        bStems.addActionListener { exportStems() }
        bSet.addActionListener { editSettings() }
        row3.add(bWav)
        row3.add(bMidi)
        row3.add(bStems)
        row3.add(bSet)

        lbMidi = mkLabel("MIDI: -", DIM)
        row3.add(lbMidi)
        lbStatus = mkLabel("CPU 0% · XRUN 0", DIM)
        row3.add(lbStatus)

        top.add(row1)
        top.add(row2)
        top.add(row3)
        return top
    }

    private fun buildMaster(): JPanel {
        val mg = JPanel(FlowLayout(FlowLayout.LEFT, 10, 4))
        mg.background = PANEL
        mg.preferredSize = Dimension(0, 90)
        mg.border = BorderFactory.createTitledBorder(
                BorderFactory.createLineBorder(LINE), "MASTER · 徐某的母线",
                TitledBorder.LEFT, TitledBorder.TOP,
                Font("Microsoft YaHei", Font.PLAIN, 10), ORANGE
        )

        addMaster(mg, "LP 低通", 200, 20000, proj.master.lpCutoff.toInt()) {
            proj.master.lpCutoff = it.toDouble()
        }
        addMaster(mg, "RES 共鸣", 50, 800, (proj.master.lpQ * 100).toInt()) {
            proj.master.lpQ = it / 100.0
        }
        addMaster(mg, "HP 高通", 10, 800, proj.master.hpCutoff.toInt()) {
            proj.master.hpCutoff = it.toDouble()
        }
        addMaster(mg, "DRIVE", 100, 800, (proj.master.drive * 100).toInt()) {
            proj.master.drive = it / 100.0
        }
        addMaster(mg, "VOL", 0, 100, (proj.master.volume * 100).toInt()) {
            proj.master.volume = it / 100.0
        }

        meter = MeterPanel()
        mg.add(meter)

        val cbClock = JCheckBox("MIDI CLK")
        cbClock.isOpaque = false
        cbClock.foreground = TEXT
        cbClock.addActionListener { eng.clockExternal = cbClock.isSelected }
        mg.add(cbClock)
        return mg
    }

    private fun addMaster(host: JPanel, text: String, lo: Int, hi: Int, initial: Int, setter: (Int) -> Unit) {
        val box = JPanel()
        box.isOpaque = false
        box.layout = BoxLayout(box, BoxLayout.Y_AXIS)
        box.preferredSize = Dimension(128, 64)
        box.maximumSize = Dimension(128, 64)

        val l = mkLabel(text, DIM)
        l.font = Font("Microsoft YaHei", Font.PLAIN, 9)
        val s = JSlider(lo, hi, initial.coerceIn(lo, hi))
        s.background = PANEL
        s.preferredSize = Dimension(124, 24)
        s.maximumSize = Dimension(124, 24)
        val v = mkLabel(initial.toString(), ORANGE)
        v.font = Font("Microsoft YaHei", Font.PLAIN, 9)
        s.addChangeListener {
            setter(s.value)
            v.text = s.value.toString()
        }
        box.add(l)
        box.add(s)
        box.add(v)
        host.add(box)
    }

    private fun mkButton(text: String, w: Int): JButton {
        val b = JButton(text)
        b.preferredSize = Dimension(w, 30)
        b.background = PANEL2
        b.foreground = TEXT
        b.isFocusPainted = false
        b.border = BorderFactory.createLineBorder(LINE)
        return b
    }

    private fun mkLabel(text: String, c: Color): JLabel {
        val l = JLabel(text)
        l.foreground = c
        return l
    }

    private fun syncStrips() {
        for (s in strips) s.sync()
    }

    // ==================================================================
    // 运行期刷新（EDT，只读快照）
    // ==================================================================
    private fun onTick() {
        grid.setPlayhead(if (eng.playing) eng.stepIndex else -1)
        meter.setLevels(eng.meterL, eng.meterR)
        lbStatus.text = if (audioOk)
            String.format("CPU %.1f%% · XRUN %d", eng.cpu * 100, eng.xruns)
        else "无音频设备（仍可离线导出）"
        while (true) {
            val m = midiQ.poll() ?: break
            handleMidi(m)
        }
        if (!eng.playing && cbPattern.selectedIndex != proj.current) {
            cbPattern.selectedIndex = proj.current
        }
    }

    /** Tap Tempo：去掉最大最小后取平均，异常值（<30 或 >400 BPM）丢弃。 */
    private fun onTap() {
        val now = System.nanoTime()
        if (taps.isNotEmpty() && now - taps[taps.size - 1] > 2_000_000_000L) taps.clear()
        taps.add(now)
        if (taps.size > 8) taps.removeAt(0)
        if (taps.size >= 2) {
            val iv = ArrayList<Double>()
            for (i in 1 until taps.size) {
                val d = (taps[i] - taps[i - 1]) / 1e9
                if (d > 0.15 && d < 2.0) iv.add(d)
            }
            if (iv.size >= 2) {
                iv.remove(iv.maxOrNull())
                iv.remove(iv.minOrNull())
            }
            if (iv.isNotEmpty()) {
                val avg = iv.average()
                spBpm.value = Math.max(40.0, Math.min(300.0, 60.0 / avg))
            }
        }
    }

    // ==================================================================
    // MIDI 输入
    // ==================================================================
    private fun startMidi() {
        try {
            for (info in MidiSystem.getMidiDeviceInfo()) {
                // 跳过软件音序器/合成器，只找真正的输入端口
                val nm = info.name
                if (nm.contains("Sequencer") || nm.contains("Gervill")) continue
                val d = MidiSystem.getMidiDevice(info)
                if (d.maxTransmitters == 0) continue
                d.open()
                val tr = d.transmitter
                tr.receiver = object : Receiver {
                    override fun send(message: MidiMessage, timeStamp: Long) {
                        val b = message.message
                        if (b.size >= 3) {
                            midiQ.add(intArrayOf(
                                    b[0].toInt() and 0xFF,
                                    b[1].toInt() and 0xFF,
                                    b[2].toInt() and 0xFF
                            ))
                        } else if (b.size == 1) {
                            // 实时消息（时钟/开始/停止）
                            midiQ.add(intArrayOf(b[0].toInt() and 0xFF, 0, 0))
                        }
                    }

                    override fun close() {}
                }
                midiDev = d
                midiTransmitter = tr
                lbMidi.text = "MIDI: " + info.name
                lbMidi.foreground = ORANGE
                return
            }
            lbMidi.text = "MIDI: 无设备"
        } catch (ex: Exception) {
            lbMidi.text = "MIDI: 不可用"
        }
    }

    private fun stopMidi() {
        try {
            midiTransmitter?.close()
            midiTransmitter = null
            midiDev?.close()
            midiDev = null
        } catch (ignored: Exception) {
        }
    }

    private fun handleMidi(m: IntArray) {
        val cmd = m[0] and 0xF0
        when {
            cmd == 0x90 && m[2] > 0 -> {
                for (p in 0 until XuProject.PADS) {
                    if (XuProject.MIDI_NOTES[p] == m[1]) {
                        eng.triggerPad(p, m[2], m[2] >= 120)
                        break
                    }
                }
            }

            cmd == 0xB0 -> when (m[1]) {
                7 -> proj.master.volume = m[2] / 127.0
                74 -> proj.master.lpCutoff = 20.0 * Math.pow(1000.0, m[2] / 127.0)
                71 -> proj.master.lpQ = 0.5 + m[2] / 127.0 * 7.5
            }

            cmd == 0xC0 -> cbPattern.selectedIndex = m[1] % XuProject.PATTERNS
            m[0] == 0xFA -> eng.play()
            m[0] == 0xFC -> {
                eng.stop()
                grid.setPlayhead(-1)
            }

            m[0] == 0xF8 -> if (eng.clockExternal) eng.externalTick()
        }
    }

    // ==================================================================
    // 文件操作
    // ==================================================================
    private fun saveProject() {
        val fc = JFileChooser()
        fc.dialogTitle = "保存工程"
        fc.selectedFile = File("xu808.json")
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return
        try {
            val path = ensureExt(fc.selectedFile.absolutePath, ".json")
            proj.save(path)
            lbStatus.text = "已保存：${File(path).name}"
        } catch (ex: Exception) {
            JOptionPane.showMessageDialog(this, "保存失败：" + ex.message, "错误", JOptionPane.ERROR_MESSAGE)
        }
    }

    private fun loadProject() {
        val fc = JFileChooser()
        fc.dialogTitle = "打开工程"
        if (fc.showOpenDialog(this) != JFileChooser.APPROVE_OPTION) return
        try {
            val np = XuProject.open(fc.selectedFile.absolutePath)
            eng.stop()
            // 把新内容拷进现有对象，避免 UI 各处仍持有旧引用
            proj.name = np.name
            proj.seed = np.seed
            proj.current = np.current
            proj.patterns = np.patterns
            proj.pads = np.pads
            proj.master = np.master
            proj.song = np.song
            cbPattern.removeAllItems()
            for (i in 0 until XuProject.PATTERNS)
                cbPattern.addItem(String.format("%02d %s", i + 1, proj.patterns[i].name))
            cbPattern.selectedIndex = proj.current
            spSteps.value = proj.cur().steps
            slBpm.value = proj.master.bpm.toInt()
            slSwing.value = (proj.master.swing * 100).toInt()
            grid.rebuild()
            syncStrips()
            lbStatus.text = "已打开：${fc.selectedFile.name}"
        } catch (ex: Exception) {
            JOptionPane.showMessageDialog(this, "加载失败：" + ex.message, "错误", JOptionPane.ERROR_MESSAGE)
        }
    }

    /** 导出放到后台线程，绝不冻结 EDT（渲染一首歌可能要几秒）。 */
    private fun runAsync(busyText: String, work: () -> String) {
        lbStatus.text = busyText
        cursor = Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR)
        Thread {
            var ok = true
            var msg: String
            try {
                msg = work()
            } catch (ex: Exception) {
                ok = false
                msg = "失败：" + (ex.message ?: ex.javaClass.simpleName)
            }
            val finalOk = ok
            val finalMsg = msg
            SwingUtilities.invokeLater {
                cursor = Cursor.getDefaultCursor()
                lbStatus.text = finalMsg
                if (!finalOk) JOptionPane.showMessageDialog(
                        this, finalMsg, "错误", JOptionPane.ERROR_MESSAGE
                )
            }
        }.start()
    }

    private fun exportWav() {
        val fc = JFileChooser()
        fc.dialogTitle = "导出 WAV"
        fc.selectedFile = File("xu808.wav")
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return
        val path = ensureExt(fc.selectedFile.absolutePath, ".wav")
        val song = proj.master.songMode
        runAsync("正在导出 WAV…") {
            eng.exportWav(path, song)
            "WAV 已导出：" + File(path).name
        }
    }

    private fun exportMidi() {
        val fc = JFileChooser()
        fc.dialogTitle = "导出 MIDI"
        fc.selectedFile = File("xu808.mid")
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return
        val path = ensureExt(fc.selectedFile.absolutePath, ".mid")
        val song = proj.master.songMode
        runAsync("正在导出 MIDI…") {
            eng.exportMidi(path, song)
            "MIDI 已导出：" + File(path).name
        }
    }

    private fun exportStems() {
        val fc = JFileChooser()
        fc.dialogTitle = "分轨导出 WAV（选择基准文件名）"
        fc.selectedFile = File("xu808_stem.wav")
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return
        val path = ensureExt(fc.selectedFile.absolutePath, ".wav")
        val song = proj.master.songMode
        runAsync("正在导出 16 条分轨…") {
            eng.exportStems(path, song)
            "分轨已导出（16 个 WAV）"
        }
    }

    private fun editSong() {
        val sb = StringBuilder()
        for (e in proj.song) sb.append(e.pattern + 1).append(',').append(e.repeats).append('\n')
        val ta = JTextArea(sb.toString(), 10, 24)
        ta.font = Font("Consolas", Font.PLAIN, 12)
        val r = JOptionPane.showConfirmDialog(
                this, JScrollPane(ta),
                "歌曲链：每行“Pattern号,重复次数”", JOptionPane.OK_CANCEL_OPTION
        )
        if (r != JOptionPane.OK_OPTION) return
        proj.song.clear()
        for (raw in ta.text.split(Regex("\\R"))) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            val parts = line.split(",")
            try {
                val p = parts[0].trim().toInt() - 1
                val rep = if (parts.size > 1) parts[1].trim().toInt() else 1
                proj.song.add(XuProject.SongEntry(p.coerceIn(0, 15), Math.max(1, rep)))
            } catch (ignored: NumberFormatException) {
            }
        }
        lbStatus.text = "歌曲链已更新（${proj.song.size} 段）"
    }

    private fun editSettings() {
        val form = JPanel(GridLayout(0, 2, 6, 4))
        form.background = PANEL
        val seed = JSpinner(SpinnerNumberModel(proj.seed, 0, Int.MAX_VALUE, 1))
        val bpb = JSpinner(SpinnerNumberModel(proj.master.beatsPerBar, 1, 16, 1))
        val bu = JComboBox(arrayOf("4", "8"))
        bu.selectedItem = proj.master.beatUnit.toString()
        val hp = JCheckBox("高通启用", proj.master.hpOn)
        val lp = JCheckBox("低通启用", proj.master.lpOn)
        hp.isOpaque = false
        lp.isOpaque = false
        form.add(mkLabel("随机种子 Seed", TEXT)); form.add(seed)
        form.add(mkLabel("每小节拍数", TEXT)); form.add(bpb)
        form.add(mkLabel("拍号分母", TEXT)); form.add(bu)
        form.add(mkLabel("滤波开关", TEXT)); form.add(hp)
        form.add(JLabel(" ")); form.add(lp)

        val r = JOptionPane.showConfirmDialog(
                this, form, "设置 - 徐某的 808", JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE
        )
        if (r != JOptionPane.OK_OPTION) return
        proj.seed = (seed.value as Number).toInt()
        proj.master.beatsPerBar = (bpb.value as Number).toInt()
        proj.master.beatUnit = (bu.selectedItem as String).toInt()
        proj.master.hpOn = hp.isSelected
        proj.master.lpOn = lp.isSelected
    }

    private fun editStep(pad: Int, step: Int) {
        val st = proj.cur().data[pad][step]
        val form = JPanel(GridLayout(0, 2, 6, 4))
        form.background = PANEL
        val on = JCheckBox("启用", st.on)
        val acc = JCheckBox("重音 Accent", st.accent)
        val vel = JSpinner(SpinnerNumberModel(st.velocity, 1, 127, 1))
        val prob = JSpinner(SpinnerNumberModel(st.prob, 0, 100, 1))
        val mic = JSpinner(SpinnerNumberModel(st.micro, -2400, 2400, 10))
        val rat = JSpinner(SpinnerNumberModel(st.ratchet, 1, 8, 1))
        val cond = JComboBox(XuProject.COND_NAMES)
        cond.selectedIndex = st.cond.coerceIn(0, 4)
        on.isOpaque = false
        acc.isOpaque = false
        form.add(on); form.add(acc)
        form.add(mkLabel("力度 Velocity", TEXT)); form.add(vel)
        form.add(mkLabel("概率 Probability %", TEXT)); form.add(prob)
        form.add(mkLabel("微时 Micro（样本）", TEXT)); form.add(mic)
        form.add(mkLabel("滚奏 Ratchet", TEXT)); form.add(rat)
        form.add(mkLabel("条件触发", TEXT)); form.add(cond)

        val r = JOptionPane.showConfirmDialog(
                this, form,
                "步进属性 · ${XuProject.CODES[pad]} 第 ${step + 1} 步 - 徐某的 808",
                JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE
        )
        if (r != JOptionPane.OK_OPTION) return
        st.on = on.isSelected
        st.accent = acc.isSelected
        st.velocity = (vel.value as Number).toInt()
        st.prob = (prob.value as Number).toInt()
        st.micro = (mic.value as Number).toInt()
        st.ratchet = (rat.value as Number).toInt()
        st.cond = cond.selectedIndex
        grid.repaint()
    }

    private fun startAudio() {
        audio = XuAudio(eng)
        audioOk = audio?.start() == true
        if (!audioOk) {
            lbStatus.text = "无音频设备（仍可离线导出）"
        }
    }

    // ==================================================================
    // 步进网格（自绘）
    // ==================================================================
    private inner class GridPanel : JPanel() {
        // 布局常量与 Java 版一致，保证视觉一致
        private val cellH = 18
        private val labelW = 132
        private val headH = 24
        private var playhead = -1
        var cellW = 32
            private set

        init {
            background = BG
            addMouseListener(object : MouseAdapter() {
                override fun mousePressed(e: MouseEvent) {
                    val x = e.x
                    val y = e.y
                    if (y < headH) return
                    val r = (y - headH) / cellH
                    if (r < 0 || r >= 16) return
                    if (x < labelW) {
                        // 左侧：M/S 小按钮，其余区域点击试听
                        for (j in 0..1) {
                            val bx = 92 + j * 18
                            if (x >= bx && x <= bx + 15) {
                                if (j == 0) proj.pads[r].mute = !proj.pads[r].mute
                                else proj.pads[r].solo = !proj.pads[r].solo
                                syncStrips()
                                repaint()
                                return
                            }
                        }
                        eng.triggerPad(r, 112, false)
                        return
                    }
                    val c = (x - labelW) / cellW
                    if (c < 0 || c >= proj.cur().steps) return
                    if (e.button == MouseEvent.BUTTON3) {
                        editStep(r, c)      // 右键：步进属性
                    } else {
                        val s = proj.cur().data[r][c]
                        s.on = !s.on
                        if (s.on) eng.triggerPad(r, s.velocity, s.accent)
                        repaint()
                    }
                }
            })
        }

        fun rebuild() {
            val steps = Math.max(1, proj.cur().steps)
            val w = 1500
            cellW = Math.max(10, Math.min(32, (w - labelW) / steps))
            preferredSize = Dimension(labelW + steps * cellW + 20, headH + 16 * cellH + 8)
            revalidate()
            repaint()
        }

        fun setPlayhead(s: Int) {
            if (s != playhead) {
                playhead = s
                repaint()
            }
        }

        override fun paintComponent(g0: Graphics) {
            super.paintComponent(g0)
            val g = g0 as Graphics2D
            val steps = Math.max(1, proj.cur().steps)
            g.color = BG
            g.fillRect(0, 0, width, height)

            // 顶部步号
            g.color = PANEL
            g.fillRect(0, 0, width, headH)
            g.font = Font("Segoe UI", Font.PLAIN, 8)
            for (c in 0 until steps) {
                val x = labelW + c * cellW
                if (c % 4 == 0) {
                    g.color = Color(58, 58, 58)
                    g.fillRect(x, 0, 1, height)
                }
                g.color = DIM
                g.drawString((c + 1).toString(), x + cellW / 2 - 4, 16)
            }

            // 左侧声部名 / M / S
            g.color = PANEL2
            g.fillRect(0, headH, labelW, height - headH)
            for (r in 0 until 16) {
                val y = headH + r * cellH
                g.color = if (r % 2 == 0) Color(22, 22, 22) else Color(26, 26, 26)
                g.fillRect(labelW, y, width - labelW, cellH)
                g.font = Font("Consolas", Font.BOLD, 10)
                g.color = ORANGE
                g.drawString(XuProject.CODES[r], 6, y + 15)
                g.font = Font("Microsoft YaHei", Font.PLAIN, 8)
                g.color = TEXT
                g.drawString(XuProject.NAMES_CN[r], 42, y + 15)

                val pd = proj.pads[r]
                for (j in 0..1) {
                    val on = if (j == 0) pd.mute else pd.solo
                    val bx = 92 + j * 18
                    g.color = if (on) (if (j == 0) RED else YELLOW) else Color(42, 42, 42)
                    g.fillRect(bx, y + 4, 15, cellH - 8)
                    g.color = if (on) Color(18, 18, 18) else DIM
                    g.font = Font("Segoe UI", Font.BOLD, 7)
                    g.drawString(if (j == 0) "M" else "S", bx + 5, y + 14)
                }
                g.color = LINE
                g.drawLine(0, y + cellH - 1, width, y + cellH - 1)
            }

            // 16×N 格子
            for (c in 0 until steps) {
                val x = labelW + c * cellW
                for (r in 0 until 16) {
                    val st = proj.cur().data[r][c]
                    val y = headH + r * cellH
                    if (st.on) {
                        val base = if (st.accent) YELLOW else ORANGE
                        val a = 110 + (145 * (st.velocity / 127.0)).toInt()
                        g.color = Color(base.red, base.green, base.blue, a)
                        g.fillRect(x + 2, y + 2, cellW - 4, cellH - 4)
                        if (st.ratchet > 1 || st.prob < 100) {
                            g.color = Color(26, 16, 8)
                            g.font = Font("Consolas", Font.BOLD, 7)
                            g.drawString(
                                    if (st.ratchet > 1) "R" else st.prob.toString(),
                                    x + cellW / 2 - 3, y + 15
                            )
                        }
                    } else {
                        g.color = Color(16, 16, 16)
                        g.fillRect(x + 2, y + 2, cellW - 4, cellH - 4)
                        g.color = Color(39, 39, 39)
                        g.drawRect(x + 2, y + 2, cellW - 5, cellH - 5)
                    }
                }
            }

            // 播放头（青色）
            if (playhead in 0 until steps) {
                val x = labelW + playhead * cellW
                g.color = Color(76, 201, 240, 26)
                g.fillRect(x, headH, cellW, height - headH)
                g.color = CYAN
                g.drawLine(x, headH, x, height)
                g.drawLine(x + cellW, headH, x + cellW, height)
            }
            g.color = LINE
            g.drawRect(0, 0, width - 1, height - 1)
        }
    }

    // ==================================================================
    // 通道条：名称、M/S、音色参数、Level/Pan、轨道长度
    // ==================================================================
    private inner class Strip(private val pad: Int) : JPanel() {
        private val params = LinkedHashMap<String, JSlider>()
        private lateinit var bm: JToggleButton
        private lateinit var bs: JToggleButton
        private lateinit var len: JSpinner
        private var updating = false

        init {
            background = BG
            preferredSize = Dimension(78, 300)
            layout = BoxLayout(this, BoxLayout.Y_AXIS)

            val name = JButton(XuProject.CODES[pad] + " " + XuProject.NAMES_CN[pad])
            name.font = Font("Microsoft YaHei", Font.PLAIN, 9)
            name.foreground = ORANGE
            name.background = PANEL2
            name.isFocusPainted = false
            name.maximumSize = Dimension(76, 30)
            name.alignmentX = Component.CENTER_ALIGNMENT
            name.addActionListener { eng.triggerPad(pad, 112, false) }
            add(name)

            val ms = JPanel(FlowLayout(FlowLayout.LEFT, 1, 0))
            ms.isOpaque = false
            bm = JToggleButton("M")
            bs = JToggleButton("S")
            for (b in arrayOf(bm, bs)) {
                b.preferredSize = Dimension(30, 18)
                b.font = Font("Segoe UI", Font.BOLD, 8)
                b.background = PANEL2
                b.foreground = TEXT
                b.isFocusPainted = false
            }
            bm.isSelected = proj.pads[pad].mute
            bs.isSelected = proj.pads[pad].solo
            bm.addActionListener {
                proj.pads[pad].mute = bm.isSelected
                grid.repaint()
            }
            bs.addActionListener {
                proj.pads[pad].solo = bs.isSelected
                grid.repaint()
            }
            ms.add(bm)
            ms.add(bs)
            ms.maximumSize = Dimension(76, 20)
            add(ms)

            for (d in paramsFor(pad)) addParam(d.first, d.second)
            addParam("LEVEL", "level")
            addParam("PAN", "pan")

            // 先把剩余高度吃掉，让所有通道条的“轨道长度”统一贴在底部
            add(Box.createVerticalGlue())

            len = JSpinner(SpinnerNumberModel(proj.cur().trackLen[pad], 1, 64, 1))
            len.maximumSize = Dimension(68, 20)
            len.alignmentX = Component.CENTER_ALIGNMENT
            len.toolTipText = "轨道步数（polymeter，1-64）"
            len.addChangeListener {
                proj.cur().trackLen[pad] = (len.value as Number).toInt()
            }
            add(len)
        }

        private fun addParam(text: String, key: String) {
            val l = JLabel(text)
            l.foreground = DIM
            l.font = Font("Microsoft YaHei", Font.PLAIN, 7)
            l.alignmentX = Component.CENTER_ALIGNMENT
            add(l)

            val s = JSlider(JSlider.VERTICAL, 0, 100, (proj.pads[pad].get(key) * 100).toInt())
            s.background = BG
            s.preferredSize = Dimension(18, 32)
            s.maximumSize = Dimension(18, 32)
            s.alignmentX = Component.CENTER_ALIGNMENT
            s.addChangeListener {
                if (updating) return@addChangeListener
                proj.pads[pad].set(key, s.value / 100.0)
            }
            add(s)
            params[key] = s
        }

        fun sync() {
            updating = true
            for ((k, v) in params) v.value = (proj.pads[pad].get(k) * 100).toInt()
            bm.isSelected = proj.pads[pad].mute
            bs.isSelected = proj.pads[pad].solo
            len.value = proj.cur().trackLen[pad]
            updating = false
        }
    }

    private fun paramsFor(pad: Int): List<Pair<String, String>> {
        val l = ArrayList<Pair<String, String>>()
        when {
            pad == 0 -> {
                l.add("TUNE" to "tune"); l.add("DECAY" to "decay"); l.add("TONE" to "tone")
            }

            pad == 1 -> {
                l.add("TUNE" to "tune"); l.add("SNAP" to "snappy"); l.add("DECAY" to "decay")
            }

            pad in 2..4 -> {
                l.add("TUNE" to "tune"); l.add("DECAY" to "decay"); l.add("COLOR" to "tone")
            }

            pad == 10 -> {
                l.add("DECAY" to "decay"); l.add("TONE" to "tone"); l.add("TAIL" to "tail")
            }

            pad == 11 -> {
                l.add("DECAY" to "decay"); l.add("TONE" to "tone")
            }

            pad == 12 -> {
                l.add("TUNE" to "tune"); l.add("DECAY" to "decay")
            }

            pad >= 13 -> {
                l.add("DECAY" to "decay"); l.add("TONE" to "tone")
            }

            else -> {
                l.add("TUNE" to "tune"); l.add("DECAY" to "decay")
            }
        }
        return l
    }

    // ==================================================================
    // 电平表
    // ==================================================================
    private inner class MeterPanel : JPanel() {
        private var l = 0f
        private var r = 0f

        init {
            preferredSize = Dimension(160, 30)
            maximumSize = Dimension(160, 30)
            background = Color(12, 12, 12)
        }

        fun setLevels(a: Float, b: Float) {
            l = a
            r = b
            repaint()
        }

        override fun paintComponent(g: Graphics) {
            super.paintComponent(g)
            for (i in 0..1) {
                val v = Math.max(0f, Math.min(1f, if (i == 0) l else r))
                val y = 3 + i * 9
                g.color = Color(26, 26, 26)
                g.fillRect(2, y, width - 4, 7)
                val w = ((width - 4) * v).toInt()
                if (w > 0) {
                    g.color = if (v > 0.9f) RED else if (v > 0.7f) YELLOW else Color(61, 220, 132)
                    g.fillRect(2, y, w, 7)
                }
            }
            g.color = LINE
            g.drawRect(0, 0, width - 1, height - 1)
        }
    }
}
