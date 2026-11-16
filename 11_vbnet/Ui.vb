' ============================================================================
'  XU-808  Ui.vb —— WinForms 界面控件（VB.NET 版）
'  作者：徐某
'
'  视觉目标：深色机身 + 橙色按钮，尽量贴近 TR-808 面板。
'  技术上不使用任何第三方控件库，全部是 System.Windows.Forms 原生控件
'  加四个自绘控件（竖滑杆、电平表、步进网格），
'  这样单文件发布时不会带上任何奇怪的 DLL。
'
'  GDI 资源纪律（徐某踩过一次坑）：
'    字体只创建一次放在 XuTheme 里；画笔/画刷一律用 Using 包住。
'    如果在 OnPaint 里 New Font，句柄会一直泄漏，跑十几分钟后
'    Windows 就会开始报“创建窗口句柄时出错”。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.Collections.Generic
Imports System.Drawing
Imports System.Drawing.Drawing2D
Imports System.Globalization
Imports System.Windows.Forms

Namespace Xu808

    ' ------------------------------------------------------------------------
    '  颜色与字体（规格书第 9 节的配色）
    ' ------------------------------------------------------------------------
    Friend Module XuTheme
        Public ReadOnly Bg As Color = Color.FromArgb(18, 18, 18)          ' #121212
        Public ReadOnly PanelBack As Color = Color.FromArgb(28, 28, 28)   ' #1c1c1c
        Public ReadOnly Panel2 As Color = Color.FromArgb(35, 35, 35)
        Public ReadOnly Line As Color = Color.FromArgb(46, 46, 46)        ' #2e2e2e
        Public ReadOnly Orange As Color = Color.FromArgb(255, 122, 24)    ' #ff7a18
        Public ReadOnly OrangeDark As Color = Color.FromArgb(168, 74, 12)
        Public ReadOnly Yellow As Color = Color.FromArgb(255, 209, 102)   ' #ffd166
        Public ReadOnly Cyan As Color = Color.FromArgb(76, 201, 240)      ' #4cc9f0
        Public ReadOnly TextColor As Color = Color.FromArgb(232, 232, 232)
        Public ReadOnly DimColor As Color = Color.FromArgb(138, 138, 138)
        Public ReadOnly Red As Color = Color.FromArgb(229, 72, 77)

        Public ReadOnly FontSmall As New Font("Microsoft YaHei UI", 8.0F)
        Public ReadOnly FontNormal As New Font("Microsoft YaHei UI", 9.0F)
        Public ReadOnly FontBold As New Font("Microsoft YaHei UI", 9.0F, FontStyle.Bold)
        Public ReadOnly FontMono As New Font("Consolas", 9.0F, FontStyle.Bold)
        Public ReadOnly FontLogo As New Font("Consolas", 20.0F, FontStyle.Bold)
        Public ReadOnly FontTiny As New Font("Microsoft YaHei UI", 6.5F)
    End Module

    ''' <summary>
    ''' 通道参数按名字读写。C# 版用反射（GetField），VB 版改成 Select Case：
    ''' 类型安全、没有反射开销，而且字段名大小写写错会在编译期就被发现。
    ''' </summary>
    Friend Module XuPadAccess
        Public Function GetParam(p As XuPadParams, attr As String) As Single
            Select Case attr
                Case "tune"
                    Return p.Tune
                Case "decay"
                    Return p.Decay
                Case "tone"
                    Return p.Tone
                Case "snappy"
                    Return p.Snappy
                Case "tail"
                    Return p.Tail
                Case "level"
                    Return p.Level
                Case "pan"
                    Return p.Pan
                Case Else
                    Return 0.0F
            End Select
        End Function

        Public Sub SetParam(p As XuPadParams, attr As String, value As Single)
            Select Case attr
                Case "tune"
                    p.Tune = value
                Case "decay"
                    p.Decay = value
                Case "tone"
                    p.Tone = value
                Case "snappy"
                    p.Snappy = value
                Case "tail"
                    p.Tail = value
                Case "level"
                    p.Level = value
                Case "pan"
                    p.Pan = value
            End Select
        End Sub
    End Module

    ''' <summary>竖向小滑杆：WinForms 自带 TrackBar 不支持竖向，只能自绘。</summary>
    Public Class XuVSlider
        Inherits Control

        Public Min As Integer = 0
        Public Max As Integer = 100
        Private _value As Integer

        Public Event ValueChanged As EventHandler

        ' 只在代码里使用，不需要设计器保存；显式标记可消掉设计器序列化警告
        <System.ComponentModel.Browsable(False)>
        <System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)>
        Public Property Value As Integer
            Get
                Return _value
            End Get
            Set(v As Integer)
                Dim nv As Integer = Math.Clamp(v, Min, Max)
                If nv <> _value Then
                    _value = nv
                    RaiseEvent ValueChanged(Me, EventArgs.Empty)
                    Invalidate()
                End If
            End Set
        End Property

        Public Sub New()
            SetStyle(ControlStyles.OptimizedDoubleBuffer Or ControlStyles.AllPaintingInWmPaint Or ControlStyles.UserPaint, True)
            Width = 16
        End Sub

        Protected Overrides Sub OnPaint(e As PaintEventArgs)
            Dim g As Graphics = e.Graphics
            g.SmoothingMode = SmoothingMode.AntiAlias
            g.Clear(XuTheme.Panel2)

            Dim x As Integer = Width \ 2
            ' 轨道：一条深色竖槽
            Using b As New SolidBrush(Color.FromArgb(16, 16, 16))
                g.FillRectangle(b, x - 3, 2, 6, Height - 4)
            End Using
            ' 已填充部分（从底部往上长）
            Dim span As Integer = Math.Max(1, Max - Min)
            Dim h As Integer = CInt((Height - 4) * (_value - Min) / CDbl(span))
            Using b As New SolidBrush(XuTheme.OrangeDark)
                g.FillRectangle(b, x - 3, Height - 2 - h, 6, h)
            End Using
            ' 手柄
            Dim hy As Integer = Height - 2 - h
            Using b As New SolidBrush(XuTheme.Orange)
                g.FillRectangle(b, x - 7, hy - 5, 14, 10)
            End Using
            Using p As New Pen(XuTheme.Line)
                g.DrawRectangle(p, x - 7, hy - 5, 14, 10)
            End Using
        End Sub

        Protected Overrides Sub OnMouseDown(e As MouseEventArgs)
            SetFromY(e.Y)
            MyBase.OnMouseDown(e)
        End Sub

        Protected Overrides Sub OnMouseMove(e As MouseEventArgs)
            If e.Button = MouseButtons.Left Then SetFromY(e.Y)
            MyBase.OnMouseMove(e)
        End Sub

        ''' <summary>把鼠标 Y 换算成 0..1 再映射到 [Min,Max]。</summary>
        Private Sub SetFromY(y As Integer)
            Dim k As Double = 1.0 - Math.Clamp(y, 2, Height - 2) / CDbl(Math.Max(1, Height - 4))
            Value = Min + CInt(Math.Round(k * (Max - Min)))
        End Sub
    End Class

    ''' <summary>双声道电平表（绿→黄→红）。</summary>
    Public Class XuMeter
        Inherits Control

        Public L As Single
        Public R As Single

        Public Sub New()
            SetStyle(ControlStyles.OptimizedDoubleBuffer Or ControlStyles.AllPaintingInWmPaint Or ControlStyles.UserPaint, True)
            Width = 150
            Height = 22
        End Sub

        Protected Overrides Sub OnPaint(e As PaintEventArgs)
            Dim g As Graphics = e.Graphics
            g.Clear(Color.FromArgb(12, 12, 12))
            For i As Integer = 0 To 1
                Dim v As Single = If(i = 0, L, R)
                Dim y As Integer = 3 + i * 9
                Using bg As New SolidBrush(Color.FromArgb(26, 26, 26))
                    g.FillRectangle(bg, 2, y, Width - 4, 7)
                End Using
                Dim w As Integer = CInt((Width - 4) * Math.Clamp(v, 0.0F, 1.0F))
                If w > 0 Then
                    ' 三段渐变：低电平绿、中段黄、接近 0dB 红
                    Using lg As New LinearGradientBrush(New Rectangle(2, y, Math.Max(2, w), 7),
                                                        Color.FromArgb(61, 220, 132), Color.FromArgb(229, 72, 77), 0.0F)
                        Dim blend As New ColorBlend With {
                            .Colors = New Color() {Color.FromArgb(61, 220, 132), Color.FromArgb(255, 209, 102), Color.FromArgb(229, 72, 77)},
                            .Positions = New Single() {0.0F, 0.72F, 1.0F}
                        }
                        lg.InterpolationColors = blend
                        g.FillRectangle(lg, 2, y, w, 7)
                    End Using
                End If
            Next
            Using p As New Pen(XuTheme.Line)
                g.DrawRectangle(p, 1, 1, Width - 2, Height - 2)
            End Using
        End Sub
    End Class

    ''' <summary>
    ''' 步进网格：16 行（声部）× N 列（步）。
    ''' 左键开关格子，右键打开步进属性；左栏点名字 = 手敲该鼓垫，点 M/S = 静音/独奏。
    ''' </summary>
    Public Class XuGridPanel
        Inherits Control

        Public Const CellW As Integer = 30
        Public Const CellH As Integer = 19      ' 行高 19：16 行 + 表头刚好 326px，1080p 上也放得下
        Public Const LabelW As Integer = 128
        Public Const HeaderH As Integer = 22

        ' 字体只创建一次，见文件头的 GDI 纪律说明
        Private Shared ReadOnly CellFont As New Font("Consolas", 6.0F, FontStyle.Bold)

        Public Project As XuProject
        Public Playhead As Integer = -1

        Public Event ToggleStep As Action(Of Integer, Integer)
        Public Event EditStep As Action(Of Integer, Integer)
        Public Event HitPad As Action(Of Integer)
        Public Event ToggleMute As Action(Of Integer)
        Public Event ToggleSolo As Action(Of Integer)

        Public Sub New(p As XuProject)
            Project = p
            SetStyle(ControlStyles.OptimizedDoubleBuffer Or ControlStyles.AllPaintingInWmPaint Or ControlStyles.UserPaint, True)
            RebuildSize()
        End Sub

        ''' <summary>按当前 Pattern 步数重算尺寸（16 / 32 / 64 步都很自然）。</summary>
        Public Sub RebuildSize()
            Dim cols As Integer = Math.Max(1, Project.Current.Steps)
            Width = LabelW + cols * CellW
            Height = HeaderH + 16 * CellH
            Invalidate()
        End Sub

        Public Sub SetPlayhead(s As Integer)
            If s <> Playhead Then
                Playhead = s
                Invalidate()
            End If
        End Sub

        Protected Overrides Sub OnPaint(e As PaintEventArgs)
            Dim g As Graphics = e.Graphics
            g.Clear(XuTheme.Bg)
            Dim pat As XuPattern = Project.Current
            Dim cols As Integer = Math.Max(1, pat.Steps)

            ' ---- 顶部步号条 ----
            Using b As New SolidBrush(XuTheme.PanelBack)
                g.FillRectangle(b, 0, 0, Width, HeaderH)
            End Using
            For c As Integer = 0 To cols - 1
                Dim x As Integer = LabelW + c * CellW
                If c Mod 4 = 0 Then
                    ' 每 4 格一根分隔线，等价于“一拍”
                    Using b As New SolidBrush(Color.FromArgb(58, 58, 58))
                        g.FillRectangle(b, x, 0, 1, Height)
                    End Using
                End If
                TextRenderer.DrawText(g, (c + 1).ToString(CultureInfo.InvariantCulture), XuTheme.FontSmall,
                                      New Rectangle(x, 0, CellW, HeaderH), XuTheme.DimColor,
                                      TextFormatFlags.HorizontalCenter Or TextFormatFlags.VerticalCenter)
            Next

            ' ---- 左侧声部区（代号 / 中文名 / M / S）----
            Using b As New SolidBrush(XuTheme.Panel2)
                g.FillRectangle(b, 0, HeaderH, LabelW, Height - HeaderH)
            End Using
            For r As Integer = 0 To 15
                Dim y As Integer = HeaderH + r * CellH
                Using b As New SolidBrush(If(r Mod 2 = 0, Color.FromArgb(22, 22, 22), Color.FromArgb(26, 26, 26)))
                    g.FillRectangle(b, LabelW, y, Width - LabelW, CellH)
                End Using
                TextRenderer.DrawText(g, XuConst.Codes(r), XuTheme.FontMono,
                                      New Rectangle(6, y, 36, CellH), XuTheme.Orange,
                                      TextFormatFlags.VerticalCenter Or TextFormatFlags.Left)
                TextRenderer.DrawText(g, XuConst.NamesCn(r), XuTheme.FontSmall,
                                      New Rectangle(42, y, 46, CellH), XuTheme.TextColor,
                                      TextFormatFlags.VerticalCenter Or TextFormatFlags.Left)
                Dim pad As XuPadParams = Project.Pads(r)
                For j As Integer = 0 To 1
                    Dim isOn As Boolean = If(j = 0, pad.Mute, pad.Solo)
                    Dim bx As Integer = 92 + j * 18
                    Using b As New SolidBrush(If(isOn, If(j = 0, XuTheme.Red, XuTheme.Yellow), Color.FromArgb(42, 42, 42)))
                        g.FillRectangle(b, bx, y + 4, 15, CellH - 8)
                    End Using
                    TextRenderer.DrawText(g, If(j = 0, "M", "S"), XuTheme.FontSmall,
                                          New Rectangle(bx, y + 4, 15, CellH - 8),
                                          If(isOn, Color.FromArgb(18, 18, 18), XuTheme.DimColor),
                                          TextFormatFlags.HorizontalCenter Or TextFormatFlags.VerticalCenter)
                Next
                Using pen As New Pen(XuTheme.Line)
                    g.DrawLine(pen, 0, y + CellH - 1, Width, y + CellH - 1)
                End Using
            Next

            ' ---- 格子 ----
            For c As Integer = 0 To cols - 1
                Dim x As Integer = LabelW + c * CellW
                For r As Integer = 0 To 15
                    Dim st As XuStep = pat.Data(r)(c)
                    Dim y As Integer = HeaderH + r * CellH
                    Dim rect As New Rectangle(x + 2, y + 2, CellW - 4, CellH - 4)
                    If st.[On] Then
                        ' 重音偏黄、普通偏橙；力度越高越亮（透明度 110..255）
                        Dim baseCol As Color = If(st.Accent, XuTheme.Yellow, XuTheme.Orange)
                        Dim alpha As Integer = CInt(110 + 145 * (st.Velocity / 127.0))
                        Using b As New SolidBrush(Color.FromArgb(Math.Clamp(alpha, 0, 255), baseCol))
                            g.FillRectangle(b, rect)
                        End Using
                        ' 非默认属性用小字标出来，一眼能看出这格“有花样”
                        If st.Prob < 100 OrElse st.Ratchet > 1 OrElse st.Micro <> 0 Then
                            Dim txt As String = If(st.Prob < 100, st.Prob.ToString(CultureInfo.InvariantCulture) + "%", "") +
                                                If(st.Ratchet > 1, "R" + st.Ratchet.ToString(CultureInfo.InvariantCulture), "")
                            TextRenderer.DrawText(g, txt, CellFont, rect, Color.FromArgb(26, 16, 8),
                                                  TextFormatFlags.HorizontalCenter Or TextFormatFlags.VerticalCenter)
                        End If
                    Else
                        Using b As New SolidBrush(Color.FromArgb(16, 16, 16))
                            g.FillRectangle(b, rect)
                        End Using
                        Using p As New Pen(Color.FromArgb(39, 39, 39))
                            g.DrawRectangle(p, rect)
                        End Using
                    End If
                Next
            Next

            ' ---- 播放头：半透明青蓝覆盖 + 顶部三角 ----
            If Playhead >= 0 AndAlso Playhead < cols Then
                Dim x As Integer = LabelW + Playhead * CellW
                Using b As New SolidBrush(Color.FromArgb(26, 76, 201, 240))
                    g.FillRectangle(b, x, HeaderH, CellW, Height - HeaderH)
                End Using
                Using p As New Pen(XuTheme.Cyan, 2.0F)
                    g.DrawLine(p, x, HeaderH, x, Height)
                    g.DrawLine(p, x + CellW, HeaderH, x + CellW, Height)
                End Using
                Using b As New SolidBrush(XuTheme.Cyan)
                    g.FillPolygon(b, New Point() {New Point(x + 7, 0), New Point(x + CellW - 7, 0), New Point(x + CellW \ 2, 10)})
                End Using
            End If

            Using border As New Pen(XuTheme.Line)
                g.DrawRectangle(border, 0, 0, Width - 1, Height - 1)
            End Using
        End Sub

        Protected Overrides Sub OnMouseDown(e As MouseEventArgs)
            Dim y As Integer = e.Y
            Dim x As Integer = e.X
            If y < HeaderH Then Return
            Dim r As Integer = (y - HeaderH) \ CellH
            If r < 0 OrElse r >= 16 Then Return

            If x < LabelW Then
                ' 左栏：先判 M / S 热区，其余位置当作鼓垫
                For j As Integer = 0 To 1
                    Dim bx As Integer = 92 + j * 18
                    If x >= bx AndAlso x <= bx + 15 Then
                        If j = 0 Then
                            RaiseEvent ToggleMute(r)
                        Else
                            RaiseEvent ToggleSolo(r)
                        End If
                        Return
                    End If
                Next
                RaiseEvent HitPad(r)
                Return
            End If

            Dim c As Integer = (x - LabelW) \ CellW
            If c < 0 OrElse c >= Math.Max(1, Project.Current.Steps) Then Return
            If e.Button = MouseButtons.Right Then
                RaiseEvent EditStep(r, c)
            Else
                RaiseEvent ToggleStep(r, c)
            End If
        End Sub
    End Class

    ''' <summary>一条通道条：名字按键 + M/S + 若干竖滑杆 + 轨道步长（polymeter）。</summary>
    Public Class XuChannelStrip
        Inherits Panel

        Public ReadOnly Pad As Integer
        Private ReadOnly project As XuProject
        Public Event HitPad As Action(Of Integer)
        Public Event Changed As Action

        ' (滑杆, 参数名, 上限) —— 上限 125 是给 LEVEL 用的（0..1.25）
        Private ReadOnly sliders As New List(Of ValueTuple(Of XuVSlider, String, Integer))
        Private bm As CheckBox
        Private bs As CheckBox
        Private lenBox As NumericUpDown

        Public Sub New(pad As Integer, p As XuProject)
            Me.Pad = pad
            project = p
            Width = 74
            Height = 306          ' 54 + 5×46 + 20（步长框）= 304 的内容，留 2px 余量
            BackColor = XuTheme.Bg

            Dim name As New Button With {
                .Text = XuConst.Codes(pad) + vbCrLf + XuConst.NamesCn(pad),
                .Top = 2, .Left = 2, .Width = 70, .Height = 34,
                .FlatStyle = FlatStyle.Flat,
                .ForeColor = XuTheme.Orange,
                .BackColor = XuTheme.Panel2,
                .Font = XuTheme.FontSmall
            }
            name.FlatAppearance.BorderColor = Color.FromArgb(58, 42, 26)
            AddHandler name.Click, Sub(s As Object, ev As EventArgs) RaiseEvent HitPad(Pad)
            Controls.Add(name)

            ' M / S
            bm = New CheckBox With {.Text = "M", .Top = 38, .Left = 4, .Width = 30, .Height = 18,
                                    .ForeColor = XuTheme.Red, .Font = XuTheme.FontSmall}
            bs = New CheckBox With {.Text = "S", .Top = 38, .Left = 38, .Width = 30, .Height = 18,
                                    .ForeColor = XuTheme.Yellow, .Font = XuTheme.FontSmall}
            bm.Checked = project.Pads(pad).Mute
            bs.Checked = project.Pads(pad).Solo
            AddHandler bm.CheckedChanged, Sub(s As Object, ev As EventArgs)
                                              project.Pads(Pad).Mute = bm.Checked
                                              RaiseEvent Changed()
                                          End Sub
            AddHandler bs.CheckedChanged, Sub(s As Object, ev As EventArgs)
                                              project.Pads(Pad).Solo = bs.Checked
                                              RaiseEvent Changed()
                                          End Sub
            Controls.Add(bm)
            Controls.Add(bs)

            Dim top As Integer = 54
            For Each prm As String In ParamNamesFor(pad)
                AddSlider(LABELS(prm), prm, top)
            Next
            AddSlider("LEVEL", "level", top, 125)
            AddSlider("PAN", "pan", top)

            ' 轨道步长：1..64，实现 polymeter（轨道步号 = 全局步号 % 轨道长度）
            Dim len As New NumericUpDown With {
                .Top = top, .Left = 6, .Width = 60,
                .Minimum = 1, .Maximum = 64,
                .Value = Math.Clamp(project.Current.TrackLen(pad), 1, 64),
                .Font = XuTheme.FontSmall,
                .BackColor = XuTheme.Panel2,
                .ForeColor = XuTheme.TextColor,
                .BorderStyle = BorderStyle.FixedSingle
            }
            AddHandler len.ValueChanged, Sub(s As Object, ev As EventArgs)
                                             project.Current.TrackLen(Pad) = CInt(len.Value)
                                             RaiseEvent Changed()
                                         End Sub
            lenBox = len
            Controls.Add(len)
        End Sub

        ''' <summary>参数显示名（滑杆上方的小字）。</summary>
        Private Shared ReadOnly LABELS As Dictionary(Of String, String) =
            New Dictionary(Of String, String) From {
                {"tune", "TUNE"}, {"decay", "DECAY"}, {"tone", "TONE"},
                {"snappy", "SNAP"}, {"tail", "TAIL"}
            }

        ''' <summary>按规格书第 3 节，每个声部暴露的参数不同。</summary>
        Private Shared Function ParamNamesFor(pad As Integer) As String()
            Select Case pad
                Case 0
                    Return {"tune", "decay", "tone"}
                Case 1
                    Return {"tune", "snappy", "decay"}
                Case 2, 3, 4
                    Return {"tune", "decay", "tone"}          ' COLOR 走的是 Tone 字段
                Case 10
                    Return {"decay", "tone", "tail"}          ' 拍手：Tone 是带通中心
                Case 11
                    Return {"decay", "tone"}
                Case 13, 14, 15
                    Return {"decay", "tone"}
                Case 12
                    Return {"tune", "decay"}
                Case Else
                    Return {"tune", "decay"}
            End Select
        End Function

        Private Sub AddSlider(label As String, attr As String, ByRef top As Integer, Optional max As Integer = 100)
            Dim lab As New Label With {
                .Text = label, .Top = top, .Left = 2, .Width = 70, .Height = 11,
                .TextAlign = ContentAlignment.MiddleCenter,
                .ForeColor = XuTheme.DimColor, .Font = XuTheme.FontTiny
            }
            Controls.Add(lab)

            Dim s As New XuVSlider With {.Top = top + 11, .Left = 29, .Width = 16, .Height = 34, .Max = max}
            ' 初始值和写回必须除以同一个 max，否则 LEVEL（max=125）一拖动就跳到 0.8
            s.Value = CInt(Math.Round(XuPadAccess.GetParam(project.Pads(Pad), attr) * max))
            AddHandler s.ValueChanged, Sub(sender As Object, ev As EventArgs)
                                           XuPadAccess.SetParam(project.Pads(Pad), attr, s.Value / CSng(max))
                                           RaiseEvent Changed()
                                       End Sub
            Controls.Add(s)
            sliders.Add(New ValueTuple(Of XuVSlider, String, Integer)(s, attr, max))
            top += 46
        End Sub

        ''' <summary>切 Pattern 后把界面拉回与工程数据一致（静默，不触发写回）。</summary>
        Public Sub SyncFromProject()
            For Each item In sliders
                item.Item1.Value = CInt(Math.Round(XuPadAccess.GetParam(project.Pads(Pad), item.Item2) * item.Item3))
            Next
            bm.Checked = project.Pads(Pad).Mute
            bs.Checked = project.Pads(Pad).Solo
            lenBox.Value = Math.Clamp(project.Current.TrackLen(Pad), 1, 64)
        End Sub
    End Class

    ''' <summary>步进属性对话框：力度 / 重音 / 概率 / 微时 / ratchet / 条件。</summary>
    Public Class XuStepDialog
        Inherits Form

        ' 注意：Step 是 VB 关键字，字段名只能叫 StepData
        Public ReadOnly StepData As XuStep
        Private ReadOnly cbOn As CheckBox
        Private ReadOnly cbAcc As CheckBox
        Private ReadOnly spVel As NumericUpDown
        Private ReadOnly spProb As NumericUpDown
        Private ReadOnly spMicro As NumericUpDown
        Private ReadOnly spRat As NumericUpDown
        Private ReadOnly cbCond As ComboBox

        Public Sub New(st As XuStep)
            StepData = st
            Text = "步进属性 - 徐某的 808"
            FormBorderStyle = FormBorderStyle.FixedDialog
            StartPosition = FormStartPosition.CenterParent
            MaximizeBox = False
            MinimizeBox = False
            BackColor = XuTheme.PanelBack
            ForeColor = XuTheme.TextColor
            ClientSize = New Size(430, 350)
            Font = XuTheme.FontNormal

            Dim lay As New TableLayoutPanel With {
                .Dock = DockStyle.Fill,
                .ColumnCount = 2,
                .RowCount = 8,
                .Padding = New Padding(12),
                .BackColor = XuTheme.PanelBack
            }
            lay.ColumnStyles.Add(New ColumnStyle(SizeType.Percent, 45.0F))
            lay.ColumnStyles.Add(New ColumnStyle(SizeType.Percent, 55.0F))
            For i As Integer = 0 To 6
                lay.RowStyles.Add(New RowStyle(SizeType.Absolute, 36.0F))
            Next
            lay.RowStyles.Add(New RowStyle(SizeType.Absolute, 52.0F))
            Controls.Add(lay)

            cbOn = New CheckBox With {.Text = "启用", .Checked = st.[On], .ForeColor = XuTheme.TextColor,
                                      .AutoSize = True, .Anchor = AnchorStyles.Left}
            cbAcc = New CheckBox With {.Text = "重音 Accent（音量+衰减+音色）", .Checked = st.Accent,
                                       .ForeColor = XuTheme.Yellow, .AutoSize = True, .Anchor = AnchorStyles.Left}
            lay.Controls.Add(cbOn, 0, 0)
            lay.SetColumnSpan(cbOn, 2)
            lay.Controls.Add(cbAcc, 0, 1)
            lay.SetColumnSpan(cbAcc, 2)

            spVel = Num(1, 127, st.Velocity)
            spProb = Num(0, 100, st.Prob)
            spMicro = Num(-2400, 2400, st.Micro)
            spRat = Num(1, 8, st.Ratchet)
            AddRow(lay, "力度 Velocity", spVel, 2)
            AddRow(lay, "概率 Probability %", spProb, 3)
            AddRow(lay, "微时 Micro（样本）", spMicro, 4)
            AddRow(lay, "滚奏 Ratchet", spRat, 5)

            cbCond = New ComboBox With {.DropDownStyle = ComboBoxStyle.DropDownList,
                                        .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor,
                                        .Anchor = AnchorStyles.Left Or AnchorStyles.Right}
            For Each s As String In XuCond.Names
                cbCond.Items.Add(s)
            Next
            cbCond.SelectedIndex = Math.Clamp(st.Cond, 0, 4)
            AddRow(lay, "条件触发", cbCond, 6)

            Dim ok As New Button With {.Text = "确定", .DialogResult = DialogResult.OK,
                                       .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor,
                                       .FlatStyle = FlatStyle.Flat, .Width = 84, .Height = 30}
            Dim cancel As New Button With {.Text = "取消", .DialogResult = DialogResult.Cancel,
                                           .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor,
                                           .FlatStyle = FlatStyle.Flat, .Width = 84, .Height = 30}
            Dim btns As New FlowLayoutPanel With {.FlowDirection = FlowDirection.RightToLeft,
                                                  .Dock = DockStyle.Fill, .BackColor = XuTheme.PanelBack}
            btns.Controls.Add(ok)
            btns.Controls.Add(cancel)
            lay.Controls.Add(btns, 0, 7)
            lay.SetColumnSpan(btns, 2)

            AcceptButton = ok
            CancelButton = cancel
            AddHandler ok.Click, Sub(s As Object, ev As EventArgs) Apply()
        End Sub

        Private Shared Function Num(min As Integer, max As Integer, v As Integer) As NumericUpDown
            Return New NumericUpDown With {
                .Minimum = min, .Maximum = max, .Value = Math.Clamp(v, min, max),
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .Width = 130,
                .Anchor = AnchorStyles.Left
            }
        End Function

        Private Shared Sub AddRow(lay As TableLayoutPanel, text As String, c As Control, row As Integer)
            Dim lab As New Label With {.Text = text, .ForeColor = XuTheme.DimColor, .AutoSize = True,
                                       .Anchor = AnchorStyles.Left, .Margin = New Padding(3, 9, 3, 3)}
            lay.Controls.Add(lab, 0, row)
            lay.Controls.Add(c, 1, row)
        End Sub

        ''' <summary>点“确定”时才写回数据；点“取消”原数据分毫不动。</summary>
        Private Sub Apply()
            StepData.[On] = cbOn.Checked
            StepData.Accent = cbAcc.Checked
            StepData.Velocity = CInt(spVel.Value)
            StepData.Prob = CInt(spProb.Value)
            StepData.Micro = CInt(spMicro.Value)
            StepData.Ratchet = CInt(spRat.Value)
            StepData.Cond = cbCond.SelectedIndex
        End Sub
    End Class

    ''' <summary>歌曲链编辑对话框：两列（Pattern / 重复次数），可增删行。</summary>
    Public Class XuSongDialog
        Inherits Form

        Private ReadOnly grid As DataGridView
        Private ReadOnly project As XuProject

        Public Sub New(p As XuProject)
            project = p
            Text = "歌曲链 - XU-808"
            ClientSize = New Size(480, 360)
            StartPosition = FormStartPosition.CenterParent
            BackColor = XuTheme.PanelBack
            ForeColor = XuTheme.TextColor

            grid = New DataGridView With {
                .Dock = DockStyle.Fill,
                .AllowUserToAddRows = True,
                .BackgroundColor = XuTheme.Panel2,
                .AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
                .RowHeadersVisible = False,
                .GridColor = XuTheme.Line
            }
            Dim col0 As New DataGridViewComboBoxColumn With {.HeaderText = "Pattern"}
            For i As Integer = 0 To p.Patterns.Count - 1
                col0.Items.Add($"{i + 1:00}")
            Next
            grid.Columns.Add(col0)
            grid.Columns.Add(New DataGridViewTextBoxColumn With {.HeaderText = "重复次数"})

            For Each e As XuSongEntry In p.Song.Entries
                grid.Rows.Add($"{e.Pattern + 1:00}", e.Repeats)
            Next
            If grid.Rows.Count = 0 Then grid.Rows.Add("01", 1)
            Controls.Add(grid)

            Dim btns As New FlowLayoutPanel With {.Dock = DockStyle.Bottom, .Height = 44,
                                                  .FlowDirection = FlowDirection.RightToLeft,
                                                  .BackColor = XuTheme.PanelBack}
            Dim ok As New Button With {.Text = "确定", .DialogResult = DialogResult.OK, .Width = 84, .Height = 30,
                                       .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor,
                                       .FlatStyle = FlatStyle.Flat}
            Dim cancel As New Button With {.Text = "取消", .DialogResult = DialogResult.Cancel, .Width = 84,
                                           .Height = 30, .BackColor = XuTheme.Panel2,
                                           .ForeColor = XuTheme.TextColor, .FlatStyle = FlatStyle.Flat}
            btns.Controls.Add(ok)
            btns.Controls.Add(cancel)
            Controls.Add(btns)
            AcceptButton = ok
            CancelButton = cancel

            AddHandler ok.Click, Sub(s As Object, ev As EventArgs)
                                     p.Song.Entries.Clear()
                                     For Each row As DataGridViewRow In grid.Rows
                                         If row.IsNewRow Then Continue For
                                         ' ComboBox 列里存的是 "01" 这样的字符串，必须解析后减 1
                                         Dim ps As String = If(Convert.ToString(row.Cells(0).Value), "01")
                                         Dim pv As Integer = 1
                                         If Not Integer.TryParse(ps, pv) Then pv = 1
                                         Dim pat As Integer = Math.Clamp(pv - 1, 0, p.Patterns.Count - 1)
                                         Dim rv As Integer = 1
                                         Integer.TryParse(Convert.ToString(row.Cells(1).Value), rv)
                                         p.Song.Entries.Add(New XuSongEntry With {.Pattern = pat, .Repeats = Math.Max(1, rv)})
                                     Next
                                 End Sub
        End Sub
    End Class

End Namespace
