// ============================================================================
//  XuUi.java —— Java Swing 界面
//
//  徐某把界面做得尽量接近硬件 808：
//    顶部走带条、16×N 步进网格、16 条通道条、主效果与电平表。
//  Swing 的 EDT 只负责画界面；音频在 XuAudio 的独立线程里跑。
//  所有定时器都只刷新 UI，绝不参与音序。
// ============================================================================
package xu808;

import javax.swing.*;
import javax.swing.border.*;
import javax.sound.midi.*;
import java.awt.*;
import java.awt.event.*;
import java.io.File;
import java.util.*;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedQueue;

public class XuUi extends JFrame {
    // ---- 颜色 ----
    static final Color BG = new Color(18, 18, 18);
    static final Color PANEL = new Color(28, 28, 28);
    static final Color PANEL2 = new Color(35, 35, 35);
    static final Color LINE = new Color(46, 46, 46);
    static final Color ORANGE = new Color(255, 122, 24);
    static final Color ORANGED = new Color(168, 74, 12);
    static final Color YELLOW = new Color(255, 209, 102);
    static final Color CYAN = new Color(76, 201, 240);
    static final Color TEXT = new Color(232, 232, 232);
    static final Color DIM = new Color(138, 138, 138);
    static final Color RED = new Color(229, 72, 77);

    private final XuProject proj;
    private final XuEngine eng;
    private final ArrayList<Strip> strips = new ArrayList<>();
    // MIDI 输入队列：接收线程写，EDT 读
    private final ConcurrentLinkedQueue<int[]> midiQ = new ConcurrentLinkedQueue<>();
    // Tap tempo
    private final ArrayList<Long> taps = new ArrayList<>();
    private XuAudio audio;
    private GridPanel grid;
    private MeterPanel meter;
    private JLabel lbStatus, lbMidi;
    private JComboBox<String> cbPattern;
    private JSpinner spBpm, spSteps;
    private JSlider slBpm, slSwing;
    private JLabel lbSwing;
    private javax.swing.Timer timer;
    private MidiDevice midiDev;
    private Receiver midiReceiver;

    public XuUi(XuProject p) {
        proj = p;
        eng = new XuEngine(p);
        setTitle("XU-808 鼓机 - 徐某 (Java)");
        setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        setSize(1520, 920);
        getContentPane().setBackground(BG);
        setLayout(new BorderLayout());
        buildUi();
        startAudio();
        startMidi();
        timer = new javax.swing.Timer(33, e -> onTick());   // ≈30fps，只刷新 UI
        timer.start();
        addWindowListener(new WindowAdapter() {
            @Override
            public void windowClosing(WindowEvent e) {
                eng.stop();
                if (audio != null) audio.stop();
                stopMidi();
                if (timer != null) timer.stop();
            }
        });
        setFocusable(true);
        addKeyListener(new KeyAdapter() {
            @Override
            public void keyPressed(KeyEvent e) {
                if (e.getKeyCode() == KeyEvent.VK_SPACE) {
                    eng.toggle();
                    return;
                }
                if (e.getKeyCode() >= KeyEvent.VK_1 && e.getKeyCode() <= KeyEvent.VK_8) {
                    cbPattern.setSelectedIndex(e.getKeyCode() - KeyEvent.VK_1);
                    return;
                }
                String keys = "ZXCVBNMASDFGHJKL";
                char c = Character.toUpperCase(e.getKeyChar());
                int idx = keys.indexOf(c);
                if (idx >= 0) eng.triggerPad(idx, 110, false);
            }
        });
    }

    private static JButton btn(String text, int w) {
        JButton b = new JButton(text);
        b.setPreferredSize(new Dimension(w, 40));
        b.setBackground(PANEL2);
        b.setForeground(TEXT);
        b.setFocusPainted(false);
        b.setBorder(BorderFactory.createLineBorder(LINE));
        return b;
    }

    private static JLabel label(String text, Color c) {
        JLabel l = new JLabel(text);
        l.setForeground(c);
        return l;
    }

    private static List<String[]> paramsFor(int pad) {
        List<String[]> l = new ArrayList<>();
        if (pad == 0) {
            l.add(new String[]{"TUNE", "tune"});
            l.add(new String[]{"DECAY", "decay"});
            l.add(new String[]{"TONE", "tone"});
        } else if (pad == 1) {
            l.add(new String[]{"TUNE", "tune"});
            l.add(new String[]{"SNAP", "snappy"});
            l.add(new String[]{"DECAY", "decay"});
        } else if (pad >= 2 && pad <= 4) {
            l.add(new String[]{"TUNE", "tune"});
            l.add(new String[]{"DECAY", "decay"});
            l.add(new String[]{"COLOR", "tone"});
        } else if (pad == 10) {
            l.add(new String[]{"DECAY", "decay"});
            l.add(new String[]{"TONE", "tone"});
            l.add(new String[]{"TAIL", "tail"});
        } else if (pad == 11) {
            l.add(new String[]{"DECAY", "decay"});
            l.add(new String[]{"TONE", "tone"});
        } else if (pad == 12) {
            l.add(new String[]{"TUNE", "tune"});
            l.add(new String[]{"DECAY", "decay"});
        } else if (pad >= 13) {
            l.add(new String[]{"DECAY", "decay"});
            l.add(new String[]{"TONE", "tone"});
        } else {
            l.add(new String[]{"TUNE", "tune"});
            l.add(new String[]{"DECAY", "decay"});
        }
        return l;
    }

    // ------------------------------------------------------------------
    // 构建界面
    // ------------------------------------------------------------------
    private void buildUi() {
        JPanel top = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 4));
        top.setBackground(PANEL);
        top.setPreferredSize(new Dimension(0, 68));

        JPanel logoBox = new JPanel();
        logoBox.setOpaque(false);
        logoBox.setLayout(new BoxLayout(logoBox, BoxLayout.Y_AXIS));
        logoBox.setPreferredSize(new Dimension(190, 56));
        JLabel logo = new JLabel("XU-808");
        logo.setForeground(ORANGE);
        logo.setFont(new Font("Consolas", Font.BOLD, 24));
        JLabel sub = new JLabel("RHYTHM COMPOSER · 徐某 · 16 VOICE");
        sub.setForeground(DIM);
        sub.setFont(new Font("Microsoft YaHei", Font.PLAIN, 8));
        logoBox.add(logo);
        logoBox.add(sub);
        top.add(logoBox);

        JButton bPlay = btn("▶ PLAY", 88);
        JButton bStop = btn("■ STOP", 88);
        bPlay.addActionListener(e -> eng.play());
        bStop.addActionListener(e -> {
            eng.stop();
            grid.setPlayhead(-1);
        });
        top.add(bPlay);
        top.add(bStop);

        top.add(label("BPM", DIM));
        spBpm = new JSpinner(new SpinnerNumberModel((double) proj.master.bpm, 40.0, 300.0, 1.0));
        spBpm.setPreferredSize(new Dimension(70, 30));
        spBpm.addChangeListener(e -> {
            Object v = spBpm.getValue();
            double d = v instanceof Number ? ((Number) v).doubleValue() : 120;
            proj.master.bpm = (float) d;
            slBpm.setValue((int) d);
        });
        top.add(spBpm);
        slBpm = new JSlider(40, 300, (int) proj.master.bpm);
        slBpm.setPreferredSize(new Dimension(100, 30));
        slBpm.addChangeListener(e -> spBpm.setValue((double) slBpm.getValue()));
        top.add(slBpm);

        JButton bTap = btn("TAP", 48);
        bTap.addActionListener(e -> onTap());
        top.add(bTap);

        top.add(label("SWING", DIM));
        slSwing = new JSlider(0, 75, (int) (proj.master.swing * 100));
        slSwing.setPreferredSize(new Dimension(100, 30));
        lbSwing = label(slSwing.getValue() + "%", ORANGE);
        slSwing.addChangeListener(e -> {
            proj.master.swing = slSwing.getValue() / 100f;
            lbSwing.setText(slSwing.getValue() + "%");
        });
        top.add(slSwing);
        top.add(lbSwing);

        top.add(label("PATTERN", DIM));
        cbPattern = new JComboBox<>();
        for (int i = 0; i < XuProject.PATTERNS; i++)
            cbPattern.addItem(String.format("%02d %s", i + 1, proj.patterns[i].name));
        cbPattern.setSelectedIndex(proj.current);
        cbPattern.setPreferredSize(new Dimension(150, 30));
        cbPattern.addActionListener(e -> {
            if (e.getSource() != cbPattern) return;
            eng.requestPattern(cbPattern.getSelectedIndex());
            spSteps.setValue(proj.cur().steps);
            grid.rebuild();
            syncStrips();
        });
        top.add(cbPattern);

        spSteps = new JSpinner(new SpinnerNumberModel(proj.cur().steps, 1, 64, 1));
        spSteps.setPreferredSize(new Dimension(56, 30));
        spSteps.addChangeListener(e -> {
            proj.cur().steps = (int) spSteps.getValue();
            grid.rebuild();
        });
        top.add(spSteps);

        JCheckBox cbSong = new JCheckBox("SONG");
        cbSong.setForeground(TEXT);
        cbSong.setOpaque(false);
        cbSong.setSelected(proj.master.songMode);
        cbSong.addActionListener(e -> proj.master.songMode = cbSong.isSelected());
        top.add(cbSong);

        JButton bSong = btn("歌曲链…", 90);
        bSong.addActionListener(e -> editSong());
        top.add(bSong);

        JButton bSave = btn("保存", 66);
        JButton bLoad = btn("打开", 66);
        JButton bWav = btn("导出WAV", 86);
        JButton bMidi = btn("导出MIDI", 92);
        bSave.addActionListener(e -> saveProject());
        bLoad.addActionListener(e -> loadProject());
        bWav.addActionListener(e -> exportWav());
        bMidi.addActionListener(e -> exportMidi());
        top.add(bSave);
        top.add(bLoad);
        top.add(bWav);
        top.add(bMidi);

        lbMidi = label("MIDI: -", DIM);
        top.add(lbMidi);
        add(top, BorderLayout.NORTH);

        // 中部：网格 + 通道条 + 主效果
        JPanel center = new JPanel();
        center.setBackground(BG);
        center.setLayout(new BoxLayout(center, BoxLayout.Y_AXIS));

        grid = new GridPanel();
        JScrollPane gs = new JScrollPane(grid);
        gs.setPreferredSize(new Dimension(1500, 400));
        gs.setMinimumSize(new Dimension(600, 300));
        gs.setBorder(null);
        gs.setBackground(BG);
        center.add(gs);

        JPanel stripHolder = new JPanel(new FlowLayout(FlowLayout.LEFT, 2, 2));
        stripHolder.setBackground(BG);
        for (int i = 0; i < XuProject.PADS; i++) {
            Strip s = new Strip(i);
            strips.add(s);
            stripHolder.add(s);
        }
        JScrollPane ss = new JScrollPane(stripHolder);
        ss.setPreferredSize(new Dimension(1500, 330));
        ss.setBorder(null);
        ss.setBackground(BG);
        ss.getVerticalScrollBar().setUnitIncrement(16);
        center.add(ss);

        center.add(buildMaster());
        add(center, BorderLayout.CENTER);
    }

    private JPanel buildMaster() {
        JPanel mg = new JPanel(new FlowLayout(FlowLayout.LEFT, 10, 6));
        mg.setBackground(PANEL);
        mg.setPreferredSize(new Dimension(0, 96));
        mg.setBorder(BorderFactory.createTitledBorder(
                BorderFactory.createLineBorder(LINE), "MASTER · 徐某的母线",
                0, 0, new Font("Microsoft YaHei", Font.PLAIN, 10), ORANGE));

        addMaster(mg, "LP 低通", 200, 20000, (int) proj.master.lpCutoff, v -> proj.master.lpCutoff = v);
        addMaster(mg, "RES 共鸣", 50, 800, (int) (proj.master.lpQ * 100), v -> proj.master.lpQ = v / 100f);
        addMaster(mg, "HP 高通", 10, 800, (int) proj.master.hpCutoff, v -> proj.master.hpCutoff = v);
        addMaster(mg, "DRIVE", 100, 800, (int) (proj.master.drive * 100), v -> proj.master.drive = v / 100f);
        addMaster(mg, "VOL", 0, 100, (int) (proj.master.volume * 100), v -> proj.master.volume = v / 100f);
        meter = new MeterPanel();
        mg.add(meter);
        lbStatus = label("CPU 0% · XRUN 0", DIM);
        mg.add(lbStatus);

        JCheckBox cbClock = new JCheckBox("MIDI CLK");
        cbClock.setOpaque(false);
        cbClock.setForeground(TEXT);
        cbClock.addActionListener(e -> eng.clockExternal = cbClock.isSelected());
        mg.add(cbClock);
        return mg;
    }

    private void addMaster(JPanel host, String label, int lo, int hi, int val, IntSetter setter) {
        JPanel box = new JPanel();
        box.setOpaque(false);
        box.setLayout(new BoxLayout(box, BoxLayout.Y_AXIS));
        box.setPreferredSize(new Dimension(130, 76));
        JLabel l = label(label, DIM);
        l.setFont(new Font("Microsoft YaHei", Font.PLAIN, 9));
        JSlider s = new JSlider(lo, hi, Math.max(lo, Math.min(hi, val)));
        s.setBackground(PANEL);
        s.setPreferredSize(new Dimension(124, 26));
        JLabel v = label(String.valueOf(val), ORANGE);
        v.setFont(new Font("Microsoft YaHei", Font.PLAIN, 9));
        s.addChangeListener(e -> {
            setter.set(s.getValue());
            v.setText(String.valueOf(s.getValue()));
        });
        box.add(l);
        box.add(s);
        box.add(v);
        host.add(box);
    }

    private void syncStrips() {
        for (Strip s : strips) s.sync();
    }

    // ------------------------------------------------------------------
    // 运行期刷新
    // ------------------------------------------------------------------
    private void onTick() {
        grid.setPlayhead(eng.playing ? eng.getStepIndex() : -1);
        meter.setLevels(eng.meterL, eng.meterR);
        lbStatus.setText(String.format("CPU %.1f%% · XRUN %d", eng.cpu * 100, eng.xruns));
        int[] m;
        while ((m = midiQ.poll()) != null) handleMidi(m);
        if (!eng.playing && cbPattern.getSelectedIndex() != proj.current) {
            cbPattern.setSelectedIndex(proj.current);
        }
    }

    private void onTap() {
        long now = System.nanoTime();
        if (!taps.isEmpty() && now - taps.get(taps.size() - 1) > 2_000_000_000L) taps.clear();
        taps.add(now);
        if (taps.size() > 8) taps.remove(0);
        if (taps.size() >= 2) {
            ArrayList<Double> iv = new ArrayList<>();
            for (int i = 1; i < taps.size(); i++) {
                double d = (taps.get(i) - taps.get(i - 1)) / 1e9;
                if (d > 0.15 && d < 2.0) iv.add(d);
            }
            if (iv.size() >= 2) {
                iv.remove(Collections.max(iv));
                iv.remove(Collections.min(iv));
            }
            if (!iv.isEmpty()) {
                double avg = 0;
                for (double d : iv) avg += d;
                avg /= iv.size();
                spBpm.setValue(Math.max(40.0, Math.min(300.0, 60.0 / avg)));
            }
        }
    }

    // ------------------------------------------------------------------
    // MIDI
    // ------------------------------------------------------------------
    private void startMidi() {
        try {
            MidiDevice.Info[] infos = MidiSystem.getMidiDeviceInfo();
            for (MidiDevice.Info info : infos) {
                MidiDevice d = MidiSystem.getMidiDevice(info);
                if (d.getMaxTransmitters() != 0) {
                    d.open();
                    Transmitter tr = d.getTransmitter();
                    midiReceiver = new Receiver() {
                        @Override
                        public void send(MidiMessage msg, long timeStamp) {
                            byte[] b = msg.getMessage();
                            if (b.length >= 3)
                                midiQ.add(new int[]{b[0] & 0xFF, b[1] & 0xFF, b[2] & 0xFF});
                        }

                        @Override
                        public void close() {
                        }
                    };
                    tr.setReceiver(midiReceiver);
                    midiDev = d;
                    lbMidi.setText("MIDI: " + info.getName());
                    lbMidi.setForeground(ORANGE);
                    return;
                }
            }
            lbMidi.setText("MIDI: 无设备");
        } catch (Exception ex) {
            lbMidi.setText("MIDI: 不可用");
        }
    }

    private void stopMidi() {
        try {
            if (midiDev != null) {
                midiDev.close();
                midiDev = null;
            }
        } catch (Exception ignored) {
        }
    }

    private void handleMidi(int[] m) {
        int cmd = m[0] & 0xF0;
        if (cmd == 0x90 && m[2] > 0) {
            for (int p = 0; p < XuProject.PADS; p++)
                if (XuProject.MIDI_NOTES[p] == m[1]) {
                    eng.triggerPad(p, m[2], m[2] >= 120);
                    break;
                }
        } else if (cmd == 0xB0) {
            if (m[1] == 7) proj.master.volume = m[2] / 127f;
            else if (m[1] == 74) proj.master.lpCutoff = (float) (20.0 * Math.pow(1000.0, m[2] / 127.0));
        } else if (cmd == 0xC0) {
            cbPattern.setSelectedIndex(m[1] % XuProject.PATTERNS);
        } else if (m[0] == 0xFA) eng.play();
        else if (m[0] == 0xFC) eng.stop();
        else if (m[0] == 0xF8 && eng.clockExternal) eng.externalTick();
    }

    // ------------------------------------------------------------------
    // 文件
    // ------------------------------------------------------------------
    private void saveProject() {
        JFileChooser fc = new JFileChooser();
        fc.setDialogTitle("保存工程");
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            proj.save(fc.getSelectedFile().getAbsolutePath());
            JOptionPane.showMessageDialog(this, "已保存：" + fc.getSelectedFile());
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, "保存失败：" + e.getMessage(), "错误", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void loadProject() {
        JFileChooser fc = new JFileChooser();
        fc.setDialogTitle("打开工程");
        if (fc.showOpenDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            XuProject np = XuProject.load(fc.getSelectedFile().getAbsolutePath());
            eng.stop();
            // 把新工程内容拷贝进现有对象，避免替换引用导致界面持有旧对象
            proj.name = np.name;
            proj.seed = np.seed;
            proj.current = np.current;
            proj.patterns = np.patterns;
            proj.pads = np.pads;
            proj.master = np.master;
            proj.song = np.song;
            eng.proj = proj;
            cbPattern.removeAllItems();
            for (int i = 0; i < XuProject.PATTERNS; i++)
                cbPattern.addItem(String.format("%02d %s", i + 1, proj.patterns[i].name));
            cbPattern.setSelectedIndex(proj.current);
            spSteps.setValue(proj.cur().steps);
            grid.rebuild();
            syncStrips();
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, "加载失败：" + e.getMessage(), "错误", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void exportWav() {
        JFileChooser fc = new JFileChooser();
        fc.setDialogTitle("导出 WAV");
        fc.setSelectedFile(new File("xu808.wav"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            setCursor(Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR));
            eng.exportWav(fc.getSelectedFile().getAbsolutePath(), proj.master.songMode);
            lbStatus.setText("WAV 已导出");
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, "导出失败：" + e.getMessage(), "错误", JOptionPane.ERROR_MESSAGE);
        } finally {
            setCursor(Cursor.getDefaultCursor());
        }
    }

    private void exportMidi() {
        JFileChooser fc = new JFileChooser();
        fc.setDialogTitle("导出 MIDI");
        fc.setSelectedFile(new File("xu808.mid"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            eng.exportMidi(fc.getSelectedFile().getAbsolutePath(), proj.master.songMode);
            lbStatus.setText("MIDI 已导出");
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, "导出失败：" + e.getMessage(), "错误", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void editSong() {
        StringBuilder sb = new StringBuilder();
        for (XuProject.SongEntry e : proj.song) sb.append(e.pattern + 1).append(',').append(e.repeats).append('\n');
        String s = JOptionPane.showInputDialog(this,
                "歌曲链：每行“Pattern号,重复次数”", sb.toString());
        if (s == null) return;
        proj.song.clear();
        for (String line : s.split("\\R")) {
            line = line.trim();
            if (line.isEmpty()) continue;
            String[] parts = line.split(",");
            try {
                int p = Integer.parseInt(parts[0].trim()) - 1;
                int r = parts.length > 1 ? Integer.parseInt(parts[1].trim()) : 1;
                proj.song.add(new XuProject.SongEntry(
                        Math.max(0, Math.min(15, p)), Math.max(1, r)));
            } catch (NumberFormatException ignored) {
            }
        }
    }

    private void editStep(int pad, int step) {
        XuProject.Step st = proj.cur().data[pad][step];
        JPanel form = new JPanel(new GridLayout(0, 2, 6, 4));
        form.setBackground(PANEL);
        JCheckBox on = new JCheckBox("启用", st.on);
        JCheckBox acc = new JCheckBox("重音 Accent", st.accent);
        JSpinner vel = new JSpinner(new SpinnerNumberModel(st.velocity, 1, 127, 1));
        JSpinner prob = new JSpinner(new SpinnerNumberModel(st.prob, 0, 100, 1));
        JSpinner mic = new JSpinner(new SpinnerNumberModel(st.micro, -2400, 2400, 10));
        JSpinner rat = new JSpinner(new SpinnerNumberModel(st.ratchet, 1, 8, 1));
        JComboBox<String> cond = new JComboBox<>(XuProject.COND_NAMES);
        cond.setSelectedIndex(Math.max(0, Math.min(4, st.cond)));
        for (JComponent c : new JComponent[]{on, acc}) c.setOpaque(false);
        form.add(on);
        form.add(acc);
        form.add(new JLabel("力度 Velocity"));
        form.add(vel);
        form.add(new JLabel("概率 Probability %"));
        form.add(prob);
        form.add(new JLabel("微时 Micro（样本）"));
        form.add(mic);
        form.add(new JLabel("滚奏 Ratchet"));
        form.add(rat);
        form.add(new JLabel("条件触发"));
        form.add(cond);
        int r = JOptionPane.showConfirmDialog(this, form, "步进属性 - 徐某的 808",
                JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE);
        if (r == JOptionPane.OK_OPTION) {
            st.on = on.isSelected();
            st.accent = acc.isSelected();
            st.velocity = (int) vel.getValue();
            st.prob = (int) prob.getValue();
            st.micro = (int) mic.getValue();
            st.ratchet = (int) rat.getValue();
            st.cond = cond.getSelectedIndex();
            grid.repaint();
        }
    }

    private void startAudio() {
        audio = new XuAudio(eng);
        if (!audio.start()) {
            lbStatus.setText("无音频设备（仍可离线导出）");
        }
    }

    private interface IntSetter {
        void set(int v);
    }

    // ==================================================================
    // 电平表
    // ==================================================================
    private static class MeterPanel extends JPanel {
        float l = 0, r = 0;

        MeterPanel() {
            setPreferredSize(new Dimension(150, 24));
            setBackground(new Color(12, 12, 12));
        }

        void setLevels(float a, float b) {
            l = a;
            r = b;
            repaint();
        }

        @Override
        protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            for (int i = 0; i < 2; i++) {
                float v = Math.max(0, Math.min(1, i == 0 ? l : r));
                int y = 3 + i * 9;
                g.setColor(new Color(26, 26, 26));
                g.fillRect(2, y, getWidth() - 4, 7);
                int w = (int) ((getWidth() - 4) * v);
                if (w > 0) {
                    g.setColor(v > 0.9 ? RED : v > 0.7 ? YELLOW : new Color(61, 220, 132));
                    g.fillRect(2, y, w, 7);
                }
            }
            g.setColor(LINE);
            g.drawRect(0, 0, getWidth() - 1, getHeight() - 1);
        }
    }

    // ==================================================================
    // 网格
    // ==================================================================
    private class GridPanel extends JPanel {
        static final int CELL_H = 22, LABEL_W = 132, HEAD_H = 24;
        int playhead = -1;
        int cellW = 32;

        GridPanel() {
            setBackground(BG);
            addMouseListener(new MouseAdapter() {
                @Override
                public void mousePressed(MouseEvent e) {
                    int x = e.getX(), y = e.getY();
                    if (y < HEAD_H) return;
                    int r = (y - HEAD_H) / CELL_H;
                    if (r < 0 || r >= 16) return;
                    if (x < LABEL_W) {
                        for (int j = 0; j < 2; j++) {
                            int bx = 92 + j * 18;
                            if (x >= bx && x <= bx + 15) {
                                if (j == 0) proj.pads[r].mute = !proj.pads[r].mute;
                                else proj.pads[r].solo = !proj.pads[r].solo;
                                syncStrips();
                                repaint();
                                return;
                            }
                        }
                        eng.triggerPad(r, 112, false);
                        return;
                    }
                    int c = (x - LABEL_W) / cellW;
                    if (c < 0 || c >= proj.cur().steps) return;
                    if (e.getButton() == MouseEvent.BUTTON3) {
                        editStep(r, c);
                    } else {
                        XuProject.Step st = proj.cur().data[r][c];
                        st.on = !st.on;
                        if (st.on) eng.triggerPad(r, st.velocity, st.accent);
                        repaint();
                    }
                }
            });
        }

        void rebuild() {
            int steps = Math.max(1, proj.cur().steps);
            int w = 1500;
            cellW = Math.max(10, Math.min(32, (w - LABEL_W) / steps));
            setPreferredSize(new Dimension(LABEL_W + steps * cellW + 20,
                    HEAD_H + 16 * CELL_H + 8));
            revalidate();
            repaint();
        }

        void setPlayhead(int s) {
            if (s != playhead) {
                playhead = s;
                repaint();
            }
        }

        @Override
        protected void paintComponent(Graphics g0) {
            super.paintComponent(g0);
            Graphics2D g = (Graphics2D) g0;
            int steps = Math.max(1, proj.cur().steps);
            g.setColor(BG);
            g.fillRect(0, 0, getWidth(), getHeight());
            g.setColor(PANEL);
            g.fillRect(0, 0, getWidth(), HEAD_H);
            g.setFont(new Font("Segoe UI", Font.PLAIN, 8));
            for (int c = 0; c < steps; c++) {
                int x = LABEL_W + c * cellW;
                if (c % 4 == 0) {
                    g.setColor(new Color(58, 58, 58));
                    g.fillRect(x, 0, 1, getHeight());
                }
                g.setColor(DIM);
                g.drawString(String.valueOf(c + 1), x + cellW / 2 - 4, 16);
            }
            g.setColor(PANEL2);
            g.fillRect(0, HEAD_H, LABEL_W, getHeight() - HEAD_H);
            for (int r = 0; r < 16; r++) {
                int y = HEAD_H + r * CELL_H;
                g.setColor((r % 2 == 0) ? new Color(22, 22, 22) : new Color(26, 26, 26));
                g.fillRect(LABEL_W, y, getWidth() - LABEL_W, CELL_H);
                g.setFont(new Font("Consolas", Font.BOLD, 10));
                g.setColor(ORANGE);
                g.drawString(XuProject.CODES[r], 6, y + 15);
                g.setFont(new Font("Microsoft YaHei", Font.PLAIN, 8));
                g.setColor(TEXT);
                g.drawString(XuProject.NAMES_CN[r], 42, y + 15);
                XuProject.Pad pd = proj.pads[r];
                for (int j = 0; j < 2; j++) {
                    boolean on = j == 0 ? pd.mute : pd.solo;
                    int bx = 92 + j * 18;
                    g.setColor(on ? (j == 0 ? RED : YELLOW) : new Color(42, 42, 42));
                    g.fillRect(bx, y + 4, 15, CELL_H - 8);
                    g.setColor(on ? new Color(18, 18, 18) : DIM);
                    g.setFont(new Font("Segoe UI", Font.BOLD, 7));
                    g.drawString(j == 0 ? "M" : "S", bx + 5, y + 14);
                }
                g.setColor(LINE);
                g.drawLine(0, y + CELL_H - 1, getWidth(), y + CELL_H - 1);
            }
            for (int c = 0; c < steps; c++) {
                int x = LABEL_W + c * cellW;
                for (int r = 0; r < 16; r++) {
                    XuProject.Step st = proj.cur().data[r][c];
                    int y = HEAD_H + r * CELL_H;
                    if (st.on) {
                        Color base = st.accent ? YELLOW : ORANGE;
                        int a = 110 + (int) (145 * (st.velocity / 127.0));
                        g.setColor(new Color(base.getRed(), base.getGreen(), base.getBlue(), a));
                        g.fillRect(x + 2, y + 2, cellW - 4, CELL_H - 4);
                        if (st.ratchet > 1 || st.prob < 100) {
                            g.setColor(new Color(26, 16, 8));
                            g.setFont(new Font("Consolas", Font.BOLD, 7));
                            g.drawString(st.ratchet > 1 ? "R" : String.valueOf(st.prob),
                                    x + cellW / 2 - 3, y + 15);
                        }
                    } else {
                        g.setColor(new Color(16, 16, 16));
                        g.fillRect(x + 2, y + 2, cellW - 4, CELL_H - 4);
                        g.setColor(new Color(39, 39, 39));
                        g.drawRect(x + 2, y + 2, cellW - 5, CELL_H - 5);
                    }
                }
            }
            if (playhead >= 0 && playhead < steps) {
                int x = LABEL_W + playhead * cellW;
                g.setColor(new Color(76, 201, 240, 26));
                g.fillRect(x, HEAD_H, cellW, getHeight() - HEAD_H);
                g.setColor(CYAN);
                g.drawLine(x, HEAD_H, x, getHeight());
                g.drawLine(x + cellW, HEAD_H, x + cellW, getHeight());
            }
            g.setColor(LINE);
            g.drawRect(0, 0, getWidth() - 1, getHeight() - 1);
        }
    }

    // ==================================================================
    // 通道条
    // ==================================================================
    private class Strip extends JPanel {
        final int pad;
        final LinkedHashMap<String, JSlider> params = new LinkedHashMap<>();
        JToggleButton bm, bs;
        JSpinner len;
        boolean updating = false;

        Strip(int p) {
            pad = p;
            setBackground(BG);
            setPreferredSize(new Dimension(76, 300));
            setLayout(new BoxLayout(this, BoxLayout.Y_AXIS));
            JButton name = new JButton(XuProject.CODES[p] + " " + XuProject.NAMES_CN[p]);
            name.setFont(new Font("Microsoft YaHei", Font.PLAIN, 9));
            name.setForeground(ORANGE);
            name.setBackground(PANEL2);
            name.setMaximumSize(new Dimension(74, 32));
            name.addActionListener(e -> eng.triggerPad(pad, 112, false));
            add(name);
            JPanel ms = new JPanel(new FlowLayout(FlowLayout.LEFT, 1, 0));
            ms.setOpaque(false);
            bm = new JToggleButton("M");
            bs = new JToggleButton("S");
            for (JToggleButton b : new JToggleButton[]{bm, bs}) {
                b.setPreferredSize(new Dimension(30, 18));
                b.setFont(new Font("Segoe UI", Font.BOLD, 8));
                b.setBackground(PANEL2);
                b.setForeground(TEXT);
            }
            bm.setSelected(proj.pads[pad].mute);
            bs.setSelected(proj.pads[pad].solo);
            bm.addActionListener(e -> {
                proj.pads[pad].mute = bm.isSelected();
                grid.repaint();
            });
            bs.addActionListener(e -> {
                proj.pads[pad].solo = bs.isSelected();
                grid.repaint();
            });
            ms.add(bm);
            ms.add(bs);
            add(ms);

            for (String[] d : paramsFor(pad)) addParam(d[0], d[1]);
            addParam("LEVEL", "level");
            addParam("PAN", "pan");
            len = new JSpinner(new SpinnerNumberModel(proj.cur().trackLen[pad], 1, 64, 1));
            len.setMaximumSize(new Dimension(66, 20));
            len.addChangeListener(e -> proj.cur().trackLen[pad] = (int) len.getValue());
            add(len);
            add(Box.createVerticalGlue());
        }

        void addParam(String lbl, String key) {
            JLabel l = new JLabel(lbl);
            l.setForeground(DIM);
            l.setFont(new Font("Microsoft YaHei", Font.PLAIN, 7));
            l.setAlignmentX(CENTER_ALIGNMENT);
            add(l);
            int val = (int) (proj.pads[pad].get(key) * 100);
            JSlider s = new JSlider(JSlider.VERTICAL, 0, 100, val);
            s.setBackground(BG);
            s.setPreferredSize(new Dimension(18, 42));
            s.setMaximumSize(new Dimension(18, 42));
            s.setAlignmentX(CENTER_ALIGNMENT);
            s.addChangeListener(e -> {
                if (updating) return;
                proj.pads[pad].set(key, s.getValue() / 100f);
            });
            add(s);
            params.put(key, s);
        }

        void sync() {
            updating = true;
            for (Map.Entry<String, JSlider> e : params.entrySet())
                e.getValue().setValue((int) (proj.pads[pad].get(e.getKey()) * 100));
            bm.setSelected(proj.pads[pad].mute);
            bs.setSelected(proj.pads[pad].solo);
            len.setValue(proj.cur().trackLen[pad]);
            updating = false;
        }
    }
}
