package native808;

import javax.swing.*;
import java.awt.*;
import java.awt.event.*;
import java.io.File;
import java.util.ArrayList;
import javax.swing.filechooser.FileNameExtensionFilter;

/**
 * Swing 自绘 808 界面。所有音序数据来自 Native808.snapshot() 的一次性快照，
 * 播放头/电平表每 33ms 刷新一次，UI 卡顿不会影响音频线程。
 */
public class MachinePanel extends JPanel implements MouseListener, MouseMotionListener,
        MouseWheelListener, KeyListener {

    static final int TRACKS = 16, STEPS = 64, PATTERNS = 256;
    static final String[] TRACK_NAMES = {
            "KICK", "SNARE", "CLAP", "RIM", "TOM LO", "TOM MID", "TOM HI", "CONGA",
            "COWBELL", "SHAKER", "TAMB", "CRASH", "RIDE", "HAT OPEN", "HAT CLOSED", "CLAVE"};
    static final String[] KNOB_NAMES = {"TUNE", "DECAY", "TONE", "CHAR", "LEVEL", "PAN"};
    static final String[] MASTER_NAMES = {"VOLUME", "DRIVE", "HP", "LP", "RESO", "ACCENT"};
    static final String[] COND_NAMES = {"ALWAYS", "1:2", "1:3", "1:4", "FILL", "NOT FILL", "EVERY N"};

    static final Color C_BG = new Color(14, 16, 20);
    static final Color C_PANEL = new Color(26, 29, 35);
    static final Color C_PANEL2 = new Color(36, 40, 48);
    static final Color C_PANEL3 = new Color(47, 52, 62);
    static final Color C_TEXT = new Color(228, 231, 237);
    static final Color C_DIM = new Color(139, 146, 160);
    static final Color C_ORANGE = new Color(255, 122, 48);
    static final Color C_ORANGE2 = new Color(122, 58, 24);
    static final Color C_GREEN = new Color(70, 196, 106);
    static final Color C_RED = new Color(229, 72, 77);
    static final Color C_BLUE = new Color(86, 156, 245);
    static final Color C_CELL = new Color(42, 46, 55);
    static final Color C_SELROW = new Color(45, 39, 34);

    private final long eng;
    private final AudioEngine808 audio;
    private final int[] meta = new int[7];
    private final float[] params = new float[103];
    private final byte[] steps = new byte[TRACKS * STEPS * 10];
    private final byte[] flags = new byte[TRACKS * 4];
    private final int[] ints = new int[6];
    private final float[] meters = new float[TRACKS];
    private final float[] misc = new float[3];
    private final byte[] dummy = new byte[256 * 4];
    private final Timer timer;
    private final Rectangle[] rcVK = new Rectangle[6];
    private final Rectangle[] rcMK = new Rectangle[6];
    private final Rectangle[] rcSS = new Rectangle[5];
    private final Rectangle[] rcPad = new Rectangle[16];
    private final Rectangle[] rcPQ = new Rectangle[16];
    private final Rectangle[] rcMute = new Rectangle[16];
    private final Rectangle[] rcSolo = new Rectangle[16];
    private final Rectangle[] rcMeter = new Rectangle[16];
    private final ArrayList<Long> taps = new ArrayList<>();
    private double scale = 1.0;
    private int selTrack = 0, selStep = 0, page = 0;
    private String msg = "READY";
    private String midiName = "MIDI: OFF";
    private int midiIndex = -1;
    private boolean exportBusy;
    private int frameNo;
    private int dragKind = -1; // 0=knob, 1=master, 2=slider, 3=bpm
    private int dragIdx;
    private int dragY;
    private double dragStart;
    private Rectangle rcPlay, rcStop, rcTap, rcBpmM, rcBpm, rcBpmP, rcSwing;
    private Rectangle rcPatPrev, rcPatDisp, rcPatNext, rcLenM, rcLenDisp, rcLenP;
    private Rectangle rcSong, rcSongEdit, rcSave, rcLoad, rcWav, rcMidi, rcStems, rcClear, rcCopy, rcMidiIn;
    private Rectangle rcSync, rcTimeSig, rcTrkLenM, rcTrkLenD, rcTrkLenP;
    private Rectangle rcGrid, rcGridSteps, rcPanel, rcMaster, rcPads, rcStrip, rcStatus;
    private Rectangle rcCond, rcAccent, rcTrigger;
    private Font fTitle, fLabel, fSmall, fTiny;
    private long lastTap;

    MachinePanel(long engine, AudioEngine808 audio) {
        this.eng = engine;
        this.audio = audio;
        setBackground(C_BG);
        setPreferredSize(new Dimension(1180, 700));
        setFocusable(true);
        addMouseListener(this);
        addMouseMotionListener(this);
        addMouseWheelListener(this);
        addKeyListener(this);
        midiInit();
        layoutAll();
        timer = new Timer(33, e -> tick());
        timer.start();
    }

    private static Color blend(Color a, Color b, float t) {
        return new Color(
                (int) (a.getRed() + (b.getRed() - a.getRed()) * t),
                (int) (a.getGreen() + (b.getGreen() - a.getGreen()) * t),
                (int) (a.getBlue() + (b.getBlue() - a.getBlue()) * t));
    }

    private static double clamp01(double v) {
        return v < 0 ? 0 : (v > 1 ? 1 : v);
    }

    void shutdown() {
        timer.stop();
    }

    private void midiInit() {
        int n = Native808.midiCount();
        if (n > 0 && Native808.midiStart(eng, 0) == 0) {
            midiIndex = 0;
            midiName = "MIDI: " + Native808.midiName(0);
        }
    }

    private void tick() {
        if (eng == 0) return;
        if (audio == null || !audio.isRunning()) {
            Native808.renderS16(eng, dummy, 256);
        }
        Native808.snapshot(eng, -1, meta, params, steps, flags);
        Native808.status(eng, ints, meters, misc);
        frameNo++;
        repaint();
    }

    // ------------------------------------------------------------------
    // 布局
    // ------------------------------------------------------------------
    private int S(double v) {
        return (int) Math.round(v * scale);
    }

    private Rectangle R(int x, int y, int w, int h) {
        return new Rectangle(S(x), S(y), S(w), S(h));
    }

    private void layoutAll() {
        double sx = getWidth() / 1280.0;
        double sy = getHeight() / 720.0;
        double ns = Math.max(0.35, Math.min(3.0, Math.min(sx, sy)));
        if (Math.abs(ns - scale) > 0.01) {
            scale = ns;
            updateFonts();
        }
        rcPlay = R(12, 8, 72, 30);
        rcStop = R(90, 8, 72, 30);
        rcTap = R(168, 8, 60, 30);
        rcBpmM = R(234, 8, 30, 30);
        rcBpm = R(268, 8, 86, 30);
        rcBpmP = R(358, 8, 30, 30);
        rcSwing = R(400, 8, 190, 30);
        rcPatPrev = R(600, 8, 30, 30);
        rcPatDisp = R(634, 8, 92, 30);
        rcPatNext = R(730, 8, 30, 30);
        rcLenM = R(772, 8, 30, 30);
        rcLenDisp = R(806, 8, 70, 30);
        rcLenP = R(880, 8, 30, 30);
        rcSong = R(922, 8, 72, 30);
        rcSongEdit = R(1000, 8, 84, 30);
        rcSave = R(12, 44, 70, 28);
        rcLoad = R(86, 44, 70, 28);
        rcWav = R(160, 44, 70, 28);
        rcMidi = R(234, 44, 70, 28);
        rcStems = R(308, 44, 76, 28);
        rcClear = R(388, 44, 64, 28);
        rcCopy = R(456, 44, 64, 28);
        rcMidiIn = R(524, 44, 240, 28);
        rcSync = R(772, 44, 76, 28);
        rcTimeSig = R(854, 44, 70, 28);
        rcTrkLenM = R(860, 486, 32, 26);
        rcTrkLenD = R(896, 486, 128, 26);
        rcTrkLenP = R(1028, 486, 32, 26);
        rcGrid = R(12, 84, 830, 432);
        rcPanel = R(852, 84, 416, 432);
        rcMaster = R(12, 526, 830, 106);
        rcPads = R(852, 526, 416, 106);
        rcStrip = R(12, 640, 1256, 40);
        rcStatus = R(0, 686, 1280, 34);
        rcGridSteps = new Rectangle(rcGrid.x + S(126), rcGrid.y + S(22),
                rcGrid.width - S(134), rcGrid.height - S(28));
        for (int i = 0; i < 6; i++) {
            int col = i % 3, row = i / 3;
            rcVK[i] = R(862 + col * 134, 118 + row * 80, 128, 78);
            rcMK[i] = R(30 + i * 134, 542, 128, 86);
        }
        for (int i = 0; i < 5; i++) rcSS[i] = R(860, 316 + i * 25, 400, 24);
        rcCond = R(860, 452, 210, 28);
        rcAccent = R(1080, 452, 180, 28);
        rcTrigger = R(1090, 94, 168, 24);
        for (int i = 0; i < 16; i++) {
            int col = i % 4, row = i / 4;
            rcPad[i] = R(858 + col * 102, 530 + row * 25, 98, 22);
            rcPQ[i] = R(34 + i * 76, 644, 70, 30);
        }
    }

    private void updateFonts() {
        int base = Math.max(7, S(13));
        fTitle = new Font("Segoe UI", Font.BOLD, Math.max(10, S(20)));
        fLabel = new Font("Segoe UI", Font.BOLD, base);
        fSmall = new Font("Segoe UI", Font.PLAIN, Math.max(7, S(11)));
        fTiny = new Font("Segoe UI", Font.PLAIN, Math.max(6, S(9)));
    }

    @Override
    public void addNotify() {
        super.addNotify();
        if (fTitle == null) {
            updateFonts();
            layoutAll();
        }
    }

    @Override
    public void setBounds(int x, int y, int w, int h) {
        super.setBounds(x, y, w, h);
        layoutAll();
        repaint();
    }

    // ------------------------------------------------------------------
    // 绘制工具
    // ------------------------------------------------------------------
    private void fillRect(Graphics2D g, Rectangle r, Color c) {
        g.setColor(c);
        g.fillRect(r.x, r.y, r.width, r.height);
    }

    private void text(Graphics2D g, Rectangle r, String s, Color c, Font f, int align) {
        g.setFont(f);
        g.setColor(c);
        FontMetrics fm = g.getFontMetrics();
        int x = r.x + S(4);
        if (align == SwingConstants.CENTER) x = r.x + (r.width - fm.stringWidth(s)) / 2;
        else if (align == SwingConstants.RIGHT) x = r.x + r.width - fm.stringWidth(s) - S(4);
        int y = r.y + (r.height + fm.getAscent() - fm.getDescent()) / 2;
        g.drawString(s, x, y);
    }

    private void button(Graphics2D g, Rectangle r, String s, boolean active, Font f) {
        g.setColor(active ? C_ORANGE : C_PANEL2);
        g.fillRoundRect(r.x, r.y, r.width, r.height, S(7), S(7));
        g.setColor(active ? C_ORANGE : C_PANEL3);
        g.drawRoundRect(r.x, r.y, r.width, r.height, S(7), S(7));
        text(g, r, s, active ? new Color(20, 16, 12) : C_TEXT, f, SwingConstants.CENTER);
    }

    private void knob(Graphics2D g, Rectangle r, String label, float v) {
        int rad = Math.max(S(7), Math.min(S(20), (r.height - S(38)) / 2));
        int cx = r.x + r.width / 2;
        int cy = r.y + S(4) + rad;
        g.setColor(C_PANEL3);
        g.fillOval(cx - rad, cy - rad, rad * 2, rad * 2);
        g.setStroke(new BasicStroke(Math.max(2f, S(4))));
        g.setColor(C_ORANGE2);
        g.drawOval(cx - rad, cy - rad, rad * 2, rad * 2);
        g.setStroke(new BasicStroke(Math.max(1.5f, S(2.5f))));
        g.setColor(C_ORANGE);
        g.drawArc(cx - rad - S(5), cy - rad - S(5), (rad + S(5)) * 2, (rad + S(5)) * 2,
                225, -270);
        double ang = Math.toRadians(135 + 270 * v);
        g.setColor(C_TEXT);
        g.drawLine(cx, cy, cx + (int) (Math.cos(ang) * (rad - S(4))),
                cy - (int) (Math.sin(ang) * (rad - S(4))));
        Rectangle lr = new Rectangle(r.x, cy + rad + S(1), r.width, S(14));
        text(g, lr, label, C_DIM, fTiny, SwingConstants.CENTER);
        Rectangle vr = new Rectangle(r.x, cy + rad + S(15), r.width, S(14));
        text(g, vr, String.valueOf(Math.round(v * 100)), C_TEXT, fSmall, SwingConstants.CENTER);
    }

    private void slider(Graphics2D g, Rectangle r, String label, double v, String val, boolean bipolar) {
        Rectangle lab = new Rectangle(r.x, r.y, S(72), r.height);
        Rectangle valr = new Rectangle(r.x + r.width - S(60), r.y, S(60), r.height);
        Rectangle bar = new Rectangle(r.x + S(78), r.y + S(7), r.width - S(138), r.height - S(14));
        text(g, lab, label, C_DIM, fSmall, SwingConstants.LEFT);
        text(g, valr, val, C_TEXT, fSmall, SwingConstants.RIGHT);
        fillRect(g, bar, C_PANEL3);
        g.setColor(C_ORANGE);
        if (bipolar) {
            int mid = bar.x + bar.width / 2;
            int x = bar.x + (int) (bar.width * v);
            g.fillRect(Math.min(mid, x), bar.y, Math.abs(x - mid), bar.height);
            g.setColor(C_DIM);
            g.drawLine(mid, bar.y - S(2), mid, bar.y + bar.height + S(2));
        } else {
            g.fillRect(bar.x, bar.y, (int) (bar.width * v), bar.height);
        }
    }

    // ------------------------------------------------------------------
    // 绘制
    // ------------------------------------------------------------------
    @Override
    protected void paintComponent(Graphics g0) {
        super.paintComponent(g0);
        Graphics2D g = (Graphics2D) g0;
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
        g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_ON);
        g.setColor(C_BG);
        g.fillRect(0, 0, getWidth(), getHeight());

        int pat = meta[2];
        int len = Math.max(1, Math.min(STEPS, meta[0] == 0 ? 16 : meta[0]));

        button(g, rcPlay, ints[0] != 0 ? "PLAYING" : "PLAY", ints[0] != 0, fLabel);
        button(g, rcStop, "STOP", false, fLabel);
        button(g, rcTap, "TAP", false, fLabel);
        button(g, rcBpmM, "-", false, fLabel);
        button(g, rcBpmP, "+", false, fLabel);
        fillRect(g, rcBpm, C_PANEL2);
        g.setColor(C_PANEL3);
        g.drawRoundRect(rcBpm.x, rcBpm.y, rcBpm.width, rcBpm.height, S(7), S(7));
        text(g, rcBpm, String.format("%.1f BPM", params[0]), C_ORANGE, fLabel, SwingConstants.CENTER);

        Rectangle swingTxt = new Rectangle(rcSwing.x, rcSwing.y, S(54), rcSwing.height);
        text(g, swingTxt, "SWING", C_DIM, fSmall, SwingConstants.LEFT);
        Rectangle swingBar = new Rectangle(rcSwing.x + S(54), rcSwing.y + S(9),
                S(136), rcSwing.height - S(18));
        fillRect(g, swingBar, C_PANEL3);
        g.setColor(C_ORANGE);
        g.fillRect(swingBar.x, swingBar.y, swingBar.width * meta[1] / 100, swingBar.height);
        text(g, swingTxt, meta[1] + "%", C_TEXT, fSmall, SwingConstants.RIGHT);

        button(g, rcPatPrev, "<", false, fLabel);
        button(g, rcPatNext, ">", false, fLabel);
        fillRect(g, rcPatDisp, C_PANEL2);
        text(g, rcPatDisp, String.format("PAT %03d", pat), C_TEXT, fLabel, SwingConstants.CENTER);
        button(g, rcLenM, "-", false, fLabel);
        button(g, rcLenP, "+", false, fLabel);
        fillRect(g, rcLenDisp, C_PANEL2);
        text(g, rcLenDisp, "LEN " + len, C_TEXT, fLabel, SwingConstants.CENTER);
        button(g, rcSong, meta[3] != 0 ? "SONG ON" : "SONG OFF", meta[3] != 0, fLabel);
        button(g, rcSongEdit, "EDIT SONG", false, fLabel);

        button(g, rcSave, "SAVE", false, fLabel);
        button(g, rcLoad, "LOAD", false, fLabel);
        button(g, rcWav, "WAV", false, fLabel);
        button(g, rcMidi, "MIDI", false, fLabel);
        button(g, rcStems, "STEMS", false, fLabel);
        button(g, rcClear, "CLEAR", false, fLabel);
        button(g, rcCopy, "COPY>", false, fLabel);
        fillRect(g, rcMidiIn, C_PANEL2);
        text(g, rcMidiIn, midiName, C_DIM, fSmall, SwingConstants.LEFT);
        boolean clockMidi = Native808.clockSource(eng) != 0;
        button(g, rcSync, clockMidi ? "SYNC MIDI" : "SYNC INT", clockMidi, fSmall);
        button(g, rcTimeSig, meta[5] + "/" + meta[6], false, fSmall);

        // 网格
        fillRect(g, rcGrid, C_PANEL);
        text(g, new Rectangle(rcGrid.x + S(6), rcGrid.y + S(2), S(110), S(18)), "TRACK",
                C_DIM, fTiny, SwingConstants.LEFT);
        int rowH = rcGridSteps.height / 16;
        int colW = rcGridSteps.width / len;
        for (int s = 0; s < len; s += 4) {
            int x = rcGridSteps.x + s * colW + colW / 2;
            g.setFont(fTiny);
            g.setColor(C_DIM);
            FontMetrics fm = g.getFontMetrics();
            String t = String.valueOf(s + 1);
            g.drawString(t, x - fm.stringWidth(t) / 2, rcGrid.y + S(16));
        }
        for (int t = 0; t < 16; t++) {
            int y = rcGridSteps.y + t * rowH;
            fillRect(g, new Rectangle(rcGrid.x, y, rcGridSteps.x - rcGrid.x, rowH),
                    t == selTrack ? C_SELROW : ((t & 1) != 0 ? new Color(24, 27, 33) : new Color(22, 25, 30)));
            text(g, new Rectangle(rcGrid.x + S(4), y, S(66), rowH), TRACK_NAMES[t],
                    t == selTrack ? C_ORANGE : C_TEXT, fTiny, SwingConstants.LEFT);
            rcMute[t] = new Rectangle(rcGrid.x + S(72), y + S(2), S(22), rowH - S(4));
            rcSolo[t] = new Rectangle(rcGrid.x + S(96), y + S(2), S(22), rowH - S(4));
            rcMeter[t] = new Rectangle(rcGrid.x + S(120), y + S(2), S(4), rowH - S(4));
            fillRect(g, rcMute[t], (flags[t * 4] != 0) ? C_RED : C_PANEL3);
            text(g, rcMute[t], "M", flags[t * 4] != 0 ? C_TEXT : C_DIM, fTiny, SwingConstants.CENTER);
            fillRect(g, rcSolo[t], (flags[t * 4 + 1] != 0) ? C_GREEN : C_PANEL3);
            text(g, rcSolo[t], "S", flags[t * 4 + 1] != 0 ? C_TEXT : C_DIM, fTiny, SwingConstants.CENTER);
            int mh = (int) (rcMeter[t].height * Math.min(1.0f, meters[t]));
            fillRect(g, rcMeter[t], C_PANEL3);
            if (mh > 0) {
                g.setColor(meters[t] > 0.9f ? C_RED : (meters[t] > 0.6f ? C_ORANGE : C_GREEN));
                g.fillRect(rcMeter[t].x, rcMeter[t].y + rcMeter[t].height - mh, rcMeter[t].width, mh);
            }
            int tlen = Math.max(1, Math.min(len, flags[t * 4 + 2] == 0 ? len : flags[t * 4 + 2]));
            int playStep = ints[0] != 0 ? ints[2] % tlen : -1;
            for (int s = 0; s < len; s++) {
                int idx = (t * STEPS + s) * 10;
                boolean on = steps[idx] != 0;
                int vel = steps[idx + 1] & 0xff;
                boolean acc = steps[idx + 2] != 0;
                int prob = steps[idx + 3] & 0xff;
                int rat = steps[idx + 4] & 0xff;
                int micro = (short) ((steps[idx + 8] & 0xff) | ((steps[idx + 9] & 0xff) << 8));
                Rectangle cell = new Rectangle(rcGridSteps.x + s * colW + S(1), y + S(2),
                        colW - S(2), rowH - S(4));
                Color c = C_CELL;
                if (on) {
                    c = blend(C_ORANGE2, C_ORANGE, vel / 127.0f);
                    if (acc) c = new Color(255, 196, 120);
                }
                fillRect(g, cell, c);
                if (on && prob < 100) fillRect(g, new Rectangle(cell.x + S(2), cell.y + S(2), S(3), S(3)), C_BLUE);
                if (on && rat > 1) {
                    for (int k = 0; k < Math.min(4, rat); k++)
                        fillRect(g, new Rectangle(cell.x + S(3) + k * S(3), cell.y + cell.height - S(5), S(2), S(3)), C_TEXT);
                }
                if (on && micro != 0)
                    fillRect(g, new Rectangle(cell.x + cell.width - S(4), cell.y, S(3), S(4)),
                            micro > 0 ? C_GREEN : C_RED);
                if (t == selTrack && s == selStep) {
                    g.setColor(C_TEXT);
                    g.drawRect(cell.x, cell.y, cell.width, cell.height);
                }
                if (s == playStep) {
                    g.setColor(C_BLUE);
                    g.drawRect(cell.x, cell.y, cell.width, cell.height);
                }
            }
        }

        // 声部面板
        fillRect(g, rcPanel, C_PANEL);
        text(g, new Rectangle(rcPanel.x + S(12), rcPanel.y + S(7), rcPanel.width - S(190), S(20)),
                "VOICE EDIT  ·  " + TRACK_NAMES[selTrack], C_ORANGE, fLabel, SwingConstants.LEFT);
        button(g, rcTrigger, "TRIGGER", false, fSmall);
        for (int i = 0; i < 6; i++) knob(g, rcVK[i], KNOB_NAMES[i], params[7 + selTrack * 6 + i]);

        text(g, new Rectangle(rcPanel.x + S(10), rcPanel.y + S(200), rcPanel.width - S(20), S(20)),
                String.format("STEP %02d  ·  %s", selStep + 1, TRACK_NAMES[selTrack]), C_TEXT, fLabel,
                SwingConstants.LEFT);
        int idx = (selTrack * STEPS + selStep) * 10;
        int vel = steps[idx + 1] & 0xff;
        int prob = steps[idx + 3] & 0xff;
        int rat = Math.max(1, steps[idx + 4] & 0xff);
        int rate = steps[idx + 5] & 0xff;
        int cond = steps[idx + 6] & 0xff;
        int micro = (short) ((steps[idx + 8] & 0xff) | ((steps[idx + 9] & 0xff) << 8));
        slider(g, rcSS[0], "VELOCITY", (vel - 1) / 126.0, String.valueOf(vel), false);
        slider(g, rcSS[1], "PROB", prob / 100.0, prob + "%", false);
        slider(g, rcSS[2], "RATCHET", (rat - 1) / 7.0, "x" + rat, false);
        slider(g, rcSS[3], "RATE", (rate - 10) / 90.0, rate + "%", false);
        slider(g, rcSS[4], "MICRO", micro / 4800.0 + 0.5, String.format("%+d smp", micro), true);
        button(g, rcCond, "COND: " + COND_NAMES[Math.min(6, cond)], cond != 0, fSmall);
        button(g, rcAccent, (steps[idx + 2] != 0) ? "ACCENT ON" : "ACCENT", steps[idx + 2] != 0, fSmall);
        button(g, rcTrkLenM, "-", false, fSmall);
        button(g, rcTrkLenD, "TRACK LEN " + Math.max(1, flags[selTrack * 4 + 2]), false, fSmall);
        button(g, rcTrkLenP, "+", false, fSmall);

        // 主控
        fillRect(g, rcMaster, C_PANEL);
        text(g, new Rectangle(rcMaster.x + S(12), rcMaster.y + S(2), rcMaster.width - S(20), S(16)),
                "MASTER  ·  FILTER / SATURATION / ACCENT", C_DIM, fTiny, SwingConstants.LEFT);
        for (int i = 0; i < 6; i++) knob(g, rcMK[i], MASTER_NAMES[i], params[1 + i]);

        // 打击垫
        fillRect(g, rcPads, C_PANEL);
        for (int i = 0; i < 16; i++) {
            boolean sel = i == selTrack;
            fillRect(g, rcPad[i], sel ? C_ORANGE2 : C_PANEL2);
            g.setColor(sel ? C_ORANGE : C_PANEL3);
            g.drawRoundRect(rcPad[i].x, rcPad[i].y, rcPad[i].width, rcPad[i].height, S(4), S(4));
            text(g, rcPad[i], TRACK_NAMES[i], sel ? C_TEXT : C_DIM, fTiny, SwingConstants.CENTER);
        }

        // pattern 快捷条
        fillRect(g, rcStrip, C_PANEL);
        button(g, R(14, 644, 16, 30), "<", false, fTiny);
        button(g, R(1252, 644, 16, 30), ">", false, fTiny);
        for (int i = 0; i < 16; i++) {
            int p = page * 16 + i;
            button(g, rcPQ[i], String.valueOf(p), p == pat, fSmall);
        }

        // 状态栏
        fillRect(g, rcStatus, C_PANEL);
        String status = String.format("%s · CPU %d%% · XRUN %d · %s · %s · %d/%d%s",
                audio != null ? audio.info() : "NO AUDIO", Math.round(misc[0] * 100), ints[5],
                msg, ints[0] != 0 ? "PLAY" : "STOP", meta[5], meta[6],
                exportBusy ? " · EXPORTING..." : "");
        text(g, new Rectangle(rcStatus.x + S(12), rcStatus.y, rcStatus.width - S(24), rcStatus.height),
                status, (audio != null && audio.isRunning()) ? C_DIM : C_RED, fSmall, SwingConstants.LEFT);
    }

    // ------------------------------------------------------------------
    // 鼠标 / 键盘
    // ------------------------------------------------------------------
    @Override
    public void mousePressed(MouseEvent e) {
        requestFocusInWindow();
        if (SwingUtilities.isRightMouseButton(e)) {
            int[] gs = gridAt(e.getX(), e.getY());
            if (gs != null) {
                selTrack = gs[0];
                selStep = gs[1];
                repaint();
            }
            return;
        }
        int x = e.getX(), y = e.getY();
        int pat = meta[2];
        int curIdx = (selTrack * STEPS + selStep) * 10;
        if (rcPlay.contains(x, y)) {
            if (ints[0] != 0) Native808.stop(eng);
            else Native808.play(eng);
            return;
        }
        if (rcStop.contains(x, y)) {
            Native808.stopReset(eng);
            return;
        }
        if (rcTap.contains(x, y)) {
            tapTempo();
            return;
        }
        if (rcBpmM.contains(x, y)) {
            Native808.setBpm(eng, Math.max(20, params[0] - 1));
            return;
        }
        if (rcBpmP.contains(x, y)) {
            Native808.setBpm(eng, Math.min(400, params[0] + 1));
            return;
        }
        if (rcBpm.contains(x, y)) {
            dragKind = 3;
            dragY = y;
            dragStart = params[0];
            return;
        }
        if (rcSwing.contains(x, y)) {
            double t = (x - (rcSwing.x + S(54))) / (double) Math.max(1, rcSwing.width - S(54));
            Native808.setSwing(eng, pat, (int) Math.max(0, Math.min(100, t * 100)));
            return;
        }
        if (rcPatPrev.contains(x, y)) {
            Native808.selectPattern(eng, (pat + PATTERNS - 1) % PATTERNS);
            return;
        }
        if (rcPatNext.contains(x, y)) {
            Native808.selectPattern(eng, (pat + 1) % PATTERNS);
            return;
        }
        if (rcLenM.contains(x, y)) {
            Native808.setPatternLen(eng, pat, meta[0] - 1);
            return;
        }
        if (rcLenP.contains(x, y)) {
            Native808.setPatternLen(eng, pat, meta[0] + 1);
            return;
        }
        if (rcSong.contains(x, y)) {
            Native808.setSongOn(eng, meta[3] != 0 ? 0 : 1);
            return;
        }
        if (rcSongEdit.contains(x, y)) {
            openSongEditor();
            return;
        }
        if (rcSave.contains(x, y)) {
            saveProject();
            return;
        }
        if (rcLoad.contains(x, y)) {
            loadProject();
            return;
        }
        if (rcWav.contains(x, y)) {
            exportFile(0);
            return;
        }
        if (rcMidi.contains(x, y)) {
            exportFile(1);
            return;
        }
        if (rcStems.contains(x, y)) {
            exportFile(2);
            return;
        }
        if (rcClear.contains(x, y)) {
            Native808.clearPattern(eng, pat);
            return;
        }
        if (rcCopy.contains(x, y)) {
            int dst = (pat + 1) % PATTERNS;
            Native808.copyPattern(eng, pat, dst);
            Native808.selectPattern(eng, dst);
            msg = "COPIED " + pat + " -> " + dst;
            return;
        }
        if (rcMidiIn.contains(x, y)) {
            cycleMidi();
            return;
        }
        if (rcSync.contains(x, y)) {
            Native808.setClock(eng, Native808.clockSource(eng) != 0 ? 0 : 1);
            return;
        }
        if (rcTimeSig.contains(x, y)) {
            int[][] sigs = {{4, 4}, {3, 4}, {5, 4}, {7, 8}, {6, 8}, {12, 8}};
            int next = 0;
            for (int i = 0; i < sigs.length; i++)
                if (sigs[i][0] == meta[5] && sigs[i][1] == meta[6]) {
                    next = (i + 1) % sigs.length;
                    break;
                }
            Native808.setTimesig(eng, sigs[next][0], sigs[next][1]);
            return;
        }
        for (int i = 0; i < 16; i++) {
            if (rcPQ[i].contains(x, y)) {
                Native808.selectPattern(eng, page * 16 + i);
                return;
            }
        }
        if (x >= S(14) && x <= S(30) && y >= S(644) && y <= S(674)) {
            if (page > 0) page--;
            return;
        }
        if (x >= S(1252) && x <= S(1268) && y >= S(644) && y <= S(674)) {
            if (page < PATTERNS / 16 - 1) page++;
            return;
        }
        for (int i = 0; i < 6; i++) {
            if (rcVK[i].contains(x, y)) {
                double v = clamp01((rcVK[i].y + rcVK[i].height - S(6) - y) /
                        (double) (rcVK[i].height - S(20)));
                Native808.setTrackParam(eng, selTrack, i, (float) v);
                dragKind = 0;
                dragIdx = i;
                dragY = y;
                dragStart = v;
                return;
            }
            if (rcMK[i].contains(x, y)) {
                double v = clamp01((rcMK[i].y + rcMK[i].height - S(6) - y) /
                        (double) (rcMK[i].height - S(20)));
                Native808.setMaster(eng, i, (float) v);
                dragKind = 1;
                dragIdx = i;
                dragY = y;
                dragStart = v;
                return;
            }
        }
        for (int i = 0; i < 5; i++) {
            if (rcSS[i].contains(x, y)) {
                dragKind = 2;
                dragIdx = i;
                setStepSlider(i, x);
                return;
            }
        }
        if (rcCond.contains(x, y)) {
            int c = (steps[curIdx + 6] + 1) % 7;
            int n = Math.max(2, steps[curIdx + 7]);
            Native808.setCond(eng, pat, selTrack, selStep, c, n);
            return;
        }
        if (rcAccent.contains(x, y)) {
            Native808.setStep(eng, pat, selTrack, selStep, 2, steps[curIdx + 2] != 0 ? 0 : 1);
            return;
        }
        if (rcTrkLenM.contains(x, y)) {
            Native808.setTrackLen(eng, selTrack, Math.max(1, flags[selTrack * 4 + 2] - 1));
            return;
        }
        if (rcTrkLenP.contains(x, y)) {
            Native808.setTrackLen(eng, selTrack, Math.min(64, flags[selTrack * 4 + 2] + 1));
            return;
        }
        if (rcTrigger.contains(x, y)) {
            Native808.noteOn(eng, selTrack, 112);
            return;
        }
        for (int i = 0; i < 16; i++) {
            if (rcPad[i].contains(x, y)) {
                Native808.noteOn(eng, i, 112);
                selTrack = i;
                return;
            }
        }
        for (int t = 0; t < 16; t++) {
            if (rcMute[t] != null && rcMute[t].contains(x, y)) {
                Native808.setMute(eng, t, flags[t * 4] != 0 ? 0 : 1);
                return;
            }
            if (rcSolo[t] != null && rcSolo[t].contains(x, y)) {
                Native808.setSolo(eng, t, flags[t * 4 + 1] != 0 ? 0 : 1);
                return;
            }
        }
        int[] gs = gridAt(x, y);
        if (gs != null) {
            selTrack = gs[0];
            selStep = gs[1];
            int i2 = (selTrack * STEPS + selStep) * 10;
            if (e.isControlDown()) {
                Native808.setStep(eng, pat, selTrack, selStep, 2, steps[i2 + 2] != 0 ? 0 : 1);
            } else if (e.isShiftDown()) {
                cycleProb(selTrack, selStep);
            } else if (e.isAltDown()) {
                int r = (steps[i2 + 4] & 0xff) + 1;
                if (r > 8) r = 1;
                Native808.setStep(eng, pat, selTrack, selStep, 4, r);
            } else {
                Native808.setStep(eng, pat, selTrack, selStep, 0, steps[i2] != 0 ? 0 : 1);
            }
        }
    }

    private int[] gridAt(int x, int y) {
        if (!rcGridSteps.contains(x, y)) return null;
        int len = Math.max(1, meta[0]);
        int rowH = rcGridSteps.height / 16;
        int colW = rcGridSteps.width / len;
        int t = (y - rcGridSteps.y) / rowH;
        int s = (x - rcGridSteps.x) / colW;
        if (t < 0 || t >= 16 || s < 0 || s >= len) return null;
        return new int[]{t, s};
    }

    private void cycleProb(int t, int s) {
        int[] seq = {100, 75, 50, 25, 10, 0};
        int idx = (t * STEPS + s) * 10 + 3;
        int cur = steps[idx] & 0xff;
        int next = 100;
        for (int i = 0; i < seq.length; i++)
            if (seq[i] == cur) {
                next = seq[(i + 1) % seq.length];
                break;
            }
        Native808.setStep(eng, meta[2], t, s, 3, next);
    }

    @Override
    public void mouseDragged(MouseEvent e) {
        if (dragKind < 0) return;
        if (dragKind == 3) {
            double delta = (dragY - e.getY()) / (double) S(110);
            Native808.setBpm(eng, (float) Math.max(20, Math.min(400, dragStart + delta * 120)));
        } else if (dragKind == 2) {
            setStepSlider(dragIdx, e.getX());
        } else {
            double delta = (dragY - e.getY()) / (double) S(110);
            double v = clamp01(dragStart + delta);
            if (dragKind == 0) Native808.setTrackParam(eng, selTrack, dragIdx, (float) v);
            else Native808.setMaster(eng, dragIdx, (float) v);
        }
    }

    @Override
    public void mouseReleased(MouseEvent e) {
        dragKind = -1;
    }

    @Override
    public void mouseMoved(MouseEvent e) {
    }

    @Override
    public void mouseClicked(MouseEvent e) {
    }

    @Override
    public void mouseEntered(MouseEvent e) {
    }

    @Override
    public void mouseExited(MouseEvent e) {
    }

    @Override
    public void mouseWheelMoved(MouseWheelEvent e) {
        int[] gs = gridAt(e.getX(), e.getY());
        if (gs != null) {
            int idx = (gs[0] * STEPS + gs[1]) * 10 + 1;
            int v = (steps[idx] & 0xff) + (e.getWheelRotation() < 0 ? 4 : -4);
            Native808.setStep(eng, meta[2], gs[0], gs[1], 1, Math.max(1, Math.min(127, v)));
        } else if (rcBpm.contains(e.getX(), e.getY())) {
            Native808.setBpm(eng, Math.max(20, Math.min(400, params[0] +
                    (e.getWheelRotation() < 0 ? 1 : -1))));
        }
    }

    private void setStepSlider(int idx, int x) {
        int barX = rcSS[idx].x + S(78);
        int barW = rcSS[idx].width - S(138);
        double t = clamp01((x - barX) / (double) Math.max(1, barW));
        int pat = meta[2];
        switch (idx) {
            case 0:
                Native808.setStep(eng, pat, selTrack, selStep, 1, (int) (1 + t * 126));
                break;
            case 1:
                Native808.setStep(eng, pat, selTrack, selStep, 3, (int) (t * 100));
                break;
            case 2:
                Native808.setStep(eng, pat, selTrack, selStep, 4, (int) (1 + t * 7));
                break;
            case 3:
                Native808.setStep(eng, pat, selTrack, selStep, 5, (int) (10 + t * 90));
                break;
            default:
                Native808.setStep(eng, pat, selTrack, selStep, 8, (int) ((t - 0.5) * 4800));
                break;
        }
    }

    @Override
    public void keyPressed(KeyEvent e) {
        if (e.getKeyCode() == KeyEvent.VK_SPACE) {
            if (ints[0] != 0) Native808.stop(eng);
            else Native808.play(eng);
            return;
        }
        if (e.getKeyCode() == KeyEvent.VK_LEFT) {
            Native808.selectPattern(eng, (meta[2] + PATTERNS - 1) % PATTERNS);
            return;
        }
        if (e.getKeyCode() == KeyEvent.VK_RIGHT) {
            Native808.selectPattern(eng, (meta[2] + 1) % PATTERNS);
            return;
        }
        if (e.getKeyCode() == KeyEvent.VK_T) {
            tapTempo();
            return;
        }
        String s = KeyEvent.getKeyText(e.getKeyCode()).toLowerCase();
        String keys = "1234qwerasdfzxcv";
        int i = keys.indexOf(s);
        if (i >= 0 && s.length() == 1) {
            Native808.noteOn(eng, i, 112);
            selTrack = i;
        }
    }

    @Override
    public void keyReleased(KeyEvent e) {
    }

    @Override
    public void keyTyped(KeyEvent e) {
    }

    private void tapTempo() {
        long now = System.currentTimeMillis();
        if (!taps.isEmpty() && now - lastTap > 2000) taps.clear();
        if (!taps.isEmpty()) {
            long dt = now - lastTap;
            if (dt > 150 && dt < 1500) {
                taps.add(dt);
                if (taps.size() > 8) taps.remove(0);
            }
        }
        lastTap = now;
        if (taps.size() >= 2) {
            long sum = 0;
            for (long d : taps) sum += d;
            double bpm = 60000.0 / ((double) sum / taps.size());
            Native808.setBpm(eng, (float) Math.max(20, Math.min(400, bpm)));
            msg = String.format("TAP TEMPO %.1f BPM", bpm);
        }
    }

    // ------------------------------------------------------------------
    // 文件 / 导出 / MIDI / Song
    // ------------------------------------------------------------------
    private void saveProject() {
        JFileChooser fc = new JFileChooser();
        fc.setFileFilter(new FileNameExtensionFilter("808 Project (*.808)", "808"));
        fc.setSelectedFile(new File("808-project.808"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        File f = fc.getSelectedFile();
        if (!f.getName().toLowerCase().endsWith(".808")) f = new File(f.getAbsolutePath() + ".808");
        int rc = Native808.save(eng, f.getAbsolutePath());
        msg = rc == 0 ? "SAVED: " + f.getName() : "SAVE FAILED " + rc;
    }

    private void loadProject() {
        JFileChooser fc = new JFileChooser();
        fc.setFileFilter(new FileNameExtensionFilter("808 Project (*.808)", "808"));
        if (fc.showOpenDialog(this) != JFileChooser.APPROVE_OPTION) return;
        int rc = Native808.load(eng, fc.getSelectedFile().getAbsolutePath());
        msg = rc == 0 ? "LOADED: " + fc.getSelectedFile().getName() : "LOAD FAILED " + rc;
    }

    private void exportFile(int kind) {
        if (exportBusy) {
            msg = "EXPORT ALREADY RUNNING";
            return;
        }
        JFileChooser fc = new JFileChooser();
        String def = kind == 1 ? "808-pattern.mid" : (kind == 2 ? "808-stems.wav" : "808-pattern.wav");
        fc.setSelectedFile(new File(def));
        fc.setFileFilter(new FileNameExtensionFilter(kind == 1 ? "MIDI (*.mid)" : "WAV (*.wav)",
                kind == 1 ? "mid" : "wav"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        final File f = fc.getSelectedFile();
        final int mode = meta[3] != 0 ? 1 : 0;
        exportBusy = true;
        msg = "EXPORTING...";
        new Thread(() -> {
            int rc = kind == 1 ? Native808.exportMidi(eng, f.getAbsolutePath(), mode, 2)
                    : Native808.exportWav(eng, f.getAbsolutePath(), mode, 2, kind == 2 ? 1 : 0);
            final int r = rc;
            SwingUtilities.invokeLater(() -> {
                exportBusy = false;
                msg = r == 0 ? "EXPORT COMPLETE" : "EXPORT FAILED " + r;
                repaint();
            });
        }, "808-export").start();
    }

    private void cycleMidi() {
        int n = Native808.midiCount();
        if (n <= 0) {
            midiName = "MIDI: NONE";
            msg = "NO MIDI INPUT DEVICE";
            return;
        }
        midiIndex++;
        if (midiIndex >= n) {
            midiIndex = -1;
            Native808.midiStop(eng);
            midiName = "MIDI: OFF";
            msg = "MIDI INPUT DISABLED";
            return;
        }
        if (Native808.midiStart(eng, midiIndex) == 0) {
            midiName = "MIDI: " + Native808.midiName(midiIndex);
            msg = "MIDI INPUT: " + Native808.midiName(midiIndex);
        } else {
            midiName = "MIDI: ERROR";
        }
    }

    private void openSongEditor() {
        JDialog dlg = new JDialog(SwingUtilities.getWindowAncestor(this), "SONG CHAIN EDITOR",
                Dialog.ModalityType.APPLICATION_MODAL);
        dlg.setSize(560, 520);
        dlg.setLocationRelativeTo(this);
        DefaultListModel<String> model = new DefaultListModel<>();
        ArrayList<int[]> items = new ArrayList<>();
        int n = Native808.songLen(eng);
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++)
            items.add(new int[]{Native808.songPat(eng, i),
                    Math.max(1, Native808.songReps(eng, i))});
        JList<String> list = new JList<>(model);
        list.setBackground(C_PANEL2);
        list.setForeground(C_TEXT);
        list.setSelectionBackground(C_ORANGE);
        Runnable refresh = () -> {
            model.clear();
            for (int i = 0; i < items.size(); i++)
                model.addElement(String.format("%02d   PATTERN %03d   x%d", i + 1,
                        items.get(i)[0], items.get(i)[1]));
        };
        refresh.run();
        dlg.setLayout(new BorderLayout(8, 8));
        dlg.getContentPane().setBackground(C_PANEL);
        dlg.add(new JScrollPane(list), BorderLayout.CENTER);
        JPanel bar = new JPanel(new GridLayout(2, 4, 4, 4));
        bar.setBackground(C_PANEL);
        String[] labels = {"+ ADD", "- DEL", "UP", "DOWN", "PAT -", "PAT +", "REP -", "REP +"};
        for (int i = 0; i < labels.length; i++) {
            final int k = i;
            JButton b = new JButton(labels[i]);
            b.addActionListener(ev -> {
                int sel = list.getSelectedIndex();
                switch (k) {
                    case 0:
                        items.add(new int[]{items.isEmpty() ? 0 : items.get(items.size() - 1)[0], 4});
                        break;
                    case 1:
                        if (sel >= 0) items.remove(sel);
                        break;
                    case 2:
                        if (sel > 0) {
                            int[] t = items.get(sel);
                            items.set(sel, items.get(sel - 1));
                            items.set(sel - 1, t);
                        }
                        break;
                    case 3:
                        if (sel >= 0 && sel < items.size() - 1) {
                            int[] t = items.get(sel);
                            items.set(sel, items.get(sel + 1));
                            items.set(sel + 1, t);
                        }
                        break;
                    case 4:
                        if (sel >= 0) items.get(sel)[0] = (items.get(sel)[0] + PATTERNS - 1) % PATTERNS;
                        break;
                    case 5:
                        if (sel >= 0) items.get(sel)[0] = (items.get(sel)[0] + 1) % PATTERNS;
                        break;
                    case 6:
                        if (sel >= 0) items.get(sel)[1] = Math.max(1, items.get(sel)[1] - 1);
                        break;
                    default:
                        if (sel >= 0) items.get(sel)[1] = Math.min(64, items.get(sel)[1] + 1);
                        break;
                }
                refresh.run();
            });
            bar.add(b);
        }
        JPanel south = new JPanel(new BorderLayout());
        south.setBackground(C_PANEL);
        south.add(bar, BorderLayout.NORTH);
        JPanel bottom = new JPanel(new FlowLayout(FlowLayout.LEFT));
        bottom.setBackground(C_PANEL);
        JButton apply = new JButton("APPLY & CLOSE");
        JButton cancel = new JButton("CANCEL");
        bottom.add(apply);
        bottom.add(cancel);
        south.add(bottom, BorderLayout.SOUTH);
        dlg.add(south, BorderLayout.SOUTH);
        apply.addActionListener(ev -> {
            Native808.setSongLen(eng, items.size());
            for (int i = 0; i < items.size(); i++)
                Native808.setSongEntry(eng, i, items.get(i)[0], items.get(i)[1]);
            dlg.dispose();
        });
        cancel.addActionListener(ev -> dlg.dispose());
        dlg.setVisible(true);
    }
}
