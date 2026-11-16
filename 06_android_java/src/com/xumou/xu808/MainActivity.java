// ============================================================================
//  MainActivity.java —— XU-808 鼓机 Android 版控制器（MVC 中的 Controller）
//
//  徐某说明：
//    · Model   = XuProject（工程/音色/步进数据）+ XuEngine（音序/DSP/导出）
//    · View    = XuGridView（16×N 网格）+ XuMeterView（电平表）+ 代码构建的控制条
//    · Control = 本类：只做事件转发、参数写入与 UI 刷新，不写 DSP；
//    · 音频线程由 XuAudio 独立运行，本类每 33ms 读一次快照刷新界面（≥30fps），
//      绝不让 UI 阻塞音频；所有导出都在后台线程做。
//    · 不依赖 AndroidX / Gradle，纯 Java + 原生 View/Canvas。
// ============================================================================
package com.xumou.xu808;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class MainActivity extends Activity implements XuGridView.Listener {

    // ---- 规格书第 9 节配色 ----
    private static final int BG = 0xFF121212;
    private static final int PANEL = 0xFF1C1C1C;
    private static final int ACCENT = 0xFFFF7A18;
    private static final int ACCENT2 = 0xFFFFD166;
    private static final int PLAY = 0xFF4CC9F0;
    private static final int LINE = 0xFF2E2E2E;
    private static final int TXT = 0xFFE8E8E8;
    private static final int DIM = 0xFF9A9A9A;
    // Tap Tempo 记录最近点击时刻（毫秒）
    private final ArrayList<Long> tapTimes = new ArrayList<>();
    private final Handler ui = new Handler(Looper.getMainLooper());
    // ---- Model ----
    private XuProject proj;
    private XuEngine engine;
    private XuAudio audio;
    private XuMidiIn midi;
    // ---- View 引用 ----
    private XuGridView grid;
    private XuMeterView meter;
    private Button playBtn, songBtn, patBtn, stepsBtn, songChainBtn;
    private Button hpBtn, lpBtn;
    private TextView bpmValue, swingValue, statusText, padTitle;
    private Button padMuteBtn, padSoloBtn;
    private SeekBar bpmBar, swingBar;
    private LinearLayout bottomContent;
    private LinearLayout channelBox;
    private int selectedPad = 0;
    private boolean lastPlaying = false;
    private boolean uiReady = false;
    /**
     * 每 33ms 的 UI 刷新：只读引擎快照，绝不反向阻塞音频。
     */
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refreshUi();
            ui.postDelayed(this, 33);
        }
    };

    private static String fmt(float v) {
        if (Math.abs(v) >= 100 || v == Math.rint(v)) return String.valueOf(Math.round(v));
        return String.format(java.util.Locale.US, "%.2f", v);
    }

    // ==================================================================
    // 生命周期
    // ==================================================================
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        proj = new XuProject();
        engine = new XuEngine(proj);
        audio = new XuAudio(engine);

        setContentView(buildRoot());
        uiReady = true;

        // 先刷一次界面，再开音频（即使音频设备打不开，界面也能编辑）
        refreshUi();
        if (!audio.start()) {
            Toast.makeText(this, "音频设备打开失败：" + audio.getLastError(),
                    Toast.LENGTH_LONG).show();
        }
        midi = new XuMidiIn(this, engine, proj);
        midi.start();

        ui.post(tick);
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(tick);
        uiReady = false;
        if (midi != null) midi.close();
        if (audio != null) audio.stop();
        if (engine != null) engine.stop();
        super.onDestroy();
    }

    // ==================================================================
    // 界面构建
    // ==================================================================
    private View buildRoot() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        // 顶部传输条
        View transport = buildTransport();
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        tlp.setMargins(dp(6), dp(6), dp(6), dp(3));
        root.addView(transport, tlp);

        // 中部网格：占满剩余高度，最少 120dp
        grid = new XuGridView(this, proj, engine);
        grid.setListener(this);
        grid.setSelectedPad(0);
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        glp.setMargins(dp(6), dp(2), dp(6), dp(0));
        grid.setMinimumHeight((int) dp(120));
        root.addView(grid, glp);

        // 状态行
        statusText = tv("就绪", 9f, DIM, false);
        statusText.setSingleLine(true);
        statusText.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.setMargins(dp(10), dp(2), dp(10), dp(2));
        root.addView(statusText, slp);

        // 底部参数面板
        View bottom = buildBottom();
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) dp(212));
        blp.setMargins(dp(6), 0, dp(6), dp(6));
        root.addView(bottom, blp);
        return root;
    }

    /**
     * 顶部传输条：4 行紧凑布局，保证 360dp 宽的手机也放得下。
     */
    private View buildTransport() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(panelBg(PANEL, 10, LINE));
        box.setPadding(dp(7), dp(5), dp(7), dp(5));

        // ---- 第 1 行：LOGO / 播放 / SONG ----
        LinearLayout r1 = row();
        TextView logo = tv("XU-808", 19f, ACCENT, true);
        logo.setLetterSpacing(0.08f);
        r1.addView(logo);
        TextView sub = tv("鼓机 · 徐某", 8.5f, DIM, false);
        LinearLayout.LayoutParams subLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        subLp.setMargins(dp(5), dp(4), 0, 0);
        r1.addView(sub, subLp);

        View spacer = new View(this);
        r1.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1f));

        playBtn = button("播放", ACCENT, 0xFF101010);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams((int) dp(62), (int) dp(31));
        plp.setMargins(0, 0, dp(5), 0);
        r1.addView(playBtn, plp);
        playBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (engine.playing) engine.stop();
                else engine.play();
            }
        });

        songBtn = button("SONG 关", 0xFF2A2A2A, TXT);
        r1.addView(songBtn, new LinearLayout.LayoutParams((int) dp(66), (int) dp(31)));
        songBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                proj.master.songMode = !proj.master.songMode;
                refreshUi();
            }
        });
        box.addView(r1);

        // ---- 第 2 行：BPM + Swing ----
        LinearLayout r2 = row();
        r2.addView(tv("BPM", 8.5f, DIM, false), fixed(dp(28), 0));
        bpmBar = new SeekBar(this);
        bpmBar.setMax(260);                       // 40..300
        bpmBar.setProgress(Math.round(proj.master.bpm) - 40);
        bpmBar.setProgressTintList(ColorStateList.valueOf(ACCENT));
        bpmBar.setThumbTintList(ColorStateList.valueOf(ACCENT));
        r2.addView(bpmBar, new LinearLayout.LayoutParams(0, (int) dp(28), 1f));
        bpmValue = tv("120", 10f, TXT, true);
        bpmValue.setGravity(Gravity.END);
        r2.addView(bpmValue, fixed(dp(40), 0));
        bpmBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                proj.master.bpm = 40 + p;         // 改 BPM 不重置播放位置
                bpmValue.setText(String.valueOf(40 + p));
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar sb) {
            }
        });

        r2.addView(tv("SWG", 8.5f, DIM, false), fixed(dp(30), 0));
        swingBar = new SeekBar(this);
        swingBar.setMax(75);                      // swing 0–75%
        swingBar.setProgress(Math.round(proj.master.swing * 100));
        swingBar.setProgressTintList(ColorStateList.valueOf(ACCENT));
        swingBar.setThumbTintList(ColorStateList.valueOf(ACCENT));
        r2.addView(swingBar, new LinearLayout.LayoutParams(0, (int) dp(28), 1f));
        swingValue = tv("0%", 10f, TXT, true);
        swingValue.setGravity(Gravity.END);
        r2.addView(swingValue, fixed(dp(38), 0));
        swingBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                proj.master.swing = p / 100f;     // 奇数步偏移 stepSamples*swing*0.5
                swingValue.setText(p + "%");
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar sb) {
            }
        });
        box.addView(r2);

        // ---- 第 3 行：Pattern / 步数 / Tap / 歌曲链 ----
        LinearLayout r3 = row();
        patBtn = smallButton("PAT 01");
        stepsBtn = smallButton("16 步");
        Button tapBtn = smallButton("TAP");
        songChainBtn = smallButton("歌曲链");
        r3.addView(patBtn, weight());
        r3.addView(stepsBtn, weight());
        r3.addView(tapBtn, weight());
        r3.addView(songChainBtn, weight());
        patBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPatternDialog();
            }
        });
        stepsBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showStepsDialog();
            }
        });
        songChainBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showSongDialog();
            }
        });
        tapBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onTapTempo();
            }
        });
        box.addView(r3);

        // ---- 第 4 行：保存 / 载入 / 导出 / 设置 ----
        LinearLayout r4 = row();
        Button saveBtn = smallButton("保存");
        Button loadBtn = smallButton("载入");
        Button exportBtn = smallButton("导出 WAV/MIDI");
        Button setBtn = smallButton("设置");
        r4.addView(saveBtn, weight());
        r4.addView(loadBtn, weight());
        r4.addView(exportBtn, weight());
        r4.addView(setBtn, weight());
        saveBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveProject();
            }
        });
        loadBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                loadProject();
            }
        });
        exportBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showExportDialog();
            }
        });
        setBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showSettingsDialog();
            }
        });
        box.addView(r4);
        return box;
    }

    /**
     * 底部面板：通道参数 + 主效果，内容可滚动（矮屏也能用）。
     */
    private View buildBottom() {
        ScrollView sv = new ScrollView(this);
        sv.setBackground(panelBg(PANEL, 10, LINE));
        sv.setFillViewport(false);
        bottomContent = new LinearLayout(this);
        bottomContent.setOrientation(LinearLayout.VERTICAL);
        bottomContent.setPadding(dp(7), dp(5), dp(7), dp(5));
        sv.addView(bottomContent, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        buildBottomSections(bottomContent);
        return sv;
    }

    /**
     * 构建底部所有小节：选中声部参数 / 主滤波 / 过载 / 音量 / 电平。
     */
    private void buildBottomSections(LinearLayout root) {
        // ---- 选中声部标题 + M/S ----
        LinearLayout header = row();
        padTitle = tv("BD 底鼓", 10.5f, ACCENT, true);
        padTitle.setSingleLine(true);
        header.addView(padTitle, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        padMuteBtn = smallButton("M");
        padSoloBtn = smallButton("S");
        header.addView(padMuteBtn, fixed(dp(34), dp(26)));
        header.addView(padSoloBtn, fixed(dp(34), dp(26)));
        root.addView(header);
        padMuteBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                XuProject.Pad p = proj.pads[selectedPad];
                p.mute = !p.mute;
                refreshMixButtons();
                grid.invalidate();
            }
        });
        padSoloBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                XuProject.Pad p = proj.pads[selectedPad];
                p.solo = !p.solo;
                refreshMixButtons();
                grid.invalidate();
            }
        });

        channelBox = new LinearLayout(this);
        channelBox.setOrientation(LinearLayout.VERTICAL);
        root.addView(channelBox);
        rebuildChannel();

        // ---- 分隔线 ----
        View div = new View(this);
        div.setBackgroundColor(LINE);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, (int) dp(1)));
        dlp.setMargins(0, dp(4), 0, dp(3));
        root.addView(div, dlp);

        // ---- 主输出标题 + HP/LP 开关 ----
        LinearLayout mh = row();
        mh.addView(tv("主输出 · 滤波 / 过载 / 音量", 9.5f, TXT, true),
                new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        hpBtn = smallButton("HP");
        lpBtn = smallButton("LP");
        mh.addView(hpBtn, fixed(dp(38), dp(24)));
        mh.addView(lpBtn, fixed(dp(38), dp(24)));
        root.addView(mh);
        hpBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                proj.master.hpOn = !proj.master.hpOn;
                refreshFilterButtons();
            }
        });
        lpBtn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                proj.master.lpOn = !proj.master.lpOn;
                refreshFilterButtons();
            }
        });

        // ---- 主参数两列 ----
        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout c1 = column(), c2 = column();
        XuProject.Master m = proj.master;

        c1.addView(floatRow("HP 截止", 20, 800, m.hpCutoff, true, new Sink() {
            @Override
            public void on(float v) {
                proj.master.hpCutoff = v;
            }
        }));
        c1.addView(floatRow("LP 截止", 200, 20000, m.lpCutoff, true, new Sink() {
            @Override
            public void on(float v) {
                proj.master.lpCutoff = v;
            }
        }));
        c1.addView(floatRow("LP 共鸣", 0.5f, 8, m.lpQ, false, new Sink() {
            @Override
            public void on(float v) {
                proj.master.lpQ = v;
            }
        }));

        c2.addView(floatRow("Drive", 1, 8, m.drive, false, new Sink() {
            @Override
            public void on(float v) {
                proj.master.drive = v;
            }
        }));
        c2.addView(floatRow("主音量", 0, 1, m.volume, false, new Sink() {
            @Override
            public void on(float v) {
                proj.master.volume = v;
            }
        }));

        // 右列第三个位置放电平表
        LinearLayout meterRow = new LinearLayout(this);
        meterRow.setOrientation(LinearLayout.HORIZONTAL);
        meterRow.setGravity(Gravity.CENTER_VERTICAL);
        meterRow.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) dp(24)));
        meterRow.addView(tv("电平", 8.5f, DIM, false), fixed(dp(38), 0));
        meter = new XuMeterView(this);
        meterRow.addView(meter, new LinearLayout.LayoutParams(0, (int) dp(12), 1f));
        c2.addView(meterRow);

        cols.addView(c1, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        cols.addView(c2, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(cols);

        refreshFilterButtons();
        refreshMixButtons();
    }

    /**
     * 只重建“当前声部”的 7 个参数条；主输出控件保持不动。
     */
    private void rebuildChannel() {
        if (channelBox == null) return;
        channelBox.removeAllViews();
        final XuProject.Pad p = proj.pads[selectedPad];
        if (padTitle != null) padTitle.setText(XuProject.CODES[selectedPad] + " " +
                XuProject.NAMES_CN[selectedPad]);

        LinearLayout cols = new LinearLayout(this);
        cols.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout c1 = column(), c2 = column();

        c1.addView(floatRow("Tune", 0, 1, p.tune, false, new Sink() {
            @Override
            public void on(float v) {
                p.tune = v;
            }
        }));
        c1.addView(floatRow("Decay", 0, 1, p.decay, false, new Sink() {
            @Override
            public void on(float v) {
                p.decay = v;
            }
        }));
        c1.addView(floatRow("Tone", 0, 1, p.tone, false, new Sink() {
            @Override
            public void on(float v) {
                p.tone = v;
            }
        }));
        c1.addView(floatRow("Snappy", 0, 1, p.snappy, false, new Sink() {
            @Override
            public void on(float v) {
                p.snappy = v;
            }
        }));
        c2.addView(floatRow("Tail", 0, 1, p.tail, false, new Sink() {
            @Override
            public void on(float v) {
                p.tail = v;
            }
        }));
        c2.addView(floatRow("Level", 0, 1.25f, p.level, false, new Sink() {
            @Override
            public void on(float v) {
                p.level = v;
            }
        }));
        c2.addView(floatRow("Pan", 0, 1, p.pan, false, new Sink() {
            @Override
            public void on(float v) {
                p.pan = v;
            }
        }));

        cols.addView(c1, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        cols.addView(c2, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        channelBox.addView(cols);
        refreshMixButtons();
    }

    // ==================================================================
    // 控件工厂（全部 dp 单位，避免不同密度下错位）
    // ==================================================================
    private float density() {
        return getResources().getDisplayMetrics().density;
    }

    private int dp(float v) {
        return (int) (v * density() + 0.5f);
    }

    private TextView tv(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        t.setIncludeFontPadding(false);
        return t;
    }

    private LinearLayout row() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    private LinearLayout column() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setLayoutParams(new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        return l;
    }

    private LinearLayout.LayoutParams fixed(float w, float h) {
        return new LinearLayout.LayoutParams(w == 0 ? 0 : (int) w,
                h == 0 ? ViewGroup.LayoutParams.WRAP_CONTENT : (int) h);
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                (int) dp(28), 1f);
        lp.setMargins(dp(2), dp(2), dp(2), 0);
        return lp;
    }

    private GradientDrawable panelBg(int color, float radiusDp, int strokeColor) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radiusDp));
        g.setStroke(Math.max(1, dp(1)), strokeColor);
        return g;
    }

    private Button button(String text, int bgColor, int textColor) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(10.5f);
        b.setTextColor(textColor);
        b.setAllCaps(false);
        b.setPadding(dp(4), 0, dp(4), 0);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setBackground(panelBg(bgColor, 8, bgColor == ACCENT ? ACCENT : LINE));
        return b;
    }

    private Button smallButton(String text) {
        Button b = button(text, 0xFF2A2A2A, TXT);
        b.setTextSize(9f);
        b.setPadding(dp(2), 0, dp(2), 0);
        return b;
    }

    /**
     * 通用浮点参数行：标签 + SeekBar + 数值，支持对数刻度（滤波截止）。
     */
    private View floatRow(String name, final float min, final float max,
                          float value, final boolean log, final Sink sink) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) dp(24)));

        TextView lab = tv(name, 8.5f, DIM, false);
        row.addView(lab, fixed(dp(40), 0));

        final SeekBar bar = new SeekBar(this);
        bar.setMax(1000);
        int prog = valueToProgress(value, min, max, log);
        bar.setProgress(prog);
        bar.setProgressTintList(ColorStateList.valueOf(ACCENT));
        bar.setThumbTintList(ColorStateList.valueOf(ACCENT));
        row.addView(bar, new LinearLayout.LayoutParams(0, (int) dp(22), 1f));

        final TextView val = tv(fmt(value), 8.5f, TXT, false);
        val.setGravity(Gravity.END);
        row.addView(val, fixed(dp(40), 0));

        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                float t = p / 1000f;
                float v = log ? (float) (min * Math.pow(max / min, t))
                        : min + t * (max - min);
                v = Math.max(min, Math.min(max, v));
                val.setText(fmt(v));
                sink.on(v);
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar sb) {
            }
        });
        return row;
    }

    private int valueToProgress(float v, float min, float max, boolean log) {
        float t = log
                ? (float) (Math.log(Math.max(min, v) / min) / Math.log(max / min))
                : (v - min) / (max - min);
        t = Math.max(0, Math.min(1, t));
        return Math.round(t * 1000);
    }

    // ==================================================================
    // XuGridView.Listener：由 View 转发用户操作
    // ==================================================================
    @Override
    public void onStepTap(int pad, int step) {
        XuProject.Pattern pat = proj.cur();
        XuProject.Step st = pat.data[pad][step];
        st.on = !st.on;
        if (st.on) engine.triggerPad(pad, st.velocity, st.accent);  // 写入即试听
        grid.invalidate();
    }

    @Override
    public void onStepLongPress(int pad, int step, float x, float y) {
        showStepDialog(pad, step);
    }

    @Override
    public void onPadSelected(int pad) {
        // 点左侧声部名或格子：切换底部参数面板；试听由 onPadAudition 单独负责
        if (selectedPad == pad) return;
        selectedPad = pad;
        grid.setSelectedPad(pad);
        rebuildChannel();
    }

    @Override
    public void onPadAudition(int pad) {
        // 试听该声部默认力度（单击即响）
        engine.triggerPad(pad, 100, false);
    }

    @Override
    public void onMuteToggle(int pad) {
        proj.pads[pad].mute = !proj.pads[pad].mute;
        grid.invalidate();
        refreshMixButtons();
    }

    @Override
    public void onSoloToggle(int pad) {
        proj.pads[pad].solo = !proj.pads[pad].solo;
        grid.invalidate();
        refreshMixButtons();
    }

    @Override
    public void onTrackLenLongPress(int pad) {
        showTrackLenDialog(pad);
    }

    // ==================================================================
    // 步进属性 / 轨道长度 / Pattern / 步数 对话框
    // ==================================================================
    private void showStepDialog(int pad, int step) {
        final XuProject.Step st = proj.cur().data[pad][step];
        LinearLayout box = dialogBox();

        final CheckBox onBox = new CheckBox(this);
        onBox.setText("开启（ON）");
        onBox.setTextColor(TXT);
        onBox.setChecked(st.on);
        box.addView(onBox);

        final CheckBox accBox = new CheckBox(this);
        accBox.setText("重音（音量 +35% / 包络 +20% / 音色变化）");
        accBox.setTextColor(TXT);
        accBox.setTextSize(11f);
        accBox.setChecked(st.accent);
        box.addView(accBox);

        final SeekBar vel = intSlider(box, "力度", 1, 127, st.velocity, "");
        final SeekBar prob = intSlider(box, "概率", 0, 100, st.prob, "%");
        final SeekBar micro = intSlider(box, "微时", -2400, 2400, st.micro, " 样本");
        final SeekBar ratchet = intSlider(box, "Ratchet", 1, 8, st.ratchet, " 连击");

        // 条件触发下拉
        LinearLayout condRow = row();
        condRow.addView(tv("条件", 9f, DIM, false), fixed(dp(40), 0));
        final Spinner cond = new Spinner(this);
        ArrayAdapter<String> ca = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, XuProject.COND_NAMES);
        ca.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        cond.setAdapter(ca);
        cond.setSelection(Math.max(0, Math.min(4, st.cond)));
        condRow.addView(cond, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(condRow);

        box.addView(tv("提示：正微时延后，负微时提前；Ratchet 子事件力度逐级 -8%。",
                8.5f, DIM, false));

        new AlertDialog.Builder(this)
                .setTitle(XuProject.CODES[pad] + " 第 " + (step + 1) + " 步属性")
                .setView(wrapDialog(box))
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        st.on = onBox.isChecked();
                        st.accent = accBox.isChecked();
                        st.velocity = 1 + vel.getProgress();
                        st.prob = prob.getProgress();
                        st.micro = -2400 + micro.getProgress();
                        st.ratchet = 1 + ratchet.getProgress();
                        st.cond = cond.getSelectedItemPosition();
                        grid.invalidate();
                        // 改完立刻试听一下，确认力度/重音/ratchet 的效果
                        if (st.on) engine.triggerPad(pad, st.velocity, st.accent);
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showTrackLenDialog(final int pad) {
        LinearLayout box = dialogBox();
        final XuProject.Pattern pat = proj.cur();
        final SeekBar len = intSlider(box, "轨道长度", 1, 64, pat.trackLen[pad], " 步");
        box.addView(tv("该轨按自己的步数循环（polymeter），与 Pattern 总步数独立。",
                8.5f, DIM, false));
        new AlertDialog.Builder(this)
                .setTitle(XuProject.CODES[pad] + " 轨道长度")
                .setView(wrapDialog(box))
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        pat.trackLen[pad] = 1 + len.getProgress();
                        grid.invalidate();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showStepsDialog() {
        LinearLayout box = dialogBox();
        final XuProject.Pattern pat = proj.cur();
        final EditText name = new EditText(this);
        name.setText(pat.name);
        name.setTextColor(TXT);
        name.setHint("Pattern 名称");
        name.setHintTextColor(DIM);
        name.setSingleLine(true);
        box.addView(name);
        final SeekBar steps = intSlider(box, "步数", 1, 64, pat.steps, " 步");
        new AlertDialog.Builder(this)
                .setTitle("Pattern 步数 / 名称")
                .setView(wrapDialog(box))
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        pat.steps = 1 + steps.getProgress();
                        if (!TextUtils.isEmpty(name.getText().toString().trim()))
                            pat.name = name.getText().toString().trim();
                        grid.invalidate();
                        refreshUi();
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void showPatternDialog() {
        LinearLayout box = dialogBox();
        for (int r = 0; r < 4; r++) {
            LinearLayout row = row();
            for (int c = 0; c < 4; c++) {
                final int idx = r * 4 + c;
                Button b = smallButton("P" + (idx < 9 ? "0" : "") + (idx + 1));
                b.setLayoutParams(new LinearLayout.LayoutParams(0, dp(40), 1f));
                b.setTextSize(11f);
                if (idx == proj.current) b.setTextColor(ACCENT);
                b.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        engine.requestPattern(idx);      // 播放中下一步边界生效
                        patBtn.setText("PAT " + (idx + 1));
                        grid.invalidate();
                    }
                });
                row.addView(b);
            }
            box.addView(row);
        }
        box.addView(tv("播放中切换 Pattern 会在下一步边界生效，不截断当前步。",
                8.5f, DIM, false));
        new AlertDialog.Builder(this)
                .setTitle("选择 Pattern")
                .setView(wrapDialog(box))
                .setPositiveButton("关闭", null)
                .show();
    }

    private void showSongDialog() {
        LinearLayout box = dialogBox();
        final LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list);

        final List<Spinner> spins = new ArrayList<>();
        final List<EditText> reps = new ArrayList<>();
        final List<LinearLayout> rows = new ArrayList<>();

        for (XuProject.SongEntry e : proj.song)
            addSongRow(list, spins, reps, rows, e.pattern, e.repeats);

        Button add = smallButton("＋ 添加一段");
        add.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                addSongRow(list, spins, reps, rows, 0, 2);
            }
        });
        box.addView(add, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(30)));
        box.addView(tv("Song 模式按列表顺序循环播放，每段可重复多次。", 8.5f, DIM, false));

        new AlertDialog.Builder(this)
                .setTitle("歌曲链（Song）")
                .setView(wrapDialog(box))
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        // 徐某：不在原列表上增删（音频线程可能正在遍历），
                        // 而是构建新列表后整体替换引用，避免并发修改异常。
                        ArrayList<XuProject.SongEntry> list2 = new ArrayList<>();
                        for (int i = 0; i < spins.size(); i++) {
                            int pat = spins.get(i).getSelectedItemPosition();
                            int rep = 1;
                            try {
                                rep = Integer.parseInt(reps.get(i).getText().toString());
                            } catch (Throwable ignored) {
                            }
                            rep = Math.max(1, Math.min(999, rep));
                            list2.add(new XuProject.SongEntry(pat, rep));
                        }
                        proj.song = list2;
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /**
     * 歌曲链一行：Pattern 下拉 + 重复次数 + 删除。
     */
    private void addSongRow(final LinearLayout list, final List<Spinner> spins,
                            final List<EditText> reps, final List<LinearLayout> rows,
                            int pattern, int repeat) {
        final LinearLayout row = row();
        final Spinner sp = new Spinner(this);
        List<String> items = new ArrayList<>();
        for (int i = 0; i < 16; i++)
            items.add("P" + (i < 9 ? "0" : "") + (i + 1));
        ArrayAdapter<String> ad = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(Math.max(0, Math.min(15, pattern)));
        row.addView(sp, new LinearLayout.LayoutParams(0, dp(38), 1f));

        final EditText et = new EditText(this);
        et.setText(String.valueOf(repeat));
        et.setTextColor(TXT);
        et.setTextSize(11f);
        et.setInputType(InputType.TYPE_CLASS_NUMBER);
        et.setSingleLine(true);
        row.addView(et, new LinearLayout.LayoutParams(dp(44), dp(38)));

        Button del = smallButton("删");
        row.addView(del, new LinearLayout.LayoutParams(dp(38), dp(30)));
        del.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                int i = rows.indexOf(row);
                if (i >= 0) {
                    rows.remove(i);
                    spins.remove(i);
                    reps.remove(i);
                    list.removeView(row);
                }
            }
        });

        rows.add(row);
        spins.add(sp);
        reps.add(et);
        list.addView(row);
    }

    // ==================================================================
    // 导出 / 保存 / 载入 / 设置
    // ==================================================================
    private void showExportDialog() {
        final String[] items = {
                "WAV · 单 Pattern", "WAV · 整曲 Song", "WAV · 16 分轨",
                "MIDI · 单 Pattern", "MIDI · 整曲 Song"
        };
        new AlertDialog.Builder(this)
                .setTitle("导出（48kHz / 16bit / 立体声）")
                .setItems(items, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        doExport(which);
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    /**
     * 后台线程导出：离线渲染复用同一套 DSP，绝不卡 UI。
     */
    private void doExport(final int kind) {
        statusText.setText("导出中，请稍候…");
        final File dir = exportDir();
        new Thread(new Runnable() {
            @Override
            public void run() {
                String msg;
                try {
                    if (dir == null) throw new IOException("外部存储不可用");
                    if (!dir.exists() && !dir.mkdirs()) throw new IOException("无法创建导出目录");
                    String path;
                    if (kind == 0) {
                        File f = new File(dir, "XU808_pattern.wav");
                        engine.exportWav(f, false);
                        path = f.getAbsolutePath();
                    } else if (kind == 1) {
                        File f = new File(dir, "XU808_song.wav");
                        engine.exportWav(f, true);
                        path = f.getAbsolutePath();
                    } else if (kind == 2) {
                        File sd = new File(dir, "stems");
                        engine.exportStems(sd, true);
                        path = sd.getAbsolutePath();
                    } else if (kind == 3) {
                        File f = new File(dir, "XU808_pattern.mid");
                        engine.exportMidi(f, false);
                        path = f.getAbsolutePath();
                    } else {
                        File f = new File(dir, "XU808_song.mid");
                        engine.exportMidi(f, true);
                        path = f.getAbsolutePath();
                    }
                    msg = "已导出：" + path;
                } catch (Throwable t) {
                    msg = "导出失败：" + t;
                }
                final String m = msg;
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        statusText.setText(m);
                        Toast.makeText(MainActivity.this, m, Toast.LENGTH_LONG).show();
                    }
                });
            }
        }, "XU808-Export").start();
    }

    private File exportDir() {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        return new File(base, "export");
    }

    private File projectFile() {
        return new File(getFilesDir(), "xu808_project.json");
    }

    private void saveProject() {
        try {
            proj.save(projectFile());
            String m = "已保存：" + projectFile().getAbsolutePath();
            statusText.setText(m);
            Toast.makeText(this, m, Toast.LENGTH_SHORT).show();
        } catch (Throwable t) {
            Toast.makeText(this, "保存失败：" + t, Toast.LENGTH_LONG).show();
        }
    }

    /**
     * 载入后重建 Model/音频，避免旧对象引用导致参数错位。
     */
    private void loadProject() {
        File f = projectFile();
        if (!f.exists()) {
            Toast.makeText(this, "还没有保存过工程", Toast.LENGTH_SHORT).show();
            return;
        }
        try {
            XuProject np = XuProject.load(f);
            proj = np;
            engine.stop();
            engine = new XuEngine(proj);
            audio.stop();
            audio = new XuAudio(engine);
            boolean ok = audio.start();
            if (midi != null) midi.close();
            midi = new XuMidiIn(this, engine, proj);
            midi.start();

            grid.setProject(proj, engine);
            selectedPad = 0;
            grid.setSelectedPad(0);

            // 顶部控件同步
            bpmBar.setProgress(Math.round(proj.master.bpm) - 40);
            swingBar.setProgress(Math.round(proj.master.swing * 100));
            bpmValue.setText(String.valueOf(Math.round(proj.master.bpm)));
            swingValue.setText(Math.round(proj.master.swing * 100) + "%");

            // 底部控件整体重建（主参数也来自新工程）
            bottomContent.removeAllViews();
            buildBottomSections(bottomContent);

            refreshUi();
            Toast.makeText(this, ok ? "工程已载入" : "工程已载入，但音频启动失败",
                    Toast.LENGTH_SHORT).show();
        } catch (Throwable t) {
            Toast.makeText(this, "载入失败：" + t, Toast.LENGTH_LONG).show();
        }
    }

    private void showSettingsDialog() {
        LinearLayout box = dialogBox();

        final SeekBar beats = intSlider(box, "每小节拍数", 1, 16,
                proj.master.beatsPerBar, " 拍");
        LinearLayout unitRow = row();
        unitRow.addView(tv("拍号分母", 9f, DIM, false), fixed(dp(64), 0));
        final Spinner unit = new Spinner(this);
        ArrayAdapter<String> ua = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, new String[]{"4", "8"});
        ua.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        unit.setAdapter(ua);
        unit.setSelection(proj.master.beatUnit == 8 ? 1 : 0);
        unitRow.addView(unit, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(unitRow);

        final CheckBox fill = new CheckBox(this);
        fill.setText("当前 Pattern 标记为过门（FILL）");
        fill.setTextColor(TXT);
        fill.setTextSize(11f);
        fill.setChecked(proj.cur().fill);
        box.addView(fill);

        LinearLayout seedRow = row();
        seedRow.addView(tv("随机种子", 9f, DIM, false), fixed(dp(64), 0));
        final EditText seed = new EditText(this);
        seed.setText(String.valueOf(proj.seed));
        seed.setInputType(InputType.TYPE_CLASS_NUMBER);
        seed.setTextColor(TXT);
        seed.setTextSize(11f);
        seed.setSingleLine(true);
        seedRow.addView(seed, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        box.addView(seedRow);

        box.addView(tv("XU-808 鼓机 · Android · 徐某\n"
                        + "48kHz / Float32 / 512 样本块 · 采样级音序\n"
                        + "设置仅影响拍号元数据与概率随机序列，不改变音色。",
                8.5f, DIM, false));

        new AlertDialog.Builder(this)
                .setTitle("设置")
                .setView(wrapDialog(box))
                .setPositiveButton("确定", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        proj.master.beatsPerBar = 1 + beats.getProgress();
                        proj.master.beatUnit = unit.getSelectedItemPosition() == 1 ? 8 : 4;
                        proj.cur().fill = fill.isChecked();
                        try {
                            proj.seed = Integer.parseInt(seed.getText().toString());
                        } catch (Throwable ignored) {
                        }
                    }
                })
                .setNegativeButton("取消", null)
                .show();
    }

    // ---- 对话框辅助 ----
    private LinearLayout dialogBox() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(14), dp(6), dp(14), dp(4));
        return box;
    }

    private View wrapDialog(View v) {
        ScrollView sv = new ScrollView(this);
        sv.addView(v);
        return sv;
    }

    /**
     * 整数滑杆行，返回 SeekBar，数值标签自动更新。
     */
    private SeekBar intSlider(LinearLayout parent, String name, final int min,
                              final int max, int value, final String suffix) {
        LinearLayout row = row();
        row.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(28)));
        row.addView(tv(name, 9f, DIM, false), fixed(dp(64), 0));
        final SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(Math.max(0, Math.min(max - min, value - min)));
        bar.setProgressTintList(ColorStateList.valueOf(ACCENT));
        bar.setThumbTintList(ColorStateList.valueOf(ACCENT));
        row.addView(bar, new LinearLayout.LayoutParams(0, dp(24), 1f));
        final TextView val = tv(value + suffix, 9f, TXT, false);
        val.setGravity(Gravity.END);
        row.addView(val, fixed(dp(62), 0));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                val.setText((min + p) + suffix);
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar sb) {
            }
        });
        parent.addView(row);
        return bar;
    }

    // ==================================================================
    // Tap Tempo
    // ==================================================================
    private void onTapTempo() {
        long now = System.currentTimeMillis();
        if (!tapTimes.isEmpty() && now - tapTimes.get(tapTimes.size() - 1) > 2500)
            tapTimes.clear();
        tapTimes.add(now);
        while (tapTimes.size() > 9) tapTimes.remove(0);
        if (tapTimes.size() < 3) {
            Toast.makeText(this, "继续点…（至少 3 次）", Toast.LENGTH_SHORT).show();
            return;
        }
        // 记录最近 8 个间隔，剔除 30–400 BPM 之外的异常值
        List<Double> intervals = new ArrayList<>();
        for (int i = 1; i < tapTimes.size(); i++) {
            double ms = tapTimes.get(i) - tapTimes.get(i - 1);
            double bpm = 60000.0 / ms;
            if (bpm >= 30 && bpm <= 400) intervals.add(ms);
        }
        if (intervals.size() < 2) return;
        // 去掉最大最小后平均（规格书第 5 节）
        Collections.sort(intervals);
        if (intervals.size() >= 4) {
            intervals.remove(0);
            intervals.remove(intervals.size() - 1);
        }
        double sum = 0;
        for (double d : intervals) sum += d;
        double bpm = 60000.0 / (sum / intervals.size());
        bpm = Math.max(40, Math.min(300, bpm));
        proj.master.bpm = (float) bpm;
        bpmBar.setProgress((int) Math.round(bpm) - 40);
        bpmValue.setText(String.valueOf(Math.round(bpm)));
        Toast.makeText(this, "Tap BPM = " + Math.round(bpm), Toast.LENGTH_SHORT).show();
    }

    // ==================================================================
    // 状态刷新（30fps）
    // ==================================================================
    private void refreshUi() {
        if (!uiReady) return;
        XuProject.Pattern pat = proj.cur();

        if (lastPlaying != engine.playing) {
            lastPlaying = engine.playing;
            playBtn.setText(engine.playing ? "停止" : "播放");
            playBtn.setBackground(panelBg(engine.playing ? 0xFFB23A2A : ACCENT,
                    8, engine.playing ? 0xFFB23A2A : ACCENT));
            playBtn.setTextColor(engine.playing ? Color.WHITE : 0xFF101010);
        }

        bpmValue.setText(String.valueOf(Math.round(proj.master.bpm)));
        swingValue.setText(Math.round(proj.master.swing * 100) + "%");
        patBtn.setText("PAT " + (proj.current + 1));
        stepsBtn.setText(pat.steps + " 步");
        songBtn.setText(proj.master.songMode ? "SONG 开" : "SONG 关");
        songBtn.setTextColor(proj.master.songMode ? ACCENT2 : TXT);

        if (meter != null) meter.setLevels(engine.meterL, engine.meterR);

        String audioState = audio.isRunning() ? "AUDIO 正常" : "AUDIO 停止";
        String m = "STEP " + (engine.playing ? (engine.getStepIndex() + 1) : 0)
                + "/" + pat.steps
                + " · CPU " + Math.round(engine.cpu * 100) + "%"
                + " · XRUN " + engine.xruns
                + " · " + audioState;
        statusText.setText(m);

        grid.invalidate();          // 播放头与参数实时反映
    }

    /**
     * 只更新 M/S 两个小按钮的颜色，避免每帧重建背景。
     */
    private void refreshMixButtons() {
        if (padMuteBtn == null || padSoloBtn == null) return;
        XuProject.Pad p = proj.pads[selectedPad];
        padMuteBtn.setTextColor(p.mute ? Color.WHITE : TXT);
        padMuteBtn.setBackground(panelBg(p.mute ? 0xFF9B3232 : 0xFF2A2A2A, 8,
                p.mute ? 0xFF9B3232 : LINE));
        padSoloBtn.setTextColor(p.solo ? Color.BLACK : TXT);
        padSoloBtn.setBackground(panelBg(p.solo ? ACCENT2 : 0xFF2A2A2A, 8,
                p.solo ? ACCENT2 : LINE));
        grid.invalidate();
    }

    private void refreshFilterButtons() {
        if (hpBtn == null || lpBtn == null) return;
        hpBtn.setTextColor(proj.master.hpOn ? Color.BLACK : DIM);
        hpBtn.setBackground(panelBg(proj.master.hpOn ? ACCENT2 : 0xFF2A2A2A, 8,
                proj.master.hpOn ? ACCENT2 : LINE));
        lpBtn.setTextColor(proj.master.lpOn ? Color.BLACK : DIM);
        lpBtn.setBackground(panelBg(proj.master.lpOn ? ACCENT2 : 0xFF2A2A2A, 8,
                proj.master.lpOn ? ACCENT2 : LINE));
        grid.invalidate();
    }

    private interface Sink {
        void on(float v);
    }
}
