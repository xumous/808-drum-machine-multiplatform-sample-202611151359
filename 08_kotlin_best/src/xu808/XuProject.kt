// ============================================================================
//  XuProject.kt —— 工程数据模型（Kotlin 版）  ·  徐某
//
//  字段名、默认值、JSON 结构与 Java/Python/C#/Qt 版完全一致，
//  因此各语言版本保存的工程可以互相打开。
//  Step/Pattern/Pad/Master/SongEntry 都写成普通类（数据袋子），
//  序列化用 toMap()/fromMap() 手工完成——字段少，手写比反射更可控。
// ============================================================================
package xu808

import java.io.IOException
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Paths
import java.util.LinkedHashMap

class XuProject {

    // ---- 兼容旧工程：所有常量与 Java 版一致 ----
    companion object {
        const val PADS = 16
        const val MAX_STEPS = 64
        const val PATTERNS = 16
        const val SR = 48000

        // 条件触发（编号固定，JSON 里存整数）
        const val COND_ALWAYS = 0
        const val COND_FILL = 1
        const val COND_NOTFILL = 2
        const val COND_EVERY2 = 3
        const val COND_EVERY4 = 4

        val COND_NAMES = arrayOf("总是", "仅过门", "非过门", "每2循环", "每4循环")

        // 16 声部代号与中文名（顺序即声部编号，不可改动）
        val CODES = arrayOf(
                "BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC",
                "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"
        )
        val NAMES_CN = arrayOf(
                "底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加",
                "边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲"
        )
        val MIDI_NOTES = intArrayOf(
                36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42
        )

        private fun clamp(v: Int, lo: Int, hi: Int) = if (v < lo) lo else if (v > hi) hi else v

        /** 从文件打开工程（UI 与命令行共用）。 */
        @JvmStatic
        @Throws(IOException::class)
        fun open(path: String): XuProject {
            val p = XuProject()
            p.load(path)
            return p
        }
    }

    /** 一个步进格子：开关 + 6 个属性。 */
    class Step {
        var on = false
        var velocity = 100          // 1..127
        var accent = false
        var prob = 100              // 0..100
        var micro = 0               // -2400..+2400 样本
        var ratchet = 1             // 1..8
        var cond = COND_ALWAYS      // 条件触发

        fun copy(): Step = Step().also {
            it.on = on; it.velocity = velocity; it.accent = accent
            it.prob = prob; it.micro = micro; it.ratchet = ratchet; it.cond = cond
        }
    }

    /** 16 轨 × 64 步；trackLen 实现 polymeter。 */
    class Pattern {
        var name = "PATTERN"
        var steps = 16
        var fill = false
        val data = Array(PADS) { Array(MAX_STEPS) { Step() } }
        val trackLen = IntArray(PADS) { 16 }

        /** 内置示范节奏，让程序一启动就有东西可听。 */
        fun demoBeat() {
            for (p in 0 until PADS)
                for (s in 0 until MAX_STEPS) data[p][s] = Step()
            steps = 16
            name = "PATTERN 1"
            set(0, 0, 118, true); set(0, 8, 105, false); set(0, 14, 90, false)
            set(1, 4, 110, false); set(1, 12, 110, true)
            for (s in 0 until 16 step 2) set(15, s, if (s % 4 == 0) 96 else 84, false)
            set(14, 14, 92, false)
            set(10, 12, 80, false)
            for (s in 1 until 16 step 2) set(11, s, 42, false)
            set(0, 15, 88, false)
            data[0][15].ratchet = 3     // 末尾三连滚奏，一耳朵能听出 ratchet
        }

        private fun set(p: Int, s: Int, v: Int, a: Boolean) {
            data[p][s].on = true
            data[p][s].velocity = v
            data[p][s].accent = a
        }
    }

    /** 每声部音色与混音参数（用 Double，DSP 公式里可以少写 toDouble()）。 */
    class Pad {
        var tune = 0.50
        var decay = 0.50
        var tone = 0.50
        var snappy = 0.55
        var tail = 0.45
        var level = 0.75
        var pan = 0.50
        var mute = false
        var solo = false

        /** UI 用字符串键读写，避免为 7 个参数写 7 组监听器。 */
        fun get(key: String): Double = when (key) {
            "tune" -> tune
            "decay" -> decay
            "tone" -> tone
            "snappy" -> snappy
            "tail" -> tail
            "level" -> level
            else -> pan
        }

        fun set(key: String, v: Double) {
            when (key) {
                "tune" -> tune = v
                "decay" -> decay = v
                "tone" -> tone = v
                "snappy" -> snappy = v
                "tail" -> tail = v
                "level" -> level = v
                else -> pan = v
            }
        }
    }

    /** 主效果链与全局设置。 */
    class Master {
        var volume = 0.80
        var drive = 1.35
        var lpCutoff = 18000.0
        var lpQ = 0.90
        var hpCutoff = 28.0
        var hpQ = 0.70
        var lpOn = true
        var hpOn = true
        var bpm = 120.0
        var swing = 0.0                  // 0..0.75
        var beatsPerBar = 4
        var beatUnit = 4
        var songMode = false
    }

    class SongEntry(var pattern: Int = 0, var repeats: Int = 1)

    // ---- 工程本体 ----
    var name = "徐某的 808 工程"
    var patterns = Array(PATTERNS) { Pattern() }
    var pads = Array(PADS) { Pad() }
    var master = Master()
    var song = ArrayList<SongEntry>()
    var current = 0
    var seed = 0x808808

    init {
        // 默认工程：1 号 Pattern 有示范节奏，歌曲链是“1 号重复 4 次 ×2”
        for (i in 0 until PATTERNS) patterns[i].name = "PATTERN ${i + 1}"
        patterns[0].demoBeat()
        pads[0].decay = 0.55
        pads[0].tone = 0.45
        song.add(SongEntry(0, 4))
        song.add(SongEntry(0, 4))
    }

    fun cur(): Pattern = patterns[current.coerceIn(0, PATTERNS - 1)]

    // ------------------------------------------------------------------
    // 序列化
    // ------------------------------------------------------------------
    fun toMap(): Map<String, Any?> {
        val root = LinkedHashMap<String, Any?>()
        root["format"] = "XU808"
        root["version"] = 1
        root["name"] = name
        root["seed"] = seed
        root["current"] = current

        val m = LinkedHashMap<String, Any?>()
        m["volume"] = master.volume; m["drive"] = master.drive
        m["lpCutoff"] = master.lpCutoff; m["lpQ"] = master.lpQ
        m["hpCutoff"] = master.hpCutoff; m["hpQ"] = master.hpQ
        m["lpOn"] = master.lpOn; m["hpOn"] = master.hpOn
        m["bpm"] = master.bpm; m["swing"] = master.swing
        m["beatsPerBar"] = master.beatsPerBar; m["beatUnit"] = master.beatUnit
        m["songMode"] = master.songMode
        root["master"] = m

        val pa = ArrayList<Any?>()
        for (p in pads) {
            val o = LinkedHashMap<String, Any?>()
            o["tune"] = p.tune; o["decay"] = p.decay; o["tone"] = p.tone
            o["snappy"] = p.snappy; o["tail"] = p.tail
            o["level"] = p.level; o["pan"] = p.pan
            o["mute"] = p.mute; o["solo"] = p.solo
            pa.add(o)
        }
        root["pads"] = pa

        val so = LinkedHashMap<String, Any?>()
        so["enabled"] = master.songMode
        val se = ArrayList<Any?>()
        for (e in song) {
            val o = LinkedHashMap<String, Any?>()
            o["pattern"] = e.pattern; o["repeats"] = e.repeats
            se.add(o)
        }
        so["entries"] = se
        root["song"] = so

        val pats = ArrayList<Any?>()
        for (pat in patterns) {
            val o = LinkedHashMap<String, Any?>()
            o["name"] = pat.name; o["steps"] = pat.steps; o["fill"] = pat.fill
            o["trackLen"] = pat.trackLen.toList()
            val rows = ArrayList<Any?>()
            for (p in 0 until PADS) {
                val row = ArrayList<Any?>()
                for (s in 0 until MAX_STEPS) {
                    val st = pat.data[p][s]
                    // 每格 7 个数：on, velocity, accent, prob, micro, ratchet, cond
                    row.add(listOf(
                            if (st.on) 1 else 0, st.velocity, if (st.accent) 1 else 0,
                            st.prob, st.micro, st.ratchet, st.cond
                    ))
                }
                rows.add(row)
            }
            o["data"] = rows
            pats.add(o)
        }
        root["patterns"] = pats
        return root
    }

    fun save(path: String) {
        Files.write(Paths.get(path), Json.write(toMap()).toByteArray(StandardCharsets.UTF_8))
    }

    // ------------------------------------------------------------------
    // 反序列化：缺失字段一律用默认值（前向兼容），非法值夹到合法区间
    // ------------------------------------------------------------------
    fun loadFromMap(root: Map<String, Any?>) {
        name = Json.str(root, "name", name)
        seed = Json.integer(root, "seed", seed)
        current = clamp(Json.integer(root, "current", 0), 0, PATTERNS - 1)

        val m = Json.obj(root["master"])
        master.volume = Json.num(m, "volume", master.volume)
        master.drive = Json.num(m, "drive", master.drive)
        master.lpCutoff = Json.num(m, "lpCutoff", master.lpCutoff)
        master.lpQ = Json.num(m, "lpQ", master.lpQ)
        master.hpCutoff = Json.num(m, "hpCutoff", master.hpCutoff)
        master.hpQ = Json.num(m, "hpQ", master.hpQ)
        master.lpOn = Json.bool(m, "lpOn", master.lpOn)
        master.hpOn = Json.bool(m, "hpOn", master.hpOn)
        master.bpm = Json.num(m, "bpm", master.bpm)
        master.swing = Json.num(m, "swing", master.swing)
        master.beatsPerBar = Json.integer(m, "beatsPerBar", master.beatsPerBar)
        master.beatUnit = Json.integer(m, "beatUnit", master.beatUnit)
        master.songMode = Json.bool(m, "songMode", master.songMode)

        val pa = Json.arr(root["pads"])
        for (i in 0 until minOf(PADS, pa.size)) {
            val o = Json.obj(pa[i])
            val d = pads[i]
            d.tune = Json.num(o, "tune", d.tune)
            d.decay = Json.num(o, "decay", d.decay)
            d.tone = Json.num(o, "tone", d.tone)
            d.snappy = Json.num(o, "snappy", d.snappy)
            d.tail = Json.num(o, "tail", d.tail)
            d.level = Json.num(o, "level", d.level)
            d.pan = Json.num(o, "pan", d.pan)
            d.mute = Json.bool(o, "mute", d.mute)
            d.solo = Json.bool(o, "solo", d.solo)
        }

        val pats = Json.arr(root["patterns"])
        for (i in 0 until minOf(PATTERNS, pats.size)) {
            val o = Json.obj(pats[i])
            val pat = patterns[i]
            pat.name = Json.str(o, "name", pat.name)
            pat.steps = clamp(Json.integer(o, "steps", 16), 1, MAX_STEPS)
            pat.fill = Json.bool(o, "fill", false)
            val tl = Json.arr(o["trackLen"])
            for (t in 0 until minOf(PADS, tl.size)) {
                val v = tl[t]
                if (v is Number) pat.trackLen[t] = clamp(v.toInt(), 1, MAX_STEPS)
            }
            val rows = Json.arr(o["data"])
            for (pad in 0 until minOf(PADS, rows.size)) {
                val row = Json.arr(rows[pad])
                for (s in 0 until minOf(MAX_STEPS, row.size)) {
                    val one = Json.arr(row[s])
                    val st = pat.data[pad][s]
                    st.on = one.size > 0 && (one[0] as? Number)?.toInt() != 0
                    st.velocity = clamp((one.getOrNull(1) as? Number)?.toInt() ?: 100, 1, 127)
                    st.accent = one.size > 2 && (one[2] as? Number)?.toInt() != 0
                    st.prob = clamp((one.getOrNull(3) as? Number)?.toInt() ?: 100, 0, 100)
                    st.micro = clamp((one.getOrNull(4) as? Number)?.toInt() ?: 0, -2400, 2400)
                    st.ratchet = clamp((one.getOrNull(5) as? Number)?.toInt() ?: 1, 1, 8)
                    st.cond = clamp((one.getOrNull(6) as? Number)?.toInt() ?: 0, 0, 4)
                }
            }
        }

        val se = Json.arr(Json.obj(root["song"])["entries"])
        song.clear()
        for (v in se) {
            val e = Json.obj(v)
            song.add(SongEntry(
                    clamp(Json.integer(e, "pattern", 0), 0, PATTERNS - 1),
                    Math.max(1, Json.integer(e, "repeats", 1))
            ))
        }
    }

    /** 从 JSON 文本构造新工程（用于导出时的“深拷贝”，避免改到正在播放的工程）。 */
    fun copyProject(): XuProject {
        val p = XuProject()
        p.loadFromMap(Json.obj(Json.parse(Json.write(toMap()))))
        return p
    }

    fun load(path: String) {
        val text = String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8)
        val root = Json.obj(Json.parse(text))
        loadFromMap(root)
    }
}
