// ============================================================================
//  Xu808 Ui.cs —— WinForms 界面（C# 版）
//
//  视觉目标：深色机身 + 橙色按钮，尽量贴近 TR-808。
//  技术上没有用任何第三方控件库，全部是 System.Windows.Forms 原生控件
//  加两个自绘控件（网格 GridPanel、竖滑杆 XuVSlider、电平表 XuMeter），
//  这样单文件发布时不会带上任何奇怪的 DLL。
// ============================================================================

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace Xu808
{
    // ------------------------------------------------------------------------
    //  颜色与字体
    // ------------------------------------------------------------------------
    internal static class XuTheme
    {
        public static readonly Color Bg = Color.FromArgb(18, 18, 18);
        public static readonly Color Panel = Color.FromArgb(28, 28, 28);
        public static readonly Color Panel2 = Color.FromArgb(35, 35, 35);
        public static readonly Color Line = Color.FromArgb(46, 46, 46);
        public static readonly Color Orange = Color.FromArgb(255, 122, 24);
        public static readonly Color OrangeDark = Color.FromArgb(168, 74, 12);
        public static readonly Color Yellow = Color.FromArgb(255, 209, 102);
        public static readonly Color Cyan = Color.FromArgb(76, 201, 240);
        public static readonly Color Text = Color.FromArgb(232, 232, 232);
        public static readonly Color Dim = Color.FromArgb(138, 138, 138);
        public static readonly Color Red = Color.FromArgb(229, 72, 77);

        public static readonly Font FontSmall = new Font("Microsoft YaHei UI", 8f);
        public static readonly Font FontNormal = new Font("Microsoft YaHei UI", 9f);
        public static readonly Font FontBold = new Font("Microsoft YaHei UI", 9f, FontStyle.Bold);
        public static readonly Font FontMono = new Font("Consolas", 9f, FontStyle.Bold);
        public static readonly Font FontLogo = new Font("Consolas", 20f, FontStyle.Bold);
    }

    /// <summary>竖向小滑杆：WinForms 自带 TrackBar 不支持竖向，只能自绘。</summary>
    public sealed class XuVSlider : Control
    {
        public int Min = 0, Max = 100;
        private int val;
        public event EventHandler ValueChanged;
        // WinForms 分析器要求自定义属性声明序列化方式；这个控件只在代码里用，
        // 不需要设计器保存，因此显式标记 Hidden，顺带消掉 WFO1000 报错。
        [System.ComponentModel.Browsable(false)]
        [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
        public int Value
        {
            get => val;
            set
            {
                int v = Math.Clamp(value, Min, Max);
                if (v != val) { val = v; ValueChanged?.Invoke(this, EventArgs.Empty); Invalidate(); }
            }
        }

        public XuVSlider()
        {
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
            Width = 16;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.Clear(XuTheme.Panel2);
            int x = Width / 2;
            // 轨道
            using (var b = new SolidBrush(Color.FromArgb(16, 16, 16)))
                g.FillRectangle(b, x - 3, 2, 6, Height - 4);
            // 已填充部分（从底部往上）
            int h = (int)((Height - 4) * (val - Min) / (double)Math.Max(1, Max - Min));
            using (var b = new SolidBrush(XuTheme.OrangeDark))
                g.FillRectangle(b, x - 3, Height - 2 - h, 6, h);
            // 手柄
            int hy = Height - 2 - h;
            using (var b = new SolidBrush(XuTheme.Orange))
                g.FillRectangle(b, x - 7, hy - 5, 14, 10);
            using (var p = new Pen(XuTheme.Line))
                g.DrawRectangle(p, x - 7, hy - 5, 14, 10);
        }

        protected override void OnMouseDown(MouseEventArgs e) { SetFromY(e.Y); }
        protected override void OnMouseMove(MouseEventArgs e) { if (e.Button == MouseButtons.Left) SetFromY(e.Y); }

        private void SetFromY(int y)
        {
            double k = 1.0 - Math.Clamp(y, 2, Height - 2) / (double)Math.Max(1, Height - 4);
            Value = Min + (int)Math.Round(k * (Max - Min));
        }
    }

    /// <summary>双声道电平表。</summary>
    public sealed class XuMeter : Control
    {
        public float L, R;
        public XuMeter()
        {
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
            Width = 150; Height = 22;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(Color.FromArgb(12, 12, 12));
            for (int i = 0; i < 2; i++)
            {
                float v = i == 0 ? L : R;
                int y = 3 + i * 9;
                g.FillRectangle(new SolidBrush(Color.FromArgb(26, 26, 26)), 2, y, Width - 4, 7);
                int w = (int)((Width - 4) * Math.Clamp(v, 0f, 1f));
                if (w > 0)
                {
                    // 绿→黄→红三段
                    using var lg = new LinearGradientBrush(new Rectangle(2, y, Math.Max(2, w), 7),
                                                           Color.FromArgb(61, 220, 132), Color.FromArgb(229, 72, 77), 0f);
                    var blend = new ColorBlend
                    {
                        Colors = new[] { Color.FromArgb(61, 220, 132), Color.FromArgb(255, 209, 102), Color.FromArgb(229, 72, 77) },
                        Positions = new[] { 0f, 0.72f, 1f }
                    };
                    lg.InterpolationColors = blend;
                    g.FillRectangle(lg, 2, y, w, 7);
                }
            }
            using var p = new Pen(XuTheme.Line);
            g.DrawRectangle(p, 1, 1, Width - 2, Height - 2);
        }
    }

    /// <summary>
    /// 步进网格。16 行 × N 列，自绘。
    /// </summary>
    public sealed class XuGridPanel : Control
    {
        public const int CellW = 30, CellH = 22, LabelW = 128, HeaderH = 22;
        // 字体只创建一次：如果在 OnPaint 里 new Font，GDI 句柄会不断泄漏，
        // 跑十几分钟后 Windows 会开始报“创建窗口句柄时出错”。徐某踩过。
        private static readonly Font CellFont = new Font("Consolas", 6f, FontStyle.Bold);
        public XuProject Project;
        public int Playhead = -1;
        public event Action<int, int> ToggleStep;
        public event Action<int, int> EditStep;
        public event Action<int> HitPad, ToggleMute, ToggleSolo;

        public XuGridPanel(XuProject p)
        {
            Project = p;
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
            RebuildSize();
        }

        public void RebuildSize()
        {
            int cols = Math.Max(1, Project.Current.Steps);
            Width = LabelW + cols * CellW;
            Height = HeaderH + 16 * CellH;
            Invalidate();
        }

        public void SetPlayhead(int s)
        {
            if (s != Playhead) { Playhead = s; Invalidate(); }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.Clear(XuTheme.Bg);
            var pat = Project.Current;
            int cols = Math.Max(1, pat.Steps);
            // 顶部步号
            using (var b = new SolidBrush(XuTheme.Panel))
                g.FillRectangle(b, 0, 0, Width, HeaderH);
            for (int c = 0; c < cols; c++)
            {
                int x = LabelW + c * CellW;
                if (c % 4 == 0)
                    g.FillRectangle(new SolidBrush(Color.FromArgb(58, 58, 58)), x, 0, 1, Height);
                TextRenderer.DrawText(g, (c + 1).ToString(), XuTheme.FontSmall,
                    new Rectangle(x, 0, CellW, HeaderH), XuTheme.Dim,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            }
            // 左侧声部区
            using (var b = new SolidBrush(XuTheme.Panel2))
                g.FillRectangle(b, 0, HeaderH, LabelW, Height - HeaderH);
            for (int r = 0; r < 16; r++)
            {
                int y = HeaderH + r * CellH;
                using (var b = new SolidBrush(r % 2 == 0 ? Color.FromArgb(22, 22, 22) : Color.FromArgb(26, 26, 26)))
                    g.FillRectangle(b, LabelW, y, Width - LabelW, CellH);
                TextRenderer.DrawText(g, XuConst.Codes[r], XuTheme.FontMono,
                    new Rectangle(6, y, 36, CellH), XuTheme.Orange,
                    TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
                TextRenderer.DrawText(g, XuConst.NamesCn[r], XuTheme.FontSmall,
                    new Rectangle(42, y, 46, CellH), XuTheme.Text,
                    TextFormatFlags.VerticalCenter | TextFormatFlags.Left);
                // M / S
                var pad = Project.Pads[r];
                for (int j = 0; j < 2; j++)
                {
                    bool on = j == 0 ? pad.Mute : pad.Solo;
                    int bx = 92 + j * 18;
                    using (var b = new SolidBrush(on ? (j == 0 ? XuTheme.Red : XuTheme.Yellow) : Color.FromArgb(42, 42, 42)))
                        g.FillRectangle(b, bx, y + 4, 15, CellH - 8);
                    TextRenderer.DrawText(g, j == 0 ? "M" : "S", XuTheme.FontSmall,
                        new Rectangle(bx, y + 4, 15, CellH - 8), on ? Color.FromArgb(18, 18, 18) : XuTheme.Dim,
                        TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                }
                using var pen = new Pen(XuTheme.Line);
                g.DrawLine(pen, 0, y + CellH - 1, Width, y + CellH - 1);
            }
            // 格子
            for (int c = 0; c < cols; c++)
            {
                int x = LabelW + c * CellW;
                for (int r = 0; r < 16; r++)
                {
                    var st = pat.Data[r][c];
                    int y = HeaderH + r * CellH;
                    var rect = new Rectangle(x + 2, y + 2, CellW - 4, CellH - 4);
                    if (st.On)
                    {
                        Color baseCol = st.Accent ? XuTheme.Yellow : XuTheme.Orange;
                        int alpha = (int)(110 + 145 * (st.Velocity / 127.0));
                        using var b = new SolidBrush(Color.FromArgb(alpha, baseCol));
                        g.FillRectangle(b, rect);
                        if (st.Prob < 100 || st.Ratchet > 1 || st.Micro != 0)
                        {
                            string txt = (st.Prob < 100 ? st.Prob + "%" : "") +
                                         (st.Ratchet > 1 ? "R" + st.Ratchet : "");
                            TextRenderer.DrawText(g, txt, CellFont,
                                rect, Color.FromArgb(26, 16, 8), TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                        }
                    }
                    else
                    {
                        using var b = new SolidBrush(Color.FromArgb(16, 16, 16));
                        g.FillRectangle(b, rect);
                        using var p = new Pen(Color.FromArgb(39, 39, 39));
                        g.DrawRectangle(p, rect);
                    }
                }
            }
            // 播放头
            if (Playhead >= 0 && Playhead < cols)
            {
                int x = LabelW + Playhead * CellW;
                using var b = new SolidBrush(Color.FromArgb(26, 76, 201, 240));
                g.FillRectangle(b, x, HeaderH, CellW, Height - HeaderH);
                using var p = new Pen(XuTheme.Cyan, 2);
                g.DrawLine(p, x, HeaderH, x, Height);
                g.DrawLine(p, x + CellW, HeaderH, x + CellW, Height);
                g.FillPolygon(new SolidBrush(XuTheme.Cyan),
                    new[] { new Point(x + 7, 0), new Point(x + CellW - 7, 0), new Point(x + CellW / 2, 10) });
            }
            using var border = new Pen(XuTheme.Line);
            g.DrawRectangle(border, 0, 0, Width - 1, Height - 1);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            int y = e.Y, x = e.X;
            if (y < HeaderH) return;
            int r = (y - HeaderH) / CellH;
            if (r < 0 || r >= 16) return;
            if (x < LabelW)
            {
                for (int j = 0; j < 2; j++)
                {
                    int bx = 92 + j * 18;
                    if (x >= bx && x <= bx + 15)
                    {
                        if (j == 0) ToggleMute?.Invoke(r); else ToggleSolo?.Invoke(r);
                        return;
                    }
                }
                HitPad?.Invoke(r);
                return;
            }
            int c = (x - LabelW) / CellW;
            if (c < 0 || c >= Math.Max(1, Project.Current.Steps)) return;
            if (e.Button == MouseButtons.Right) EditStep?.Invoke(r, c);
            else ToggleStep?.Invoke(r, c);
        }
    }

    /// <summary>一条通道条。</summary>
    public sealed class XuChannelStrip : Panel
    {
        public readonly int Pad;
        private readonly XuProject project;
        public event Action<int> HitPad;
        public event Action Changed;
        private readonly List<(XuVSlider slider, string attr, int max)> sliders = new();
        private CheckBox bm, bs;
        private NumericUpDown lenBox;

        public XuChannelStrip(int pad, XuProject p)
        {
            Pad = pad;
            project = p;
            Width = 74; Height = 330;
            BackColor = XuTheme.Bg;
            var name = new Button
            {
                Text = XuConst.Codes[pad] + "\n" + XuConst.NamesCn[pad],
                Top = 2, Left = 2, Width = 70, Height = 34,
                FlatStyle = FlatStyle.Flat,
                ForeColor = XuTheme.Orange,
                BackColor = XuTheme.Panel2,
                Font = XuTheme.FontSmall
            };
            name.FlatAppearance.BorderColor = Color.FromArgb(58, 42, 26);
            name.Click += (_, __) => HitPad?.Invoke(Pad);
            Controls.Add(name);
            // M / S
            bm = new CheckBox { Text = "M", Top = 38, Left = 4, Width = 30, Height = 18, ForeColor = XuTheme.Red, Font = XuTheme.FontSmall };
            bs = new CheckBox { Text = "S", Top = 38, Left = 38, Width = 30, Height = 18, ForeColor = XuTheme.Yellow, Font = XuTheme.FontSmall };
            bm.Checked = project.Pads[pad].Mute;
            bs.Checked = project.Pads[pad].Solo;
            bm.CheckedChanged += (_, __) => { project.Pads[pad].Mute = bm.Checked; Changed?.Invoke(); };
            bs.CheckedChanged += (_, __) => { project.Pads[pad].Solo = bs.Checked; Changed?.Invoke(); };
            Controls.Add(bm); Controls.Add(bs);

            int top = 58;
            foreach (var (label, attr) in ParamsFor(pad))
            {
                AddSlider(label, attr, ref top);
            }
            AddSlider("LEVEL", "level", ref top, 125);
            AddSlider("PAN", "pan", ref top);
            var len = new NumericUpDown
            {
                Top = top, Left = 6, Width = 60,
                Minimum = 1, Maximum = 64,
                Value = project.Current.TrackLen[pad],
                Font = XuTheme.FontSmall,
                BackColor = XuTheme.Panel2,
                ForeColor = XuTheme.Text,
                BorderStyle = BorderStyle.FixedSingle
            };
            len.ValueChanged += (_, __) =>
            {
                project.Current.TrackLen[Pad] = (int)len.Value;
                Changed?.Invoke();
            };
            lenBox = len;
            Controls.Add(len);
        }

        private static IEnumerable<(string, string)> ParamsFor(int pad)
        {
            return pad switch
            {
                0 => new[] { ("TUNE", "tune"), ("DECAY", "decay"), ("TONE", "tone") },
                1 => new[] { ("TUNE", "tune"), ("SNAP", "snappy"), ("DECAY", "decay") },
                2 or 3 or 4 => new[] { ("TUNE", "tune"), ("DECAY", "decay"), ("COLOR", "tone") },
                10 => new[] { ("DECAY", "decay"), ("TONE", "tone"), ("TAIL", "tail") },
                11 => new[] { ("DECAY", "decay"), ("TONE", "tone") },
                13 or 14 or 15 => new[] { ("DECAY", "decay"), ("TONE", "tone") },
                12 => new[] { ("TUNE", "tune"), ("DECAY", "decay") },
                _ => new[] { ("TUNE", "tune"), ("DECAY", "decay") }
            };
        }

        private void AddSlider(string label, string attr, ref int top, int max = 100)
        {
            var lab = new Label
            {
                Text = label, Top = top, Left = 2, Width = 70, Height = 12,
                TextAlign = ContentAlignment.MiddleCenter,
                ForeColor = XuTheme.Dim, Font = new Font("Microsoft YaHei UI", 6.5f)
            };
            Controls.Add(lab);
            var s = new XuVSlider { Top = top + 12, Left = 29, Width = 16, Height = 40, Max = max };
            // 用反射读写字段：属性名来自规格书，绝不能写错
            var pi = typeof(XuPadParams).GetField(attr);
            float cur = (float)pi.GetValue(project.Pads[Pad]);
            // 注意：初始值和写回都必须除以同一个 max，
            // 否则 LEVEL（max=125）会一拖动就跳到 0.8 而不是 1.0。
            s.Value = (int)Math.Round(cur * max);
            s.ValueChanged += (_, __) =>
            {
                pi.SetValue(project.Pads[Pad], s.Value / (float)max);
                Changed?.Invoke();
            };
            Controls.Add(s);
            sliders.Add((s, attr, max));
            top += 54;
        }

        public void SyncFromProject()
        {
            foreach (var (s, attr, max) in sliders)
            {
                var pi = typeof(XuPadParams).GetField(attr);
                s.Value = (int)Math.Round((float)pi.GetValue(project.Pads[Pad]) * max);
            }
            bm.Checked = project.Pads[Pad].Mute;
            bs.Checked = project.Pads[Pad].Solo;
            lenBox.Value = project.Current.TrackLen[Pad];
        }
    }

    /// <summary>步进属性对话框。</summary>
    public sealed class XuStepDialog : Form
    {
        public readonly XuStep Step;
        private readonly CheckBox cbOn, cbAcc;
        private readonly NumericUpDown spVel, spProb, spMicro, spRat;
        private readonly ComboBox cbCond;

        public XuStepDialog(XuStep st)
        {
            Step = st;
            Text = "步进属性 - 徐某的 808";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false; MinimizeBox = false;
            BackColor = XuTheme.Panel;
            ForeColor = XuTheme.Text;
            ClientSize = new Size(430, 330);
            Font = XuTheme.FontNormal;

            var lay = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Padding = new Padding(12) };
            lay.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 62));
            lay.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 38));
            Controls.Add(lay);

            cbOn = new CheckBox { Text = "启用", Checked = st.On, ForeColor = XuTheme.Text };
            cbAcc = new CheckBox { Text = "重音 Accent（音量+衰减+音色）", Checked = st.Accent, ForeColor = XuTheme.Yellow };
            lay.Controls.Add(cbOn); lay.SetColumnSpan(cbOn, 2);
            lay.Controls.Add(cbAcc); lay.SetColumnSpan(cbAcc, 2);

            spVel = Num(1, 127, st.Velocity);
            spProb = Num(0, 100, st.Prob);
            spMicro = Num(-2400, 2400, st.Micro);
            spRat = Num(1, 8, st.Ratchet);
            AddRow(lay, "力度 Velocity", spVel);
            AddRow(lay, "概率 Probability %", spProb);
            AddRow(lay, "微时 Micro（样本）", spMicro);
            AddRow(lay, "滚奏 Ratchet", spRat);

            cbCond = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text };
            cbCond.Items.AddRange(XuCond.Names);
            cbCond.SelectedIndex = Math.Clamp(st.Cond, 0, 4);
            AddRow(lay, "条件触发", cbCond);

            var ok = new Button { Text = "确定", DialogResult = DialogResult.OK, BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text, FlatStyle = FlatStyle.Flat };
            var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel, BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text, FlatStyle = FlatStyle.Flat };
            var btns = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Bottom, Height = 44 };
            btns.Controls.Add(ok); btns.Controls.Add(cancel);
            lay.Controls.Add(btns); lay.SetColumnSpan(btns, 2);
            AcceptButton = ok; CancelButton = cancel;
            ok.Click += (_, __) => Apply();
        }

        private static NumericUpDown Num(int min, int max, int v) => new NumericUpDown
        {
            Minimum = min, Maximum = max, Value = Math.Clamp(v, min, max),
            BackColor = XuTheme.Panel2, ForeColor = XuTheme.Text, Width = 130
        };

        private static void AddRow(TableLayoutPanel lay, string text, Control c)
        {
            lay.Controls.Add(new Label { Text = text, ForeColor = XuTheme.Dim, AutoSize = true, Anchor = AnchorStyles.Left });
            lay.Controls.Add(c);
        }

        private void Apply()
        {
            Step.On = cbOn.Checked;
            Step.Accent = cbAcc.Checked;
            Step.Velocity = (int)spVel.Value;
            Step.Prob = (int)spProb.Value;
            Step.Micro = (int)spMicro.Value;
            Step.Ratchet = (int)spRat.Value;
            Step.Cond = cbCond.SelectedIndex;
        }
    }

    /// <summary>歌曲链编辑对话框（DataGridView 两列）。</summary>
    public sealed class XuSongDialog : Form
    {
        private readonly DataGridView grid;
        private readonly XuProject project;
        public XuSongDialog(XuProject p)
        {
            project = p;
            Text = "歌曲链 - XU-808";
            ClientSize = new Size(480, 340);
            StartPosition = FormStartPosition.CenterParent;
            BackColor = XuTheme.Panel; ForeColor = XuTheme.Text;
            grid = new DataGridView
            {
                Dock = DockStyle.Fill,
                AllowUserToAddRows = true,
                BackgroundColor = XuTheme.Panel2,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
                RowHeadersVisible = false
            };
            grid.Columns.Add(new DataGridViewComboBoxColumn
            {
                HeaderText = "Pattern",
                DataSource = Enumerable.Range(0, p.Patterns.Count).Select(i => $"{i + 1:00}").ToList()
            });
            grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "重复次数" });
            foreach (var e in p.Song.Entries)
                grid.Rows.Add($"{e.Pattern + 1:00}", e.Repeats);
            if (grid.Rows.Count == 0) grid.Rows.Add("01", 1);
            Controls.Add(grid);
            var btns = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 44, FlowDirection = FlowDirection.RightToLeft };
            var ok = new Button { Text = "确定", DialogResult = DialogResult.OK };
            var cancel = new Button { Text = "取消", DialogResult = DialogResult.Cancel };
            btns.Controls.Add(ok); btns.Controls.Add(cancel);
            Controls.Add(btns);
            AcceptButton = ok; CancelButton = cancel;
            ok.Click += (_, __) =>
            {
                p.Song.Entries.Clear();
                foreach (DataGridViewRow row in grid.Rows)
                {
                    if (row.IsNewRow) continue;
                    // ComboBox 列里存的是 "01" 这样的字符串，必须解析后减 1
                    string ps = Convert.ToString(row.Cells[0].Value) ?? "01";
                    int pat = int.TryParse(ps, out int pv) ? Math.Clamp(pv - 1, 0, p.Patterns.Count - 1) : 0;
                    int rep = int.TryParse(Convert.ToString(row.Cells[1].Value), out int r) ? Math.Max(1, r) : 1;
                    p.Song.Entries.Add(new XuSongEntry { Pattern = pat, Repeats = rep });
                }
            };
        }
    }
}
