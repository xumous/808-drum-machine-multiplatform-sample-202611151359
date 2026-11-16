// ============================================================================
//  Xu808 MainForm.cs —— 主窗口（C# 版）
//
//  徐某把所有控件都接在 XuEngine 上：
//    · 拖参数 → 直接改 project.Pads 的 float 字段（音频线程下一块就能读到）；
//    · 点格子 → 改 XuStep；
//    · 走带/切歌 → engine.Play/Stop/RequestPattern；
//    · 定时器 33ms → 只刷新播放头/电平/CPU，绝不参与音序。
// ============================================================================

using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace Xu808
{
    public sealed class XuMainForm : Form
    {
        private readonly XuProject project;
        private readonly XuEngine engine;
        private XuWaveOut waveOut;
        private XuMidiIn midi;
        private XuGridPanel grid;
        private Panel gridHost;
        private Panel stripHost;
        private ComboBox cbPattern;
        private NumericUpDown spBpm, spSteps;
        private TrackBar tbBpm, tbSwing;
        private Label lbSwing, lbStatus, lbMidi;
        private XuMeter meter;
        private readonly System.Windows.Forms.Timer timer = new();
        private readonly ToolTip tips = new();
        private bool loadingUi;

        public XuMainForm(XuProject p)
        {
            project = p;
            engine = new XuEngine(p);
            Text = "XU-808 鼓机 - 徐某";
            ClientSize = new Size(1500, 900);
            MinimumSize = new Size(1100, 720);
            BackColor = XuTheme.Bg;
            ForeColor = XuTheme.Text;
            Font = XuTheme.FontNormal;
            KeyPreview = true;
            try { Icon = new Icon(Path.Combine(AppContext.BaseDirectory, "app.ico")); } catch { }

            var root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 1,
                RowCount = 4,
                BackColor = XuTheme.Bg
            };
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 64));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 336));
            root.RowStyles.Add(new RowStyle(SizeType.Absolute, 100));
            Controls.Add(root);

            root.Controls.Add(BuildTop(), 0, 0);
            BuildGrid(root);
            BuildStrips(root);
            root.Controls.Add(BuildMaster(), 0, 3);

            StartAudio();
            StartMidi();

            // 33ms ≈ 30fps；即使界面忙，音序也由音频回调推进，不受影响
            timer.Interval = 33;
            timer.Tick += OnTick;
            timer.Start();
        }

        // ------------------------------------------------------------------
        // 顶部走带条
        // ------------------------------------------------------------------
        private Control BuildTop()
        {
            var top = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false,
                BackColor = XuTheme.Panel,
                Padding = new Padding(6, 4, 6, 4)
            };
            var logoBox = new Panel { Width = 210, Height = 52 };
            logoBox.Controls.Add(new Label
            {
                Text = "XU-808", AutoSize = true, Left = 0, Top = 0,
                Font = XuTheme.FontLogo, ForeColor = XuTheme.Orange
            });
            logoBox.Controls.Add(new Label
            {
                Text = "RHYTHM COMPOSER · 徐某 · 16 VOICE ANALOG",
                AutoSize = true, Left = 2, Top = 34, Font = new Font("Microsoft YaHei UI", 7f),
                ForeColor = XuTheme.Dim
            });
            top.Controls.Add(logoBox);

            var bPlay = BigBtn("▶ PLAY");
            var bStop = BigBtn("■ STOP");
            bPlay.Click += (_, __) => engine.Play();
            bStop.Click += (_, __) => { engine.Stop(); grid?.SetPlayhead(-1); };
            top.Controls.Add(bPlay);
            top.Controls.Add(bStop);

            top.Controls.Add(new Label { Text = "BPM", ForeColor = XuTheme.Dim, AutoSize = true, Padding = new Padding(8, 16, 0, 0) });
            spBpm = new NumericUpDown
            {
                DecimalPlaces = 1, Minimum = 40, Maximum = 300, Increment = 0.5M,
                Value = (decimal)project.Master.Bpm, Width = 70,
                BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text, BorderStyle = BorderStyle.FixedSingle
            };
            spBpm.ValueChanged += (_, __) =>
            {
                if (loadingUi) return;
                project.Master.Bpm = (float)spBpm.Value;
                tbBpm.Value = Math.Clamp((int)spBpm.Value, 40, 300);
            };
            top.Controls.Add(spBpm);
            tbBpm = new TrackBar { Minimum = 40, Maximum = 300, Value = (int)project.Master.Bpm, Width = 110, Height = 34, TickStyle = TickStyle.None };
            tbBpm.ValueChanged += (_, __) => { if (!loadingUi) spBpm.Value = tbBpm.Value; };
            top.Controls.Add(tbBpm);

            var bTap = BigBtn("TAP");
            bTap.Width = 50;
            bTap.Click += (_, __) => OnTap();
            top.Controls.Add(bTap);

            top.Controls.Add(new Label { Text = "SWING", ForeColor = XuTheme.Dim, AutoSize = true, Padding = new Padding(8, 16, 0, 0) });
            tbSwing = new TrackBar { Minimum = 0, Maximum = 75, Value = (int)(project.Master.Swing * 100), Width = 110, Height = 34, TickStyle = TickStyle.None };
            lbSwing = new Label { Text = tbSwing.Value + "%", ForeColor = XuTheme.Orange, AutoSize = true, Width = 34, Padding = new Padding(0, 18, 0, 0) };
            tbSwing.ValueChanged += (_, __) =>
            {
                if (loadingUi) return;
                project.Master.Swing = tbSwing.Value / 100f;
                lbSwing.Text = tbSwing.Value + "%";
            };
            top.Controls.Add(tbSwing);
            top.Controls.Add(lbSwing);

            top.Controls.Add(new Label { Text = "PATTERN", ForeColor = XuTheme.Dim, AutoSize = true, Padding = new Padding(8, 16, 0, 0) });
            cbPattern = new ComboBox
            {
                Width = 130, DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text
            };
            for (int i = 0; i < project.Patterns.Count; i++)
                cbPattern.Items.Add($"{i + 1:00} {project.Patterns[i].Name}");
            cbPattern.SelectedIndex = project.CurrentPattern;
            cbPattern.SelectedIndexChanged += (_, __) =>
            {
                if (loadingUi) return;
                engine.RequestPattern(cbPattern.SelectedIndex);
                spSteps.Value = project.Current.Steps;
                grid.RebuildSize();
                SyncStrips();
            };
            top.Controls.Add(cbPattern);

            spSteps = new NumericUpDown
            {
                Minimum = 1, Maximum = 64, Value = project.Current.Steps, Width = 54,
                BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text, BorderStyle = BorderStyle.FixedSingle
            };
            tips.SetToolTip(spSteps, "Pattern 步数（1-64）");
            spSteps.ValueChanged += (_, __) =>
            {
                if (loadingUi) return;
                project.Current.Steps = (int)spSteps.Value;
                grid.RebuildSize();
            };
            top.Controls.Add(spSteps);

            var cbSong = new CheckBox
            {
                Text = "SONG", ForeColor = XuTheme.Text, Width = 70, Height = 40,
                Checked = project.Master.SongMode, Padding = new Padding(4, 10, 0, 0)
            };
            cbSong.CheckedChanged += (_, __) => project.Master.SongMode = cbSong.Checked;
            top.Controls.Add(cbSong);

            var bSong = BigBtn("歌曲链…");
            bSong.Width = 80;
            bSong.Click += (_, __) => { using var d = new XuSongDialog(project); d.ShowDialog(this); };
            top.Controls.Add(bSong);

            var bSave = BigBtn("保存");
            var bLoad = BigBtn("打开");
            var bWav = BigBtn("导出WAV");
            var bMid = BigBtn("导出MIDI");
            bSave.Width = 60; bLoad.Width = 60; bWav.Width = 78; bMid.Width = 84;
            bSave.Click += (_, __) => OnSave();
            bLoad.Click += (_, __) => OnLoad();
            bWav.Click += (_, __) => OnExportWav();
            bMid.Click += (_, __) => OnExportMidi();
            top.Controls.Add(bSave); top.Controls.Add(bLoad); top.Controls.Add(bWav); top.Controls.Add(bMid);

            lbMidi = new Label { Text = "MIDI: -", ForeColor = XuTheme.Dim, AutoSize = true, Padding = new Padding(8, 18, 0, 0) };
            top.Controls.Add(lbMidi);
            return top;
        }

        private static Button BigBtn(string text) => new Button
        {
            Text = text, Width = 88, Height = 40,
            FlatStyle = FlatStyle.Flat,
            BackColor = XuTheme.Panel2,
            ForeColor = XuTheme.Text,
            Font = XuTheme.FontBold,
            Margin = new Padding(3, 5, 3, 5)
        };

        // ------------------------------------------------------------------
        private void BuildGrid(TableLayoutPanel root)
        {
            gridHost = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = XuTheme.Bg };
            grid = new XuGridPanel(project);
            grid.ToggleStep += OnToggleStep;
            grid.EditStep += OnEditStep;
            grid.HitPad += pad => engine.TriggerPad(pad, 112, false);
            grid.ToggleMute += OnToggleMute;
            grid.ToggleSolo += OnToggleSolo;
            gridHost.Controls.Add(grid);
            root.Controls.Add(gridHost, 0, 1);
        }

        private void BuildStrips(TableLayoutPanel root)
        {
            stripHost = new Panel { Dock = DockStyle.Fill, AutoScroll = true, BackColor = XuTheme.Bg };
            root.Controls.Add(stripHost, 0, 2);
            RebuildStrips();
        }

        private void RebuildStrips()
        {
            stripHost.Controls.Clear();
            int x = 2;
            for (int i = 0; i < 16; i++)
            {
                var s = new XuChannelStrip(i, project) { Left = x, Top = 2 };
                s.HitPad += pad => engine.TriggerPad(pad, 112, false);
                s.Changed += () => { };
                stripHost.Controls.Add(s);
                x += 76;
            }
        }

        private void SyncStrips()
        {
            foreach (Control c in stripHost.Controls)
                if (c is XuChannelStrip s) s.SyncFromProject();
        }

        // ------------------------------------------------------------------
        private Control BuildMaster()
        {
            var panel = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false, BackColor = XuTheme.Panel, Padding = new Padding(8, 6, 8, 4)
            };
            AddMasterSlider(panel, "LP 低通", 200, 20000, (int)project.Master.LpCutoff, v => project.Master.LpCutoff = v, 130);
            AddMasterSlider(panel, "RES 共鸣", 50, 800, (int)(project.Master.LpQ * 100), v => project.Master.LpQ = v / 100f, 90);
            AddMasterSlider(panel, "HP 高通", 10, 800, (int)project.Master.HpCutoff, v => project.Master.HpCutoff = v, 90);
            AddMasterSlider(panel, "DRIVE 过载", 100, 800, (int)(project.Master.Drive * 100), v => project.Master.Drive = v / 100f, 110);
            AddMasterSlider(panel, "VOL 音量", 0, 100, (int)(project.Master.Volume * 100), v => project.Master.Volume = v / 100f, 100);
            meter = new XuMeter { Margin = new Padding(12, 16, 4, 0) };
            panel.Controls.Add(meter);
            lbStatus = new Label
            {
                Text = "CPU 0%  ·  XRUN 0", ForeColor = XuTheme.Dim,
                AutoSize = true, Padding = new Padding(10, 26, 0, 0), Font = XuTheme.FontSmall
            };
            panel.Controls.Add(lbStatus);
            return panel;
        }

        private static void AddMasterSlider(FlowLayoutPanel host, string label, int min, int max,
                                            int val, Action<int> setter, int width)
        {
            var box = new Panel { Width = width, Height = 82 };
            box.Controls.Add(new Label
            {
                Text = label, Left = 0, Top = 0, Width = width, Height = 14,
                ForeColor = XuTheme.Dim, Font = new Font("Microsoft YaHei UI", 7f)
            });
            var tb = new TrackBar
            {
                Minimum = min, Maximum = max, Value = Math.Clamp(val, min, max),
                Left = -6, Top = 14, Width = width + 12, Height = 34,
                TickStyle = TickStyle.None
            };
            var valueLabel = new Label
            {
                Text = val.ToString(CultureInfo.InvariantCulture), Left = 0, Top = 50,
                Width = width, Height = 14, ForeColor = XuTheme.Orange, Font = XuTheme.FontSmall
            };
            tb.ValueChanged += (_, __) => { setter(tb.Value); valueLabel.Text = tb.Value.ToString(); };
            box.Controls.Add(tb);
            box.Controls.Add(valueLabel);
            host.Controls.Add(box);
        }

        // ------------------------------------------------------------------
        // 运行期
        // ------------------------------------------------------------------
        private void StartAudio()
        {
            try
            {
                waveOut = new XuWaveOut(engine);
                waveOut.Start();
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "无法打开音频设备：\n" + e.Message +
                    "\n\n界面仍可使用，导出 WAV 不受影响。", "音频设备");
            }
        }

        private void StartMidi()
        {
            try
            {
                midi = new XuMidiIn();
                lbMidi.Text = midi.Ok ? "MIDI: 已连接" : "MIDI: 无设备";
                lbMidi.ForeColor = midi.Ok ? XuTheme.Orange : XuTheme.Dim;
            }
            catch { lbMidi.Text = "MIDI: 不可用"; }
        }

        private void OnTick(object sender, EventArgs e)
        {
            int step = engine.Playing ? engine.StepIndex : -1;
            grid.SetPlayhead(step);
            meter.L = engine.MeterL;
            meter.R = engine.MeterR;
            meter.Invalidate();
            lbStatus.Text = $"CPU {engine.Cpu * 100:0.0}%  ·  XRUN {engine.Xruns}";
            if (midi != null && midi.Ok)
            {
                while (midi.Queue.TryDequeue(out var m)) HandleMidi(m.status, m.d1, m.d2);
            }
        }

        private void HandleMidi(int status, int d1, int d2)
        {
            int cmd = status & 0xF0;
            if (cmd == 0x90 && d2 > 0)
            {
                int pad = Array.IndexOf(XuConst.MidiNotes, d1);
                if (pad >= 0) engine.TriggerPad(pad, d2, d2 >= 120);
            }
            else if (cmd == 0xB0)
            {
                if (d1 == 7) project.Master.Volume = Math.Clamp(d2 / 127f, 0f, 1f);
                else if (d1 == 74) project.Master.LpCutoff = (float)(20.0 * Math.Pow(1000.0, d2 / 127.0));
            }
            else if (cmd == 0xC0)
            {
                cbPattern.SelectedIndex = Math.Min(d2, cbPattern.Items.Count - 1);
            }
            else if (status == 0xFA) engine.Play();
            else if (status == 0xFC) engine.Stop();
            else if (status == 0xF8 && engine.ClockExternal) engine.ExternalClockTick();
        }

        // ------------------------------------------------------------------
        private void OnToggleStep(int pad, int step)
        {
            var st = project.Current.Data[pad][step];
            st.On = !st.On;
            if (st.On) engine.TriggerPad(pad, st.Velocity, st.Accent);
            grid.Invalidate();
        }

        private void OnEditStep(int pad, int step)
        {
            using var d = new XuStepDialog(project.Current.Data[pad][step]);
            if (d.ShowDialog(this) == DialogResult.OK)
            {
                grid.Invalidate();
            }
        }

        private void OnToggleMute(int pad)
        {
            project.Pads[pad].Mute = !project.Pads[pad].Mute;
            grid.Invalidate();
        }

        private void OnToggleSolo(int pad)
        {
            project.Pads[pad].Solo = !project.Pads[pad].Solo;
            grid.Invalidate();
        }

        private void OnTap()
        {
            // Tap Tempo：用最近几次点击平均，超时 2 秒重开
            if (tapTimes.Count > 0 && DateTime.Now.TotalSeconds() - tapTimes[^1] > 2.0) tapTimes.Clear();
            tapTimes.Add(DateTime.Now.TotalSeconds());
            if (tapTimes.Count > 8) tapTimes.RemoveAt(0);
            if (tapTimes.Count >= 2)
            {
                var iv = tapTimes.Zip(tapTimes.Skip(1), (a, b) => b - a).Where(x => x > 0.15 && x < 2.0).ToList();
                if (iv.Count >= 2) { iv.Remove(iv.Max()); iv.Remove(iv.Min()); }
                if (iv.Count > 0)
                    spBpm.Value = Math.Clamp((decimal)(60.0 / iv.Average()), 40m, 300m);
            }
        }
        private readonly List<double> tapTimes = new();

        // ------------------------------------------------------------------
        private void OnSave()
        {
            using var d = new SaveFileDialog { Filter = "XU-808 工程 (*.xu808)|*.xu808|JSON (*.json)|*.json" };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            try { project.Save(d.FileName); }
            catch (Exception e) { MessageBox.Show(this, "保存失败：" + e.Message); }
        }

        private void OnLoad()
        {
            using var d = new OpenFileDialog { Filter = "XU-808 工程 (*.xu808)|*.xu808|JSON (*.json)|*.json" };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            try
            {
                var np = XuProject.Load(d.FileName);
                engine.Stop();
                engine.Project = np;
                // 由于 project 字段是 readonly，加载后用“重开窗口”最稳妥：
                var f = new XuMainForm(np);
                f.Show();
                Close();
            }
            catch (Exception e) { MessageBox.Show(this, "加载失败：" + e.Message); }
        }

        private void OnExportWav()
        {
            using var d = new SaveFileDialog { Filter = "WAV 音频 (*.wav)|*.wav", FileName = "xu808.wav" };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            Cursor = Cursors.WaitCursor;
            try { engine.ExportWav(d.FileName, project.Master.SongMode); }
            catch (Exception e) { MessageBox.Show(this, "导出失败：" + e.Message); }
            finally { Cursor = Cursors.Default; }
        }

        private void OnExportMidi()
        {
            using var d = new SaveFileDialog { Filter = "MIDI (*.mid)|*.mid", FileName = "xu808.mid" };
            if (d.ShowDialog(this) != DialogResult.OK) return;
            try { engine.ExportMidi(d.FileName, project.Master.SongMode); }
            catch (Exception e) { MessageBox.Show(this, "导出失败：" + e.Message); }
        }

        // ------------------------------------------------------------------
        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == Keys.Space) { engine.Toggle(); return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        protected override void OnKeyPress(KeyPressEventArgs e)
        {
            // 16 键鼓垫：ZXCVBNMASDFGHJK
            const string keys = "ZXCVBNMASDFGHJK";
            char ch = char.ToUpperInvariant(e.KeyChar);
            int idx = keys.IndexOf(ch);
            if (idx >= 0) { engine.TriggerPad(idx, 110, false); e.Handled = true; return; }
            base.OnKeyPress(e);
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            // 顺序：停音频 → 关 MIDI → 释放；否则可能留下后台线程
            try
            {
                engine.Stop();
                waveOut?.Dispose();
                midi?.Dispose();
                timer.Stop();
            }
            finally { base.OnFormClosing(e); }
        }
    }

    internal static class XuTimeExt
    {
        /// <summary>把 DateTime 换成“秒”的小工具，Tap Tempo 用。</summary>
        public static double TotalSeconds(this DateTime t) => t.Ticks / (double)TimeSpan.TicksPerSecond;
    }
}
