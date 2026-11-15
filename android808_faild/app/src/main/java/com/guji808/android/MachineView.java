package com.guji808.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.Locale;

/**
 * 808 触摸界面：全部 Canvas 自绘，布局以 dp 为单位（屏幕高度约 360dp 起）。
 * 数据来自 core808 的一次性快照 + 状态读取，音频渲染始终在 AudioEngine808 线程。
 */
public class MachineView extends View {

    static final String[] TRACK_NAMES = {
            "KICK", "SNARE", "CLAP", "RIM", "TOM LO", "TOM MID", "TOM HI", "CONGA",
            "COWBELL", "SHAKER", "TAMB", "CRASH", "RIDE", "HAT OPEN", "HAT CLOSED", "CLAVE"};
    static final String[] PAD_NAMES = {
            "KICK", "SNARE", "CLAP", "RIM", "TL", "TM", "TH", "CONGA",
            "COW", "SHAK", "TAMB", "CRSH", "RIDE", "H-OP", "H-CL", "CLAVE"};
    static final String[] KNOB_NAMES = {"TUNE", "DECAY", "TONE", "CHAR", "LEVEL", "PAN"};
    static final String[] MASTER_NAMES = {"VOLUME", "DRIVE", "HP", "LP", "RESO", "ACCENT"};
    static final String[] COND_NAMES = {"ALWAYS", "1:2", "1:3", "1:4", "FILL", "NOTFILL", "EVERY N"};
    static final int[][] TIME_SIGS = {{4, 4}, {3, 4}, {5, 4}, {7, 8}, {6, 8}, {12, 8}};

    static final int TRACKS = 16, STEPS = 64, PATTERNS = 256;

    static final int C_BG = 0xFF0E1014;
    static final int C_PANEL = 0xFF1A1D23;
    static final int C_PANEL2 = 0xFF242830;
    static final int C_PANEL3 = 0xFF2F3440;
    static final int C_TEXT = 0xFFE4E7ED;
    static final int C_DIM = 0xFF8B92A0;
    static final int C_ORANGE = 0xFFFF7A30;
    static final int C_ORANGE2 = 0xFF7A3A18;
    static final int C_ORANGE3 = 0xFFFFC478;
    static final int C_GREEN = 0xFF46C46A;
    static final int C_RED = 0xFFE5484D;
    static final int C_BLUE = 0xFF569CF5;
    static final int C_CELL = 0xFF2A2E37;
    static final int C_SELROW = 0xFF2D2722;
    static final int C_DARKTEXT = 0xFF140F0A;

    private static final int OV_NONE = 0, OV_SONG = 1, OV_FILES = 2;
    private static final int LP_NONE = 0, LP_GRID = 1, LP_CLEAR = 2;

    private final long engine;
    private final AudioEngine808 audio;
    private final MidiInput midi;
    private final File projectDir;
    private final File exportDir;

    /* 快照 */
    private final int[] meta = new int[7];
    private final float[] params = new float[103];
    private final byte[] steps = new byte[TRACKS * STEPS * 10];
    private final byte[] flags = new byte[TRACKS * 4];
    private final int[] stInts = new int[6];
    private final float[] meters = new float[TRACKS];
    private final float[] misc = new float[3];
    private final ArrayList<int[]> song = new ArrayList<>();
    private final ArrayList<File> files = new ArrayList<>();
    private final java.util.concurrent.atomic.AtomicInteger jobs =
            new java.util.concurrent.atomic.AtomicInteger();
    private final RectF[] top = new RectF[17];
    private final RectF[] row2 = new RectF[8];
    private final RectF rcGrid = new RectF(), rcSteps = new RectF(), rcPanel = new RectF();
    private final RectF rcMaster = new RectF(), rcPads = new RectF();
    private final RectF rcStrip = new RectF(), rcStatus = new RectF();
    private final RectF[] rcName = new RectF[TRACKS];
    private final RectF[] rcMute = new RectF[TRACKS];
    private final RectF[] rcSolo = new RectF[TRACKS];
    private final RectF[] rcMeter = new RectF[TRACKS];
    private final RectF[] rcPad = new RectF[TRACKS];
    private final RectF[] rcPQ = new RectF[16];
    private final RectF[] rcVK = new RectF[6];
    private final RectF[] rcMK = new RectF[6];
    private final RectF[] rcSS = new RectF[5];
    private final RectF rcStepPrev = new RectF(), rcStepPage = new RectF(), rcStepNext = new RectF();
    private final RectF rcPagePrev = new RectF(), rcPageNext = new RectF();
    private final RectF rcCond = new RectF(), rcCondN = new RectF();
    private final RectF rcAccent = new RectF(), rcTrigger = new RectF();
    private final RectF rcTLenDec = new RectF(), rcTLenDisp = new RectF();
    private final RectF rcTLenInc = new RectF();
    private final RectF rcOvPanel = new RectF(), rcOvList = new RectF();
    private final RectF[] ovBtn = new RectF[10];
    /* 画笔 */
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Typeface tfBold = Typeface.create("sans-serif-condensed", Typeface.BOLD);
    private final Typeface tfNorm = Typeface.create("sans-serif-condensed", Typeface.NORMAL);
    private final Typeface tfMono = Typeface.create("monospace", Typeface.BOLD);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ArrayList<Long> taps = new ArrayList<>();
    private String statusLine = "";
    private int frameNo;
    /* 选择状态 */
    private int selTrack;
    private int selStep;
    private int stepPage;
    private int patPage;
    private int clockSource;
    private boolean fitSteps;
    private long pageOverrideUntil;
    /* 覆盖层 */
    private int overlay = OV_NONE;
    private int selSong;
    private int songScroll;
    /* 拖动 */
    private int dragKind; // 1=voice knob 2=master 3=slider 4=bpm 5=swing
    private int dragIdx;
    private float dragStartY, dragStartV;
    private float downX, downY;
    private int longPressKind = LP_NONE;
    private int lpTrack, lpStep;
    private boolean longPressFired;
    private boolean exportBusy;
    private volatile String msg = "READY";
    private final Runnable longPressRunnable = new Runnable() {
        @Override
        public void run() {
            if (longPressKind == LP_GRID) {
                toggleAccent(lpTrack, lpStep);
                longPressFired = true;
            } else if (longPressKind == LP_CLEAR) {
                Native808.clearPattern(engine, meta[2]);
                msg = "PATTERN " + meta[2] + " CLEARED";
                longPressFired = true;
            }
        }
    };
    /* 布局 */
    private float scale = 1f, baseW = 640f, baseH = 360f;
    private float ovRowH = 20f;
    private int ovRows = 8;
    private float ovTouchY;
    private int ovTouchScroll = -1;
    private int ovPendingRow = -1;
    private int ovPendingIndex = -1;
    private boolean ovMoved;
    private boolean framesRunning;
    private final Choreographer.FrameCallback frameCb = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            if (!framesRunning) return;
            updateEngine();
            invalidate();
            Choreographer.getInstance().postFrameCallback(this);
        }
    };
    private boolean attached;
    private long lastTap;

    public MachineView(Context ctx, long engine, AudioEngine808 audio, MidiInput midi,
                       File projectDir, File exportDir) {
        super(ctx);
        this.engine = engine;
        this.audio = audio;
        this.midi = midi;
        this.projectDir = projectDir;
        this.exportDir = exportDir;
        setBackgroundColor(C_BG);
        setFocusable(true);
        setKeepScreenOn(true);
        for (int i = 0; i < top.length; i++) top[i] = new RectF();
        for (int i = 0; i < row2.length; i++) row2[i] = new RectF();
        for (int i = 0; i < TRACKS; i++) {
            rcName[i] = new RectF();
            rcMute[i] = new RectF();
            rcSolo[i] = new RectF();
            rcMeter[i] = new RectF();
            rcPad[i] = new RectF();
        }
        for (int i = 0; i < 16; i++) rcPQ[i] = new RectF();
        for (int i = 0; i < 6; i++) {
            rcVK[i] = new RectF();
            rcMK[i] = new RectF();
        }
        for (int i = 0; i < 5; i++) rcSS[i] = new RectF();
        for (int i = 0; i < ovBtn.length; i++) ovBtn[i] = new RectF();
        refreshSnapshot();
    }

    private static int blend(int a, int b, float t) {
        t = Math.max(0f, Math.min(1f, t));
        int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
        int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
        return Color.rgb((int) (ar + (br - ar) * t), (int) (ag + (bg - ag) * t),
                (int) (ab + (bb - ab) * t));
    }

    private static float clamp01(float v) {
        return v < 0f ? 0f : (v > 1f ? 1f : v);
    }

    /* ===================================================================== */
    /* 生命周期                                                              */
    /* ===================================================================== */
    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        attached = true;
        startFrames();
    }

    @Override
    protected void onDetachedFromWindow() {
        attached = false;
        stopFrames();
        super.onDetachedFromWindow();
    }

    @Override
    protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility == VISIBLE && attached) startFrames();
        else stopFrames();
    }

    public void onPause() {
        stopFrames();
    }

    public void onResume() {
        if (attached) startFrames();
    }

    private void startFrames() {
        if (framesRunning) return;
        framesRunning = true;
        Choreographer.getInstance().postFrameCallback(frameCb);
    }

    private void stopFrames() {
        framesRunning = false;
        Choreographer.getInstance().removeFrameCallback(frameCb);
    }

    public boolean handleBack() {
        if (overlay != OV_NONE) {
            overlay = OV_NONE;
            invalidate();
            return true;
        }
        return false;
    }

    /* ===================================================================== */
    /* 布局（dp 坐标）                                                        */
    /* ===================================================================== */
    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutAll();
    }

    private void layoutAll() {
        float w = getWidth(), h = getHeight();
        if (w < 4 || h < 4) return;
        float density = getResources().getDisplayMetrics().density;
        if (density < 0.5f) density = 1f;
        scale = density;
        baseW = w / scale;
        baseH = h / scale;

        final float pad = 5f;
        float topY = 3f, topH = 29f;
        float[] topW = {1.55f, 1.55f, 1.1f, 0.75f, 2.0f, 0.75f, 2.9f,
                0.75f, 1.85f, 0.75f, 0.75f, 1.6f, 0.75f, 1.55f, 1.95f, 1.85f, 1.55f};
        weightsRow(topY, topH, 2f, pad, pad, topW, top);

        float row2Y = topY + topH + 3f;
        float row2H = 24f;
        float[] r2W = {1.35f, 1.35f, 1.25f, 1.15f, 1.45f, 1.25f, 1.35f, 4.2f};
        weightsRow(row2Y, row2H, 2f, pad, pad, r2W, row2);

        float statusH = 12f;
        float stripH = 19f;
        float bottomH = 48f;
        float statusY = baseH - statusH - 2f;
        float stripY = statusY - stripH - 2f;
        float bottomY = stripY - bottomH - 3f;
        float mainY = row2Y + row2H + 5f;
        float mainH = Math.max(110f, bottomY - mainY - 3f);

        float gridW = (baseW - 2f * pad) * 0.615f;
        float panelX = pad + gridW + pad;
        float panelW = Math.max(150f, baseW - panelX - pad);

        rcGrid.set(pad, mainY, gridW, mainH);
        rcPanel.set(panelX, mainY, panelW, mainH);
        rcMaster.set(pad, bottomY, gridW, bottomH);
        rcPads.set(panelX, bottomY, panelW, bottomH);
        rcStrip.set(pad, stripY, baseW - 2f * pad, stripH);
        rcStatus.set(pad, statusY, baseW - 2f * pad, statusH);

        /* 网格 */
        float hdr = 15f;
        float leftW = 86f;
        rcSteps.set(rcGrid.left + leftW, rcGrid.top + hdr,
                Math.max(40f, rcGrid.width() - leftW - 3f),
                Math.max(40f, rcGrid.height() - hdr - 2f));
        rcStepNext.set(rcGrid.right - 20f, rcGrid.top + 1f, 18f, hdr - 2f);
        rcStepPage.set(rcGrid.right - 56f, rcGrid.top + 1f, 34f, hdr - 2f);
        rcStepPrev.set(rcGrid.right - 76f, rcGrid.top + 1f, 18f, hdr - 2f);

        float rowH = rcSteps.height() / TRACKS;
        for (int t = 0; t < TRACKS; t++) {
            float y = rcSteps.top + t * rowH;
            rcName[t].set(rcGrid.left + 2f, y, leftW - 40f, rowH);
            rcMute[t].set(rcGrid.left + leftW - 37f, y + 0.6f, 14f, Math.max(5f, rowH - 1.2f));
            rcSolo[t].set(rcGrid.left + leftW - 21f, y + 0.6f, 14f, Math.max(5f, rowH - 1.2f));
            rcMeter[t].set(rcGrid.left + leftW - 5f, y + 0.6f, 3f, Math.max(5f, rowH - 1.2f));
        }

        /* 声部面板 */
        rcTrigger.set(rcPanel.right - 58f, rcPanel.top + 2f, 54f, 14f);
        rcTLenInc.set(rcPanel.right - 74f, rcPanel.top + 2f, 13f, 14f);
        rcTLenDisp.set(rcPanel.right - 118f, rcPanel.top + 2f, 42f, 14f);
        rcTLenDec.set(rcPanel.right - 133f, rcPanel.top + 2f, 13f, 14f);
        float knobY = rcPanel.top + 19f;
        float knobH = Math.min(50f, Math.max(34f, rcPanel.height() * 0.24f));
        float cellW = rcPanel.width() / 6f;
        for (int i = 0; i < 6; i++) {
            rcVK[i].set(rcPanel.left + i * cellW, knobY, cellW, knobH);
        }
        float stepHdrY = knobY + knobH + 1f;
        float sliderY = stepHdrY + 10f;
        float condH = 20f;
        float avail = rcPanel.bottom - condH - 3f - sliderY;
        float sliderH = Math.max(12f, Math.min(18f, avail / 5f));
        for (int i = 0; i < 5; i++) {
            rcSS[i].set(rcPanel.left + 2f, sliderY + i * sliderH,
                    rcPanel.width() - 4f, sliderH);
        }
        float condY = sliderY + 5f * sliderH + 1f;
        if (condY > rcPanel.bottom - 16f) condY = rcPanel.bottom - 16f;
        float condW = rcPanel.width() - 4f;
        rcCond.set(rcPanel.left + 2f, condY, condW * 0.44f, condH);
        rcCondN.set(rcPanel.left + 2f + condW * 0.45f, condY, condW * 0.13f, condH);
        rcAccent.set(rcPanel.left + 2f + condW * 0.59f, condY, condW * 0.41f, condH);

        /* 主控 / 打击垫 */
        float mkw = rcMaster.width() / 6f;
        for (int i = 0; i < 6; i++) {
            rcMK[i].set(rcMaster.left + i * mkw, rcMaster.top, mkw, rcMaster.height());
        }
        float padW = rcPads.width() / 8f;
        float padH = rcPads.height() / 2f;
        for (int i = 0; i < TRACKS; i++) {
            int col = i % 8, row = i / 8;
            rcPad[i].set(rcPads.left + col * padW + 1f, rcPads.top + row * padH + 1f,
                    padW - 2f, padH - 2f);
        }

        /* pattern 快捷条 */
        rcPagePrev.set(rcStrip.left, rcStrip.top, 13f, rcStrip.height());
        rcPageNext.set(rcStrip.right - 13f, rcStrip.top, 13f, rcStrip.height());
        float pqW = (rcStrip.width() - 28f) / 16f;
        for (int i = 0; i < 16; i++) {
            rcPQ[i].set(rcStrip.left + 14f + i * pqW, rcStrip.top, pqW - 1f, rcStrip.height());
        }

        layoutOverlay();
    }

    private void weightsRow(float y, float h, float gap, float left, float right,
                            float[] weights, RectF[] out) {
        float total = 0f;
        for (float v : weights) total += v;
        float avail = right - left - gap * (weights.length - 1);
        float x = left;
        for (int i = 0; i < weights.length && i < out.length; i++) {
            float bw = avail * weights[i] / total;
            out[i].set(x, y, bw, h);
            x += bw + gap;
        }
    }

    private void layoutOverlay() {
        if (overlay == OV_NONE) return;
        float mw = Math.min(baseW - 16f, Math.max(320f, baseW * 0.74f));
        float mh = baseH * 0.88f;
        rcOvPanel.set((baseW - mw) / 2f, (baseH - mh) / 2f, mw, mh);
        float titleH = 24f;
        float btnH = 26f;
        rcOvList.set(rcOvPanel.left + 6f, rcOvPanel.top + titleH,
                rcOvPanel.width() - 12f, rcOvPanel.height() - titleH - btnH - 10f);
        ovRowH = 21f;
        ovRows = Math.max(1, (int) (rcOvList.height() / ovRowH));

        if (overlay == OV_SONG) {
            float[] wts = {1f, 1f, 1f, 1f, 1f, 1f, 1f, 1f, 1.2f, 1.4f};
            weightsRow(rcOvPanel.bottom - btnH - 5f, btnH, 3f,
                    rcOvPanel.left + 6f, rcOvPanel.right - 6f, wts, ovBtn);
        } else {
            float[] wts = {1f, 1f};
            weightsRow(rcOvPanel.bottom - btnH - 5f, btnH, 3f,
                    rcOvPanel.left + 6f, rcOvPanel.right - 6f, wts, ovBtn);
        }
    }

    /* ===================================================================== */
    /* 数据刷新                                                              */
    /* ===================================================================== */
    private void refreshSnapshot() {
        if (engine == 0) return;
        Native808.snapshot(engine, -1, meta, params, steps, flags);
        Native808.status(engine, stInts, meters, misc);
    }

    private void updateEngine() {
        if (engine == 0) return;
        refreshSnapshot();
        clockSource = Native808.getClockSource(engine);
        int len = Math.max(1, Math.min(STEPS, meta[0]));
        if (selStep >= len) selStep = len - 1;
        if (stInts[0] != 0 && len > 16 && !fitSteps) {
            long now = System.currentTimeMillis();
            if (now > pageOverrideUntil) {
                int pg = (stInts[2] / 16) % 4;
                if (pg != stepPage) stepPage = pg;
            }
        }
        frameNo++;
        if (frameNo % 6 == 0) {
            statusLine = String.format(Locale.US,
                    "%s · CPU %d%% · XRUN %d · %s · %s · %d/%d · %s",
                    audio != null ? audio.info() : "NO AUDIO",
                    Math.round(misc[0] * 100f), stInts[5], msg,
                    stInts[0] != 0 ? "PLAY" : "STOP", meta[5], meta[6],
                    len + " STEPS");
        }
    }

    /* ===================================================================== */
    /* 绘制                                                                  */
    /* ===================================================================== */
    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        c.drawColor(C_BG);
        c.save();
        c.scale(scale, scale);
        drawTransport(c);
        drawGrid(c);
        drawVoicePanel(c);
        drawMaster(c);
        drawPads(c);
        drawPatternStrip(c);
        drawStatus(c);
        if (overlay != OV_NONE) drawOverlay(c);
        c.restore();
    }

    private void drawTransport(Canvas c) {
        boolean playing = stInts[0] != 0;
        button(c, top[0], playing ? "PLAYING" : "PLAY", playing, 11f);
        button(c, top[1], "STOP", false, 11f);
        button(c, top[2], "TAP", false, 10f);
        button(c, top[3], "-", false, 13f);
        display(c, top[4], String.format(Locale.US, "%.1f BPM", params[0]), C_ORANGE, 11f);
        button(c, top[5], "+", false, 13f);

        /* SWING */
        RectF sw = top[6];
        fillRect(c, sw, C_PANEL2);
        strokeRect(c, sw, 3f, C_PANEL3, 1f);
        RectF swLabel = new RectF(sw.left + 3f, sw.top, sw.width() * 0.28f, sw.height());
        textIn(c, swLabel, "SWING", C_DIM, 8f, Paint.Align.LEFT, false);
        RectF bar = new RectF(sw.left + sw.width() * 0.28f, sw.top + 8f,
                sw.width() * 0.48f, sw.height() - 16f);
        fillRect(c, bar, C_PANEL3);
        fillPaint.setColor(C_ORANGE);
        c.drawRect(bar.left, bar.top, bar.left + bar.width() * meta[1] / 100f, bar.bottom, fillPaint);
        RectF swVal = new RectF(sw.right - sw.width() * 0.23f, sw.top, sw.width() * 0.22f, sw.height());
        textIn(c, swVal, meta[1] + "%", C_TEXT, 8f, Paint.Align.RIGHT, false);

        button(c, top[7], "<", false, 11f);
        display(c, top[8], String.format(Locale.US, "PAT %03d", meta[2]), C_TEXT, 11f);
        button(c, top[9], ">", false, 11f);

        int len = Math.max(1, Math.min(STEPS, meta[0]));
        button(c, top[10], "-", false, 13f);
        display(c, top[11], "LEN " + len, C_TEXT, 10f);
        button(c, top[12], "+", false, 13f);
        button(c, top[13], meta[3] != 0 ? "SONG ON" : "SONG", meta[3] != 0, 9f);
        button(c, top[14], "EDIT SONG", false, 9f);
        button(c, top[15], clockSource != 0 ? "SYNC MIDI" : "SYNC INT",
                clockSource != 0, 7.5f);
        button(c, top[16], "TS " + meta[5] + "/" + meta[6], false, 7.5f);

        button(c, row2[0], "SAVE", false, 9f);
        button(c, row2[1], "LOAD", false, 9f);
        button(c, row2[2], "WAV", false, 9f);
        button(c, row2[3], "MIDI", false, 9f);
        button(c, row2[4], "STEMS", false, 9f);
        button(c, row2[5], "CLEAR", false, 9f);
        button(c, row2[6], "COPY>", false, 9f);
        button(c, row2[7], midi != null ? midi.status() : "MIDI: OFF",
                midi != null && midi.isOpen(), 7.5f);
    }

    private void drawGrid(Canvas c) {
        int len = Math.max(1, Math.min(STEPS, meta[0]));
        int pageCols = fitSteps ? len
                : Math.min(16, Math.max(1, len - stepPage * 16));
        float colW = rcSteps.width() / Math.max(1, pageCols);

        fillRect(c, rcGrid, C_PANEL);
        textIn(c, new RectF(rcGrid.left + 3f, rcGrid.top, 70f, 15f), "TRACK",
                C_DIM, 7.5f, Paint.Align.LEFT, false);
        button(c, rcStepPrev, "<", false, 8f);
        textIn(c, rcStepPage, fitSteps ? "ALL" : (stepPage + 1) + "/" + ((len + 15) / 16),
                fitSteps ? C_ORANGE : C_DIM, 7.5f, Paint.Align.CENTER, false);
        button(c, rcStepNext, ">", false, 8f);

        float rowH = rcSteps.height() / TRACKS;
        int playStep = stInts[0] != 0 ? stInts[2] : -1;

        for (int t = 0; t < TRACKS; t++) {
            float y = rcSteps.top + t * rowH;
            boolean selectedRow = t == selTrack;
            fillRect(c, new RectF(rcGrid.left, y, rcGrid.width() - 3f, rowH),
                    selectedRow ? C_SELROW : ((t & 1) != 0 ? 0xFF181B20 : 0xFF15181D));
            textIn(c, rcName[t], TRACK_NAMES[t], selectedRow ? C_ORANGE : C_TEXT,
                    7f, Paint.Align.LEFT, false);

            fillRect(c, rcMute[t], flags[t * 4] != 0 ? C_RED : C_PANEL3);
            textIn(c, rcMute[t], "M", flags[t * 4] != 0 ? C_TEXT : C_DIM, 7f,
                    Paint.Align.CENTER, true);
            fillRect(c, rcSolo[t], flags[t * 4 + 1] != 0 ? C_GREEN : C_PANEL3);
            textIn(c, rcSolo[t], "S", flags[t * 4 + 1] != 0 ? C_TEXT : C_DIM, 7f,
                    Paint.Align.CENTER, true);

            fillRect(c, rcMeter[t], C_PANEL3);
            float m = Math.min(1f, meters[t]);
            if (m > 0.01f) {
                fillPaint.setColor(m > 0.9f ? C_RED : (m > 0.62f ? C_ORANGE : C_GREEN));
                float mh = rcMeter[t].height() * m;
                c.drawRect(rcMeter[t].left, rcMeter[t].bottom - mh,
                        rcMeter[t].right, rcMeter[t].bottom, fillPaint);
            }

            int tlen = flags[t * 4 + 2] == 0 ? len : Math.min(len, flags[t * 4 + 2]);
            int rowPlay = playStep >= 0 ? playStep % Math.max(1, tlen) : -1;

            for (int col = 0; col < pageCols; col++) {
                int s = stepPage * 16 + col;
                if (s >= len) continue;
                int idx = (t * STEPS + s) * 10;
                boolean on = steps[idx] != 0;
                int vel = steps[idx + 1] & 0xff;
                boolean acc = steps[idx + 2] != 0;
                int prob = steps[idx + 3] & 0xff;
                int rat = steps[idx + 4] & 0xff;
                int micro = (short) ((steps[idx + 8] & 0xff) | ((steps[idx + 9] & 0xff) << 8));

                float cx = rcSteps.left + col * colW + 1f;
                float cy = y + 1f;
                RectF cell = new RectF(cx, cy, cx + colW - 2f, cy + rowH - 2f);
                int color = C_CELL;
                if (on) {
                    color = blend(C_ORANGE2, C_ORANGE, vel / 127f);
                    if (acc) color = C_ORANGE3;
                }
                fillPaint.setColor(color);
                c.drawRoundRect(cell, 1.5f, 1.5f, fillPaint);

                if (on && prob < 100) {
                    fillPaint.setColor(C_BLUE);
                    c.drawRect(cell.left + 1.5f, cell.top + 1.5f,
                            cell.left + 1.5f + Math.max(1.5f, colW * 0.16f), cell.top + 3f, fillPaint);
                }
                if (on && rat > 1) {
                    fillPaint.setColor(C_TEXT);
                    int n = Math.min(4, rat);
                    for (int k = 0; k < n; k++) {
                        c.drawRect(cell.left + 2f + k * 2.6f, cell.bottom - 3f,
                                cell.left + 3.2f + k * 2.6f, cell.bottom - 1f, fillPaint);
                    }
                }
                if (on && micro != 0) {
                    fillPaint.setColor(micro > 0 ? C_GREEN : C_RED);
                    c.drawRect(cell.right - 2.5f, cell.top, cell.right - 1f,
                            cell.top + 3f, fillPaint);
                }
                if (t == selTrack && s == selStep) {
                    strokePaint.setColor(C_TEXT);
                    strokePaint.setStyle(Paint.Style.STROKE);
                    strokePaint.setStrokeWidth(1.2f);
                    c.drawRoundRect(cell, 1.5f, 1.5f, strokePaint);
                }
                if (s == rowPlay) {
                    strokePaint.setColor(C_BLUE);
                    strokePaint.setStyle(Paint.Style.STROKE);
                    strokePaint.setStrokeWidth(1.6f);
                    c.drawRoundRect(cell, 1.5f, 1.5f, strokePaint);
                }
            }
        }
        /* 4 步分隔线 */
        strokePaint.setColor(0x30FFFFFF);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(0.8f);
        for (int col = 0; col <= pageCols; col += 4) {
            float x = rcSteps.left + col * colW;
            c.drawLine(x, rcSteps.top, x, rcSteps.bottom, strokePaint);
        }
    }

    private void drawVoicePanel(Canvas c) {
        fillRect(c, rcPanel, C_PANEL);
        textIn(c, new RectF(rcPanel.left + 3f, rcPanel.top,
                        Math.max(40f, rcPanel.width() - 150f), 16f),
                "VOICE · " + TRACK_NAMES[selTrack], C_ORANGE, 9f, Paint.Align.LEFT, true);
        button(c, rcTLenDec, "-", false, 8f);
        display(c, rcTLenDisp, "TLEN " + (flags[selTrack * 4 + 2] & 0xff), C_TEXT, 7f);
        button(c, rcTLenInc, "+", false, 8f);
        button(c, rcTrigger, "TRIG", false, 7.5f);

        for (int i = 0; i < 6; i++) {
            knob(c, rcVK[i], KNOB_NAMES[i], params[7 + selTrack * 6 + i]);
        }

        int idx = (selTrack * STEPS + selStep) * 10;
        int vel = steps[idx + 1] & 0xff;
        int prob = steps[idx + 3] & 0xff;
        int rat = Math.max(1, steps[idx + 4] & 0xff);
        int rate = steps[idx + 5] & 0xff;
        int cond = steps[idx + 6] & 0xff;
        int condN = steps[idx + 7] & 0xff;
        int micro = (short) ((steps[idx + 8] & 0xff) | ((steps[idx + 9] & 0xff) << 8));

        textIn(c, new RectF(rcPanel.left + 3f, rcSS[0].top - 10f,
                        rcPanel.width() - 6f, 9f),
                String.format(Locale.US, "STEP %02d · %s", selStep + 1, TRACK_NAMES[selTrack]),
                C_TEXT, 8.5f, Paint.Align.LEFT, true);

        slider(c, rcSS[0], "VELOCITY", (vel - 1) / 126f, String.valueOf(vel), false);
        slider(c, rcSS[1], "PROB", prob / 100f, prob + "%", false);
        slider(c, rcSS[2], "RATCHET", (rat - 1) / 7f, "x" + rat, false);
        slider(c, rcSS[3], "RATE", (rate - 10) / 90f, rate + "%", false);
        slider(c, rcSS[4], "MICRO", micro / 4800f + 0.5f,
                String.format(Locale.US, "%+d", micro), true);

        String condText = cond == 6 ? ("EVERY " + Math.max(2, condN))
                : ("COND: " + COND_NAMES[Math.min(6, cond)]);
        button(c, rcCond, condText, cond != 0, 7f);
        button(c, rcCondN, "N" + Math.max(2, condN), cond == 6, 7f);
        button(c, rcAccent, (steps[idx + 2] != 0) ? "ACCENT ON" : "ACCENT",
                steps[idx + 2] != 0, 7f);
    }

    private void drawMaster(Canvas c) {
        fillRect(c, rcMaster, C_PANEL);
        for (int i = 0; i < 6; i++) {
            knob(c, rcMK[i], MASTER_NAMES[i], params[1 + i]);
        }
    }

    private void drawPads(Canvas c) {
        fillRect(c, rcPads, C_PANEL);
        for (int i = 0; i < TRACKS; i++) {
            boolean sel = i == selTrack;
            fillRect(c, rcPad[i], sel ? C_ORANGE2 : C_PANEL2);
            textIn(c, rcPad[i], PAD_NAMES[i], sel ? C_TEXT : C_DIM, 7f,
                    Paint.Align.CENTER, false);
        }
    }

    private void drawPatternStrip(Canvas c) {
        fillRect(c, rcStrip, C_PANEL);
        button(c, rcPagePrev, "<", false, 8f);
        button(c, rcPageNext, ">", false, 8f);
        for (int i = 0; i < 16; i++) {
            int p = patPage * 16 + i;
            boolean cur = p == meta[2];
            fillPaint.setColor(cur ? C_ORANGE : C_PANEL2);
            c.drawRoundRect(rcPQ[i], 2f, 2f, fillPaint);
            textIn(c, rcPQ[i], String.valueOf(p), cur ? C_DARKTEXT : C_TEXT, 7f,
                    Paint.Align.CENTER, false);
        }
    }

    private void drawStatus(Canvas c) {
        fillRect(c, rcStatus, C_PANEL);
        boolean okAudio = audio != null && audio.isRunning();
        textIn(c, rcStatus, statusLine + (exportBusy ? " · EXPORTING..." : ""),
                okAudio ? C_DIM : C_RED, 7f, Paint.Align.LEFT, false);
    }

    /* ===================================================================== */
    /* 覆盖层：Song 链 / 工程文件                                             */
    /* ===================================================================== */
    private void drawOverlay(Canvas c) {
        fillPaint.setColor(0xB0000000);
        c.drawRect(0, 0, baseW, baseH, fillPaint);
        fillRect(c, rcOvPanel, C_PANEL);
        strokeRect(c, rcOvPanel, 3f, C_ORANGE2, 1.2f);

        if (overlay == OV_SONG) drawSongOverlay(c);
        else drawFilesOverlay(c);
    }

    private void drawSongOverlay(Canvas c) {
        textIn(c, new RectF(rcOvPanel.left + 8f, rcOvPanel.top + 3f,
                        rcOvPanel.width() - 16f, 20f),
                "SONG CHAIN · " + song.size() + " ENTRIES", C_ORANGE, 10f,
                Paint.Align.LEFT, true);
        textIn(c, new RectF(rcOvPanel.right - 150f, rcOvPanel.top + 3f, 142f, 20f),
                "SONG " + (meta[3] != 0 ? "ON" : "OFF"), C_DIM, 8f,
                Paint.Align.RIGHT, false);

        clampSongScroll();
        for (int r = 0; r < ovRows; r++) {
            int i = songScroll + r;
            if (i >= song.size()) break;
            RectF rr = new RectF(rcOvList.left, rcOvList.top + r * ovRowH,
                    rcOvList.width(), ovRowH - 1f);
            boolean sel = i == selSong;
            fillRect(c, rr, sel ? C_ORANGE2 : ((r & 1) != 0 ? C_PANEL2 : 0xFF20242B));
            int[] e = song.get(i);
            String s = String.format(Locale.US, "%02d    PATTERN %03d    x%d", i + 1, e[0], e[1]);
            textIn(c, rr, s, sel ? C_TEXT : C_DIM, 8f, Paint.Align.LEFT, sel);
        }
        if (song.isEmpty()) {
            textIn(c, rcOvList, "EMPTY · PRESS ADD", C_DIM, 9f, Paint.Align.CENTER, false);
        }
        String[] labels = {"ADD", "DEL", "UP", "DOWN", "PAT-", "PAT+", "REP-", "REP+",
                "CANCEL", "DONE"};
        for (int i = 0; i < labels.length; i++) {
            button(c, ovBtn[i], labels[i], i == 9, 8f);
        }
        if (songScroll > 0) textIn(c, new RectF(rcOvList.right - 12f, rcOvList.top, 12f, 12f),
                "^", C_ORANGE, 8f, Paint.Align.CENTER, true);
        if (songScroll + ovRows < song.size())
            textIn(c, new RectF(rcOvList.right - 12f, rcOvList.bottom - 12f, 12f, 12f),
                    "v", C_ORANGE, 8f, Paint.Align.CENTER, true);
    }

    private void drawFilesOverlay(Canvas c) {
        textIn(c, new RectF(rcOvPanel.left + 8f, rcOvPanel.top + 3f,
                        rcOvPanel.width() - 16f, 20f),
                "LOAD PROJECT · " + projectDir.getName(), C_ORANGE, 10f,
                Paint.Align.LEFT, true);
        for (int r = 0; r < ovRows; r++) {
            int i = songScroll + r;
            if (i >= files.size()) break;
            RectF rr = new RectF(rcOvList.left, rcOvList.top + r * ovRowH,
                    rcOvList.width(), ovRowH - 1f);
            fillRect(c, rr, (r & 1) != 0 ? C_PANEL2 : 0xFF20242B);
            File f = files.get(i);
            String name = f.getName();
            String date = new SimpleDateFormat("MM-dd HH:mm", Locale.US)
                    .format(new Date(f.lastModified()));
            textIn(c, rr, name, C_TEXT, 8f, Paint.Align.LEFT, false);
            textIn(c, new RectF(rr.right - 74f, rr.top, 70f, rr.height()),
                    date, C_DIM, 7f, Paint.Align.RIGHT, false);
        }
        if (files.isEmpty()) {
            textIn(c, rcOvList, "NO PROJECTS · USE SAVE FIRST", C_DIM, 9f,
                    Paint.Align.CENTER, false);
        }
        button(c, ovBtn[0], "CLOSE", false, 8f);
        button(c, ovBtn[1], "REFRESH", false, 8f);
        textIn(c, new RectF(rcOvList.left, rcOvPanel.bottom - 40f, rcOvList.width(), 10f),
                projectDir.getAbsolutePath(), C_DIM, 6.5f, Paint.Align.LEFT, false);
    }

    private void clampSongScroll() {
        int n = overlay == OV_SONG ? song.size() : files.size();
        int maxScroll = Math.max(0, n - ovRows);
        if (songScroll > maxScroll) songScroll = maxScroll;
        if (songScroll < 0) songScroll = 0;
        if (selSong >= n) selSong = Math.max(0, n - 1);
        if (overlay == OV_SONG) {
            if (selSong < songScroll) songScroll = selSong;
            if (selSong >= songScroll + ovRows) songScroll = selSong - ovRows + 1;
        }
    }

    /* ===================================================================== */
    /* 绘制工具                                                              */
    /* ===================================================================== */
    private void fillRect(Canvas c, RectF r, int color) {
        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(color);
        c.drawRect(r, fillPaint);
    }

    private void strokeRect(Canvas c, RectF r, float radius, int color, float width) {
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setColor(color);
        strokePaint.setStrokeWidth(width);
        c.drawRoundRect(r, radius, radius, strokePaint);
    }

    private void textIn(Canvas c, RectF r, String s, int color, float size,
                        Paint.Align align, boolean bold) {
        textPaint.setTypeface(bold ? tfBold : tfNorm);
        textPaint.setTextSize(size);
        textPaint.setColor(color);
        textPaint.setTextAlign(align);
        textPaint.setStyle(Paint.Style.FILL);
        float x = align == Paint.Align.LEFT ? r.left
                : (align == Paint.Align.RIGHT ? r.right : r.centerX());
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float y = r.centerY() - (fm.ascent + fm.descent) / 2f;
        c.save();
        c.clipRect(r);
        c.drawText(s, x, y, textPaint);
        c.restore();
    }

    private void button(Canvas c, RectF r, String label, boolean active, float size) {
        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(active ? C_ORANGE : C_PANEL2);
        c.drawRoundRect(r, 3f, 3f, fillPaint);
        strokeRect(c, r, 3f, active ? C_ORANGE : C_PANEL3, 0.8f);
        textIn(c, r, label, active ? C_DARKTEXT : C_TEXT, size, Paint.Align.CENTER, true);
    }

    private void display(Canvas c, RectF r, String s, int color, float size) {
        fillRect(c, r, C_PANEL2);
        strokeRect(c, r, 3f, C_PANEL3, 0.8f);
        textIn(c, r, s, color, size, Paint.Align.CENTER, true);
    }

    private void knob(Canvas c, RectF cell, String label, float v) {
        float rad = Math.min(cell.width() * 0.30f, (cell.height() - 17f) * 0.5f);
        if (rad < 5f) rad = 5f;
        float cx = cell.centerX();
        float cy = cell.top + 2f + rad;
        fillPaint.setStyle(Paint.Style.FILL);
        fillPaint.setColor(C_PANEL3);
        c.drawCircle(cx, cy, rad, fillPaint);

        RectF oval = new RectF(cx - rad, cy - rad, cx + rad, cy + rad);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeWidth(2.2f);
        strokePaint.setColor(C_ORANGE2);
        c.drawArc(oval, 135f, 270f, false, strokePaint);
        strokePaint.setStrokeWidth(1.6f);
        strokePaint.setColor(C_ORANGE);
        c.drawArc(oval, 135f, 270f * Math.max(0f, Math.min(1f, v)), false, strokePaint);

        double ang = Math.toRadians(135 + 270 * Math.max(0f, Math.min(1f, v)));
        strokePaint.setStrokeWidth(1.4f);
        strokePaint.setColor(C_TEXT);
        c.drawLine(cx, cy,
                cx + (float) (Math.cos(ang) * (rad - 1.5f)),
                cy + (float) (Math.sin(ang) * (rad - 1.5f)), strokePaint);

        textIn(c, new RectF(cell.left, cy + rad + 1f, cell.width(), 8f),
                label, C_DIM, 6.5f, Paint.Align.CENTER, false);
        textIn(c, new RectF(cell.left, cy + rad + 8f, cell.width(), 8f),
                String.valueOf(Math.round(v * 100)), C_TEXT, 7f, Paint.Align.CENTER, true);
    }

    private void slider(Canvas c, RectF r, String label, float t, String value, boolean bipolar) {
        t = Math.max(0f, Math.min(1f, t));
        textIn(c, new RectF(r.left, r.top, 40f, r.height()), label, C_DIM, 7f,
                Paint.Align.LEFT, false);
        textIn(c, new RectF(r.right - 40f, r.top, 40f, r.height()), value, C_TEXT, 7f,
                Paint.Align.RIGHT, false);
        float bx = r.left + 41f;
        float bw = Math.max(10f, r.width() - 82f);
        RectF bar = new RectF(bx, r.centerY() - 2.5f, bx + bw, r.centerY() + 2.5f);
        fillRect(c, bar, C_PANEL3);
        fillPaint.setColor(C_ORANGE);
        if (bipolar) {
            float mid = bar.centerX();
            float x = bar.left + bw * t;
            c.drawRect(Math.min(mid, x), bar.top, Math.max(mid, x), bar.bottom, fillPaint);
            strokePaint.setColor(C_DIM);
            strokePaint.setStyle(Paint.Style.STROKE);
            strokePaint.setStrokeWidth(0.8f);
            c.drawLine(mid, bar.top - 1f, mid, bar.bottom + 1f, strokePaint);
        } else {
            c.drawRect(bar.left, bar.top, bar.left + bw * t, bar.bottom, fillPaint);
        }
    }

    /* ===================================================================== */
    /* 触摸                                                                  */
    /* ===================================================================== */
    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        float x = ev.getX() / scale;
        float y = ev.getY() / scale;
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = x;
                downY = y;
                longPressFired = false;
                longPressKind = LP_NONE;
                if (overlay != OV_NONE) return overlayDown(x, y);
                handleDown(x, y);
                return true;
            case MotionEvent.ACTION_MOVE:
                if (overlay != OV_NONE) {
                    overlayMove(y);
                    return true;
                }
                handleMove(x, y);
                return true;
            case MotionEvent.ACTION_UP:
                if (overlay != OV_NONE) {
                    overlayUp();
                    cancelPressTimer();
                    return true;
                }
                handleUp(x, y);
                cancelPressTimer();
                return true;
            case MotionEvent.ACTION_CANCEL:
                if (overlay != OV_NONE) resetOverlayDrag();
                cancelPressTimer();
                dragKind = 0;
                return true;
            default:
                return true;
        }
    }

    private void handleDown(float x, float y) {
        int pat = meta[2];

        if (top[0].contains(x, y)) {
            if (stInts[0] != 0) Native808.stop(engine);
            else Native808.play(engine);
            return;
        }
        if (top[1].contains(x, y)) {
            Native808.stopReset(engine);
            return;
        }
        if (top[2].contains(x, y)) {
            tapTempo();
            return;
        }
        if (top[3].contains(x, y)) {
            setBpm(params[0] - 1f);
            return;
        }
        if (top[5].contains(x, y)) {
            setBpm(params[0] + 1f);
            return;
        }
        if (top[4].contains(x, y)) {
            dragKind = 4;
            dragStartY = y;
            dragStartV = params[0];
            return;
        }
        if (top[6].contains(x, y)) {
            dragKind = 5;
            setSwingFromX(x);
            return;
        }
        if (top[7].contains(x, y)) {
            Native808.selectPattern(engine, (pat + PATTERNS - 1) % PATTERNS);
            return;
        }
        if (top[9].contains(x, y)) {
            Native808.selectPattern(engine, (pat + 1) % PATTERNS);
            return;
        }
        if (top[10].contains(x, y)) {
            cycleLen(-1);
            return;
        }
        if (top[12].contains(x, y)) {
            cycleLen(1);
            return;
        }
        if (top[13].contains(x, y)) {
            Native808.setSongOn(engine, meta[3] != 0 ? 0 : 1);
            return;
        }
        if (top[14].contains(x, y)) {
            openSongEditor();
            return;
        }
        if (top[15].contains(x, y)) {
            int v = clockSource != 0 ? 0 : 1;
            Native808.setClockSource(engine, v);
            clockSource = v;
            msg = v != 0 ? "CLOCK SOURCE: MIDI" : "CLOCK SOURCE: INTERNAL";
            return;
        }
        if (top[16].contains(x, y)) {
            cycleTimesig();
            return;
        }

        if (row2[0].contains(x, y)) {
            saveProject();
            return;
        }
        if (row2[1].contains(x, y)) {
            openFiles();
            return;
        }
        if (row2[2].contains(x, y)) {
            exportFile(0);
            return;
        }
        if (row2[3].contains(x, y)) {
            exportFile(1);
            return;
        }
        if (row2[4].contains(x, y)) {
            exportFile(2);
            return;
        }
        if (row2[5].contains(x, y)) {
            msg = "HOLD CLEAR TO ERASE PATTERN " + pat;
            longPressKind = LP_CLEAR;
            handler.postDelayed(longPressRunnable, 600);
            return;
        }
        if (row2[6].contains(x, y)) {
            int dst = (pat + 1) % PATTERNS;
            Native808.copyPattern(engine, pat, dst);
            Native808.selectPattern(engine, dst);
            msg = "COPIED " + pat + " -> " + dst;
            return;
        }
        if (row2[7].contains(x, y)) {
            if (midi != null) midi.toggle();
            msg = midi != null ? midi.status() : "MIDI NOT AVAILABLE";
            return;
        }

        if (rcStepPrev.contains(x, y)) {
            if (!fitSteps && stepPage > 0) stepPage--;
            pageOverrideUntil = System.currentTimeMillis() + 2500;
            return;
        }
        if (rcStepNext.contains(x, y)) {
            int pages = (Math.max(1, meta[0]) + 15) / 16;
            if (!fitSteps && stepPage < pages - 1) stepPage++;
            pageOverrideUntil = System.currentTimeMillis() + 2500;
            return;
        }
        if (rcStepPage.contains(x, y)) {
            fitSteps = !fitSteps;
            stepPage = 0;
            pageOverrideUntil = System.currentTimeMillis() + 2500;
            msg = fitSteps ? "STEP VIEW: ALL COLUMNS" : "STEP VIEW: 16-STEP PAGES";
            return;
        }

        for (int t = 0; t < TRACKS; t++) {
            if (rcMute[t].contains(x, y)) {
                Native808.setMute(engine, t, flags[t * 4] != 0 ? 0 : 1);
                return;
            }
            if (rcSolo[t].contains(x, y)) {
                Native808.setSolo(engine, t, flags[t * 4 + 1] != 0 ? 0 : 1);
                return;
            }
            if (rcName[t].contains(x, y)) {
                selTrack = t;
                return;
            }
        }

        int[] g = gridAt(x, y);
        if (g != null) {
            selTrack = g[0];
            selStep = g[1];
            lpTrack = g[0];
            lpStep = g[1];
            longPressKind = LP_GRID;
            handler.postDelayed(longPressRunnable, 420);
            return;
        }

        for (int i = 0; i < 6; i++) {
            if (rcVK[i].contains(x, y)) {
                dragKind = 1;
                dragIdx = i;
                dragStartY = y;
                dragStartV = params[7 + selTrack * 6 + i];
                return;
            }
            if (rcMK[i].contains(x, y)) {
                dragKind = 2;
                dragIdx = i;
                dragStartY = y;
                dragStartV = params[1 + i];
                return;
            }
        }
        for (int i = 0; i < 5; i++) {
            if (rcSS[i].contains(x, y)) {
                dragKind = 3;
                dragIdx = i;
                setSliderFromX(i, x);
                return;
            }
        }
        if (rcCond.contains(x, y)) {
            int idx = (selTrack * STEPS + selStep) * 10;
            int c = (steps[idx + 6] + 1) % 7;
            int n = Math.max(2, steps[idx + 7] & 0xff);
            Native808.setCond(engine, meta[2], selTrack, selStep, c, n);
            msg = "COND " + COND_NAMES[c];
            return;
        }
        if (rcCondN.contains(x, y)) {
            int idx = (selTrack * STEPS + selStep) * 10;
            int n = (steps[idx + 7] & 0xff) + 1;
            if (n < 2) n = 2;
            if (n > 8) n = 2;
            Native808.setCond(engine, meta[2], selTrack, selStep, 6, n);
            msg = "COND EVERY " + n;
            return;
        }
        if (rcAccent.contains(x, y)) {
            toggleAccent(selTrack, selStep);
            return;
        }
        if (rcTLenDec.contains(x, y)) {
            int cur = flags[selTrack * 4 + 2] & 0xff;
            if (cur > 1) Native808.setTrackLen(engine, selTrack, cur - 1);
            return;
        }
        if (rcTLenInc.contains(x, y)) {
            int cur = flags[selTrack * 4 + 2] & 0xff;
            int max = Math.max(1, meta[0]);
            if (cur < max) Native808.setTrackLen(engine, selTrack, cur + 1);
            return;
        }
        if (rcTrigger.contains(x, y)) {
            Native808.noteOn(engine, selTrack, 112);
            return;
        }
        for (int i = 0; i < TRACKS; i++) {
            if (rcPad[i].contains(x, y)) {
                Native808.noteOn(engine, i, 112);
                selTrack = i;
                return;
            }
        }
        for (int i = 0; i < 16; i++) {
            if (rcPQ[i].contains(x, y)) {
                Native808.selectPattern(engine, patPage * 16 + i);
                return;
            }
        }
        if (rcPagePrev.contains(x, y)) {
            if (patPage > 0) patPage--;
            return;
        }
        if (rcPageNext.contains(x, y)) {
            if (patPage < 15) patPage++;
            return;
        }
    }

    private void handleMove(float x, float y) {
        if (longPressKind != LP_NONE) {
            float slop = 7f;
            if (Math.abs(x - downX) > slop || Math.abs(y - downY) > slop) {
                handler.removeCallbacks(longPressRunnable);
                if (longPressKind == LP_GRID) longPressKind = LP_NONE;
            }
        }
        switch (dragKind) {
            case 1: {
                float v = clamp01(dragStartV + (dragStartY - y) / 70f);
                Native808.setTrackParam(engine, selTrack, dragIdx, v);
                break;
            }
            case 2: {
                float v = clamp01(dragStartV + (dragStartY - y) / 70f);
                Native808.setMaster(engine, dragIdx, v);
                break;
            }
            case 3:
                setSliderFromX(dragIdx, x);
                break;
            case 4: {
                float v = dragStartV + (dragStartY - y) * 0.9f;
                setBpm(Math.max(20f, Math.min(400f, v)));
                break;
            }
            case 5:
                setSwingFromX(x);
                break;
            default:
                break;
        }
    }

    private void handleUp(float x, float y) {
        handler.removeCallbacks(longPressRunnable);
        if (longPressKind == LP_GRID && !longPressFired) {
            int idx = (lpTrack * STEPS + lpStep) * 10;
            Native808.setStep(engine, meta[2], lpTrack, lpStep, Native808.F_ON,
                    steps[idx] != 0 ? 0 : 1);
        }
        longPressKind = LP_NONE;
        dragKind = 0;
    }

    private void cancelPressTimer() {
        handler.removeCallbacks(longPressRunnable);
        if (longPressKind != LP_NONE && !longPressFired) longPressKind = LP_NONE;
    }

    private int[] gridAt(float x, float y) {
        if (!rcSteps.contains(x, y)) return null;
        int len = Math.max(1, Math.min(STEPS, meta[0]));
        int cols = fitSteps ? len : Math.min(16, Math.max(1, len - stepPage * 16));
        float colW = rcSteps.width() / Math.max(1, cols);
        float rowH = rcSteps.height() / TRACKS;
        int t = (int) ((y - rcSteps.top) / rowH);
        int col = (int) ((x - rcSteps.left) / colW);
        int s = fitSteps ? col : stepPage * 16 + col;
        if (t < 0 || t >= TRACKS || col < 0 || col >= cols || s >= len) return null;
        return new int[]{t, s};
    }

    private void toggleAccent(int t, int s) {
        int idx = (t * STEPS + s) * 10;
        Native808.setStep(engine, meta[2], t, s, Native808.F_ACCENT,
                steps[idx + 2] != 0 ? 0 : 1);
        msg = "ACCENT " + TRACK_NAMES[t] + " STEP " + (s + 1);
    }

    private void setSliderFromX(int i, float x) {
        RectF r = rcSS[i];
        float bx = r.left + 41f;
        float bw = Math.max(10f, r.width() - 82f);
        float t = clamp01((x - bx) / bw);
        int pat = meta[2];
        switch (i) {
            case 0:
                Native808.setStep(engine, pat, selTrack, selStep, Native808.F_VEL,
                        (int) (1 + t * 126));
                break;
            case 1:
                Native808.setStep(engine, pat, selTrack, selStep, Native808.F_PROB,
                        (int) (t * 100));
                break;
            case 2:
                Native808.setStep(engine, pat, selTrack, selStep, Native808.F_RATCHET,
                        (int) (1 + t * 7));
                break;
            case 3:
                Native808.setStep(engine, pat, selTrack, selStep, Native808.F_RATE,
                        (int) (10 + t * 90));
                break;
            default:
                Native808.setStep(engine, pat, selTrack, selStep, Native808.F_MICRO,
                        (int) ((t - 0.5f) * 4800));
                break;
        }
    }

    private void setSwingFromX(float x) {
        RectF sw = top[6];
        float bx = sw.left + sw.width() * 0.28f;
        float bw = Math.max(10f, sw.width() * 0.48f);
        int v = (int) (clamp01((x - bx) / bw) * 100f);
        Native808.setSwing(engine, meta[2], v);
    }

    private void setBpm(float v) {
        Native808.setBpm(engine, Math.max(20f, Math.min(400f, v)));
    }

    private void cycleLen(int dir) {
        int len = Math.max(1, Math.min(STEPS, meta[0]));
        int next = dir > 0 ? (len == 16 ? 32 : (len == 32 ? 64 : 16))
                : (len == 64 ? 32 : (len == 32 ? 16 : 64));
        Native808.setPatternLen(engine, meta[2], next);
        if (selStep >= next) selStep = next - 1;
        stepPage = 0;
        msg = "PATTERN LENGTH " + next;
    }

    private void cycleTimesig() {
        int idx = 0;
        for (int i = 0; i < TIME_SIGS.length; i++) {
            if (TIME_SIGS[i][0] == meta[5] && TIME_SIGS[i][1] == meta[6]) {
                idx = i;
                break;
            }
        }
        int[] ts = TIME_SIGS[(idx + 1) % TIME_SIGS.length];
        Native808.setTimesig(engine, ts[0], ts[1]);
        msg = "TIME SIGNATURE " + ts[0] + "/" + ts[1];
    }

    private void tapTempo() {
        long now = System.currentTimeMillis();
        if (!taps.isEmpty() && now - lastTap > 2200) taps.clear();
        if (!taps.isEmpty()) {
            long dt = now - lastTap;
            if (dt > 150 && dt < 1600) {
                taps.add(dt);
                if (taps.size() > 8) taps.remove(0);
            }
        }
        lastTap = now;
        if (taps.size() >= 2) {
            long sum = 0;
            for (long d : taps) sum += d;
            float bpm = (float) (60000.0 / ((double) sum / taps.size()));
            setBpm(bpm);
            msg = String.format(Locale.US, "TAP TEMPO %.1f BPM", bpm);
        } else {
            msg = "TAP TEMPO · KEEP TAPPING";
        }
    }

    /* ===================================================================== */
    /* 覆盖层交互                                                            */
    /* ===================================================================== */
    private boolean overlayDown(float x, float y) {
        if (!rcOvPanel.contains(x, y)) {
            overlay = OV_NONE;
            return true;
        }
        if (overlay == OV_SONG) {
            if (ovBtn[0].contains(x, y)) {
                addSongEntry();
                return true;
            }
            if (ovBtn[1].contains(x, y)) {
                deleteSongEntry();
                return true;
            }
            if (ovBtn[2].contains(x, y)) {
                moveSongEntry(-1);
                return true;
            }
            if (ovBtn[3].contains(x, y)) {
                moveSongEntry(1);
                return true;
            }
            if (ovBtn[4].contains(x, y)) {
                shiftSongPat(-1);
                return true;
            }
            if (ovBtn[5].contains(x, y)) {
                shiftSongPat(1);
                return true;
            }
            if (ovBtn[6].contains(x, y)) {
                shiftSongReps(-1);
                return true;
            }
            if (ovBtn[7].contains(x, y)) {
                shiftSongReps(1);
                return true;
            }
            if (ovBtn[8].contains(x, y)) {
                overlay = OV_NONE;
                return true;
            }
            if (ovBtn[9].contains(x, y)) {
                applySong();
                return true;
            }
        } else {
            if (ovBtn[0].contains(x, y)) {
                overlay = OV_NONE;
                return true;
            }
            if (ovBtn[1].contains(x, y)) {
                loadFileList();
                return true;
            }
        }
        if (rcOvList.contains(x, y)) {
            int row = (int) ((y - rcOvList.top) / ovRowH);
            ovTouchY = y;
            ovTouchScroll = songScroll;
            ovMoved = false;
            ovPendingRow = row;
            ovPendingIndex = songScroll + row;
            return true;
        }
        resetOverlayDrag();
        return true;
    }

    private void overlayMove(float y) {
        if (ovTouchScroll < 0) return;
        float dy = y - ovTouchY;
        if (!ovMoved && Math.abs(dy) > 5f) ovMoved = true;
        if (!ovMoved) return;
        int n = overlay == OV_SONG ? song.size() : files.size();
        int max = Math.max(0, n - ovRows);
        int delta = (int) (dy / ovRowH);
        songScroll = Math.max(0, Math.min(max, ovTouchScroll - delta));
    }

    private void overlayUp() {
        if (ovTouchScroll >= 0 && !ovMoved && ovPendingIndex >= 0) {
            if (overlay == OV_SONG) {
                if (ovPendingIndex < song.size()) {
                    selSong = ovPendingIndex;
                    clampSongScroll();
                }
            } else if (ovPendingIndex < files.size()) {
                loadProject(files.get(ovPendingIndex));
            }
        }
        resetOverlayDrag();
    }

    private void resetOverlayDrag() {
        ovTouchScroll = -1;
        ovPendingRow = -1;
        ovPendingIndex = -1;
        ovMoved = false;
    }

    private void openSongEditor() {
        song.clear();
        int n = Native808.songLen(engine);
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            song.add(new int[]{Native808.songPat(engine, i),
                    Math.max(1, Native808.songReps(engine, i))});
        }
        selSong = 0;
        songScroll = 0;
        overlay = OV_SONG;
        layoutOverlay();
        invalidate();
    }

    private void addSongEntry() {
        int pat = song.isEmpty() ? meta[2] : song.get(song.size() - 1)[0];
        song.add(new int[]{pat, 4});
        selSong = song.size() - 1;
        clampSongScroll();
    }

    private void deleteSongEntry() {
        if (selSong >= 0 && selSong < song.size()) {
            song.remove(selSong);
            if (selSong >= song.size()) selSong = song.size() - 1;
            clampSongScroll();
        }
    }

    private void moveSongEntry(int dir) {
        int to = selSong + dir;
        if (selSong >= 0 && selSong < song.size() && to >= 0 && to < song.size()) {
            int[] t = song.get(selSong);
            song.set(selSong, song.get(to));
            song.set(to, t);
            selSong = to;
            clampSongScroll();
        }
    }

    private void shiftSongPat(int dir) {
        if (selSong >= 0 && selSong < song.size()) {
            int[] e = song.get(selSong);
            e[0] = (e[0] + PATTERNS + dir) % PATTERNS;
        }
    }

    private void shiftSongReps(int dir) {
        if (selSong >= 0 && selSong < song.size()) {
            int[] e = song.get(selSong);
            e[1] = Math.max(1, Math.min(64, e[1] + dir));
        }
    }

    private void applySong() {
        Native808.setSongLen(engine, Math.max(1, song.size()));
        for (int i = 0; i < song.size(); i++) {
            Native808.setSongEntry(engine, i, song.get(i)[0], song.get(i)[1]);
        }
        msg = "SONG CHAIN APPLIED · " + song.size() + " ENTRIES";
        overlay = OV_NONE;
    }

    private void openFiles() {
        loadFileList();
        overlay = OV_FILES;
        songScroll = 0;
        layoutOverlay();
        invalidate();
    }

    private void loadFileList() {
        files.clear();
        File[] list = projectDir.listFiles((dir, name) ->
                name.toLowerCase(Locale.US).endsWith(".808"));
        if (list != null) {
            Arrays.sort(list, new Comparator<File>() {
                @Override
                public int compare(File a, File b) {
                    return Long.compare(b.lastModified(), a.lastModified());
                }
            });
            files.addAll(Arrays.asList(list));
        }
    }

    /* ===================================================================== */
    /* 文件 / 导出 / 加载                                                    */
    /* ===================================================================== */
    private String timestamp() {
        return new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
    }

    public boolean hasActiveWorkers() {
        return jobs.get() > 0;
    }

    private void saveProject() {
        final File f = new File(projectDir, "808-" + timestamp() + ".808");
        msg = "SAVING " + f.getName();
        jobs.incrementAndGet();
        new Thread(() -> {
            final int rc;
            try {
                rc = Native808.save(engine, f.getAbsolutePath());
            } finally {
                jobs.decrementAndGet();
            }
            postMain(new Runnable() {
                @Override
                public void run() {
                    msg = rc == 0 ? ("SAVED " + f.getName()) : ("SAVE FAILED " + rc);
                }
            });
        }, "808-save").start();
    }

    private void loadProject(final File f) {
        msg = "LOADING " + f.getName();
        jobs.incrementAndGet();
        new Thread(() -> {
            final int rc;
            try {
                rc = Native808.load(engine, f.getAbsolutePath());
            } finally {
                jobs.decrementAndGet();
            }
            postMain(new Runnable() {
                @Override
                public void run() {
                    msg = rc == 0 ? ("LOADED " + f.getName()) : ("LOAD FAILED " + rc);
                    if (rc == 0) {
                        overlay = OV_NONE;
                        stepPage = 0;
                        selStep = 0;
                    }
                }
            });
        }, "808-load").start();
    }

    private void exportFile(int kind) {
        if (exportBusy) {
            msg = "EXPORT ALREADY RUNNING";
            return;
        }
        final int mode = meta[3] != 0 ? 1 : 0;
        String ts = timestamp();
        final File f;
        final int k = kind;
        if (kind == 1) f = new File(exportDir, "808-" + ts + ".mid");
        else if (kind == 2) f = new File(exportDir, "808-stems-" + ts + ".wav");
        else f = new File(exportDir, "808-" + ts + ".wav");
        exportBusy = true;
        msg = "EXPORTING " + f.getName();
        jobs.incrementAndGet();
        new Thread(() -> {
            final int rc;
            try {
                rc = (k == 1)
                        ? Native808.exportMidi(engine, f.getAbsolutePath(), mode, 2)
                        : Native808.exportWav(engine, f.getAbsolutePath(), mode, 2, 2,
                        k == 2 ? 1 : 0);
            } finally {
                jobs.decrementAndGet();
            }
            final long size = k == 2 ? 0 : f.length();
            postMain(new Runnable() {
                @Override
                public void run() {
                    exportBusy = false;
                    if (rc == 0) {
                        msg = k == 2 ? ("STEMS EXPORTED · 16 WAV IN exports/")
                                : (f.getName() + " · " + (size / 1024) + " KB");
                    } else {
                        msg = "EXPORT FAILED " + rc;
                    }
                }
            });
        }, "808-export").start();
    }

    private void postMain(Runnable r) {
        if (Looper.myLooper() == Looper.getMainLooper()) r.run();
        else handler.post(r);
    }
}
