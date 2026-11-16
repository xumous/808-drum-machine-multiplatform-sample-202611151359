// ============================================================================
//  XuProject.java —— 工程数据模型（Java 版）
//
//  字段名与 Python/C#/Qt 版完全一致，序列化后可以互相打开。
//  为了减少文件数量，Step/Pattern/Pad/Master/SongEntry 都做成静态内部类；
//  它们只是“数据袋子”，没有复杂逻辑，放在一起反而更好读。
// ============================================================================
package xu808;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class XuProject {
    public static final int PADS = 16;
    public static final int MAX_STEPS = 64;
    public static final int PATTERNS = 16;
    public static final int SR = 48000;

    // ---- 条件触发 ----
    public static final int COND_ALWAYS = 0, COND_FILL = 1, COND_NOTFILL = 2,
            COND_EVERY2 = 3, COND_EVERY4 = 4;
    public static final String[] COND_NAMES = {"总是", "仅过门", "非过门", "每2循环", "每4循环"};

    // ---- 声部代号与 MIDI 音符 ----
    public static final String[] CODES = {
            "BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"
    };
    public static final String[] NAMES_CN = {
            "底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加",
            "边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲"
    };
    public static final int[] MIDI_NOTES = {
            36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42
    };
    // ---- 工程本体 ----
    public String name = "徐某的 808 工程";
    public Pattern[] patterns = new Pattern[PATTERNS];
    public Pad[] pads = new Pad[PADS];
    public Master master = new Master();
    public ArrayList<SongEntry> song = new ArrayList<>();
    public int current = 0;
    public int seed = 0x808808;

    public XuProject() {
        for (int i = 0; i < PATTERNS; i++) {
            patterns[i] = new Pattern();
            patterns[i].name = "PATTERN " + (i + 1);
        }
        patterns[0].demoBeat();
        for (int i = 0; i < PADS; i++) pads[i] = new Pad();
        pads[0].decay = 0.55f;
        pads[0].tone = 0.45f;
        song.add(new SongEntry(0, 4));
        song.add(new SongEntry(0, 4));
    }

    public static XuProject fromMap(Map<String, Object> root) {
        XuProject p = new XuProject();
        p.name = Json.str(root, "name", p.name);
        p.seed = Json.integer(root, "seed", p.seed);
        p.current = Math.max(0, Math.min(PATTERNS - 1, Json.integer(root, "current", 0)));

        Map<String, Object> m = Json.obj(root.get("master"));
        p.master.volume = (float) Json.num(m, "volume", p.master.volume);
        p.master.drive = (float) Json.num(m, "drive", p.master.drive);
        p.master.lpCutoff = (float) Json.num(m, "lpCutoff", p.master.lpCutoff);
        p.master.lpQ = (float) Json.num(m, "lpQ", p.master.lpQ);
        p.master.hpCutoff = (float) Json.num(m, "hpCutoff", p.master.hpCutoff);
        p.master.hpQ = (float) Json.num(m, "hpQ", p.master.hpQ);
        p.master.lpOn = Json.bool(m, "lpOn", p.master.lpOn);
        p.master.hpOn = Json.bool(m, "hpOn", p.master.hpOn);
        p.master.bpm = (float) Json.num(m, "bpm", p.master.bpm);
        p.master.swing = (float) Json.num(m, "swing", p.master.swing);
        p.master.beatsPerBar = Json.integer(m, "beatsPerBar", p.master.beatsPerBar);
        p.master.beatUnit = Json.integer(m, "beatUnit", p.master.beatUnit);
        p.master.songMode = Json.bool(m, "songMode", p.master.songMode);

        List<Object> pa = Json.arr(root.get("pads"));
        for (int i = 0; i < PADS && i < pa.size(); i++) {
            Map<String, Object> o = Json.obj(pa.get(i));
            Pad d = p.pads[i];
            d.tune = (float) Json.num(o, "tune", d.tune);
            d.decay = (float) Json.num(o, "decay", d.decay);
            d.tone = (float) Json.num(o, "tone", d.tone);
            d.snappy = (float) Json.num(o, "snappy", d.snappy);
            d.tail = (float) Json.num(o, "tail", d.tail);
            d.level = (float) Json.num(o, "level", d.level);
            d.pan = (float) Json.num(o, "pan", d.pan);
            d.mute = Json.bool(o, "mute", d.mute);
            d.solo = Json.bool(o, "solo", d.solo);
        }

        List<Object> pats = Json.arr(root.get("patterns"));
        for (int i = 0; i < PATTERNS && i < pats.size(); i++) {
            Map<String, Object> o = Json.obj(pats.get(i));
            Pattern pat = p.patterns[i];
            pat.name = Json.str(o, "name", pat.name);
            pat.steps = Math.max(1, Math.min(MAX_STEPS, Json.integer(o, "steps", 16)));
            pat.fill = Json.bool(o, "fill", false);
            List<Object> tl = Json.arr(o.get("trackLen"));
            for (int t = 0; t < PADS && t < tl.size(); t++) {
                pat.trackLen[t] = Math.max(1, Math.min(MAX_STEPS,
                        ((Number) tl.get(t)).intValue()));
            }
            List<Object> rows = Json.arr(o.get("data"));
            for (int pad = 0; pad < PADS && pad < rows.size(); pad++) {
                List<Object> row = Json.arr(rows.get(pad));
                for (int s = 0; s < MAX_STEPS && s < row.size(); s++) {
                    List<Object> one = Json.arr(row.get(s));
                    Step st = pat.data[pad][s];
                    st.on = one.size() > 0 && ((Number) one.get(0)).intValue() != 0;
                    st.velocity = clamp(one.size() > 1 ? ((Number) one.get(1)).intValue() : 100, 1, 127);
                    st.accent = one.size() > 2 && ((Number) one.get(2)).intValue() != 0;
                    st.prob = clamp(one.size() > 3 ? ((Number) one.get(3)).intValue() : 100, 0, 100);
                    st.micro = clamp(one.size() > 4 ? ((Number) one.get(4)).intValue() : 0, -2400, 2400);
                    st.ratchet = clamp(one.size() > 5 ? ((Number) one.get(5)).intValue() : 1, 1, 8);
                    st.cond = clamp(one.size() > 6 ? ((Number) one.get(6)).intValue() : 0, 0, 4);
                }
            }
        }

        List<Object> se = Json.arr(Json.obj(root.get("song")).get("entries"));
        p.song.clear();
        for (Object v : se) {
            Map<String, Object> e = Json.obj(v);
            p.song.add(new SongEntry(
                    clamp(Json.integer(e, "pattern", 0), 0, PATTERNS - 1),
                    Math.max(1, Json.integer(e, "repeats", 1))));
        }
        return p;
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    public static XuProject load(String path) throws IOException {
        String text = new String(Files.readAllBytes(Paths.get(path)), StandardCharsets.UTF_8);
        return fromMap(Json.obj(Json.parse(text)));
    }

    public Pattern cur() {
        return patterns[Math.max(0, Math.min(current, PATTERNS - 1))];
    }

    // ------------------------------------------------------------------
    // JSON
    // ------------------------------------------------------------------
    public Map<String, Object> toMap() {
        Map<String, Object> root = new LinkedHashMap<>();
        root.put("format", "XU808");
        root.put("version", 1);
        root.put("name", name);
        root.put("seed", seed);
        root.put("current", current);

        Map<String, Object> m = new LinkedHashMap<>();
        m.put("volume", master.volume);
        m.put("drive", master.drive);
        m.put("lpCutoff", master.lpCutoff);
        m.put("lpQ", master.lpQ);
        m.put("hpCutoff", master.hpCutoff);
        m.put("hpQ", master.hpQ);
        m.put("lpOn", master.lpOn);
        m.put("hpOn", master.hpOn);
        m.put("bpm", master.bpm);
        m.put("swing", master.swing);
        m.put("beatsPerBar", master.beatsPerBar);
        m.put("beatUnit", master.beatUnit);
        m.put("songMode", master.songMode);
        root.put("master", m);

        List<Object> pa = new ArrayList<>();
        for (Pad p : pads) {
            Map<String, Object> o = new LinkedHashMap<>();
            o.put("tune", p.tune);
            o.put("decay", p.decay);
            o.put("tone", p.tone);
            o.put("snappy", p.snappy);
            o.put("tail", p.tail);
            o.put("level", p.level);
            o.put("pan", p.pan);
            o.put("mute", p.mute);
            o.put("solo", p.solo);
            pa.add(o);
        }
        root.put("pads", pa);

        Map<String, Object> so = new LinkedHashMap<>();
        so.put("enabled", master.songMode);
        List<Object> se = new ArrayList<>();
        for (SongEntry e : song) {
            Map<String, Object> o = new LinkedHashMap<>();
            o.put("pattern", e.pattern);
            o.put("repeats", e.repeats);
            se.add(o);
        }
        so.put("entries", se);
        root.put("song", so);

        List<Object> pats = new ArrayList<>();
        for (Pattern pat : patterns) {
            Map<String, Object> o = new LinkedHashMap<>();
            o.put("name", pat.name);
            o.put("steps", pat.steps);
            o.put("fill", pat.fill);
            List<Object> tl = new ArrayList<>();
            for (int t : pat.trackLen) tl.add(t);
            o.put("trackLen", tl);
            List<Object> rows = new ArrayList<>();
            for (int p = 0; p < PADS; p++) {
                List<Object> row = new ArrayList<>();
                for (int s = 0; s < MAX_STEPS; s++) {
                    Step st = pat.data[p][s];
                    List<Object> one = new ArrayList<>();
                    one.add(st.on ? 1 : 0);
                    one.add(st.velocity);
                    one.add(st.accent ? 1 : 0);
                    one.add(st.prob);
                    one.add(st.micro);
                    one.add(st.ratchet);
                    one.add(st.cond);
                    row.add(one);
                }
                rows.add(row);
            }
            o.put("data", rows);
            pats.add(o);
        }
        root.put("patterns", pats);
        return root;
    }

    public void save(String path) throws IOException {
        Files.write(Paths.get(path), Json.write(toMap()).getBytes(StandardCharsets.UTF_8));
    }

    /**
     * 一个格子。
     */
    public static class Step {
        public boolean on = false;
        public int velocity = 100;
        public boolean accent = false;
        public int prob = 100;
        public int micro = 0;
        public int ratchet = 1;
        public int cond = COND_ALWAYS;

        public Step copy() {
            Step s = new Step();
            s.on = on;
            s.velocity = velocity;
            s.accent = accent;
            s.prob = prob;
            s.micro = micro;
            s.ratchet = ratchet;
            s.cond = cond;
            return s;
        }
    }

    /**
     * 16 轨 × 64 步。
     */
    public static class Pattern {
        public String name = "PATTERN";
        public int steps = 16;
        public boolean fill = false;
        public Step[][] data = new Step[PADS][MAX_STEPS];
        public int[] trackLen = new int[PADS];

        public Pattern() {
            for (int p = 0; p < PADS; p++) {
                for (int s = 0; s < MAX_STEPS; s++) data[p][s] = new Step();
                trackLen[p] = 16;
            }
        }

        public void demoBeat() {
            for (int p = 0; p < PADS; p++)
                for (int s = 0; s < MAX_STEPS; s++) data[p][s] = new Step();
            steps = 16;
            name = "PATTERN 1";
            set(0, 0, 118, true);
            set(0, 8, 105, false);
            set(0, 14, 90, false);
            set(1, 4, 110, false);
            set(1, 12, 110, true);
            for (int s = 0; s < 16; s += 2) set(15, s, (s % 4 == 0) ? 96 : 84, false);
            set(14, 14, 92, false);
            set(10, 12, 80, false);
            for (int s = 1; s < 16; s += 2) set(11, s, 42, false);
            set(0, 15, 88, false);
            data[0][15].ratchet = 3;
        }

        private void set(int p, int s, int v, boolean a) {
            data[p][s].on = true;
            data[p][s].velocity = v;
            data[p][s].accent = a;
        }
    }

    /**
     * 每声部参数。
     */
    public static class Pad {
        public float tune = 0.50f, decay = 0.50f, tone = 0.50f;
        public float snappy = 0.55f, tail = 0.45f;
        public float level = 0.75f, pan = 0.50f;
        public boolean mute = false, solo = false;

        /**
         * 供 UI 用名字读写参数；名字与规格书一致。
         */
        public float get(String key) {
            switch (key) {
                case "tune":
                    return tune;
                case "decay":
                    return decay;
                case "tone":
                    return tone;
                case "snappy":
                    return snappy;
                case "tail":
                    return tail;
                case "level":
                    return level;
                default:
                    return pan;
            }
        }

        public void set(String key, float v) {
            switch (key) {
                case "tune":
                    tune = v;
                    break;
                case "decay":
                    decay = v;
                    break;
                case "tone":
                    tone = v;
                    break;
                case "snappy":
                    snappy = v;
                    break;
                case "tail":
                    tail = v;
                    break;
                case "level":
                    level = v;
                    break;
                default:
                    pan = v;
                    break;
            }
        }
    }

    /**
     * 主效果。
     */
    public static class Master {
        public float volume = 0.80f, drive = 1.35f;
        public float lpCutoff = 18000f, lpQ = 0.90f;
        public float hpCutoff = 28f, hpQ = 0.70f;
        public boolean lpOn = true, hpOn = true;
        public float bpm = 120f, swing = 0f;
        public int beatsPerBar = 4, beatUnit = 4;
        public boolean songMode = false;
    }

    public static class SongEntry {
        public int pattern = 0, repeats = 1;

        public SongEntry() {
        }

        public SongEntry(int p, int r) {
            pattern = p;
            repeats = r;
        }
    }
}
