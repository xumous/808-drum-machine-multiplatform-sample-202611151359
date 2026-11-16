' ============================================================================
'  XU-808  MainForm.vb —— 主窗口（VB.NET 版）
'  作者：徐某
'
'  所有控件都直接接在 XuEngine 上：
'    · 拖参数 → 直接改 project.Pads 的属性（音频线程下一块就能读到）；
'    · 点格子 → 改 XuStep；
'    · 走带 / 切歌 → engine.Play / Stop / RequestPattern；
'    · 定时器 33ms → 只刷新播放头 / 电平 / CPU，绝不参与音序。
'
'  重要：音频合成全部在 waveOut 的系统回调线程里完成，UI 定时器只读快照。
'  即使界面卡住（比如弹了模态对话框），节奏也不会掉一拍 —— 这正是规格书
'  “严禁用 UI 定时器当时钟”的含义。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.Collections.Generic
Imports System.Drawing
Imports System.Globalization
Imports System.IO
Imports System.Windows.Forms

Namespace Xu808

    Public Class XuMainForm
        Inherits Form

        Private ReadOnly project As XuProject
        Private ReadOnly engine As XuEngine

        Private waveOut As XuWaveOut
        Private midi As XuMidiIn

        Private grid As XuGridPanel
        Private gridHost As Panel
        Private stripHost As Panel
        Private cbPattern As ComboBox
        Private spBpm As NumericUpDown
        Private spSteps As NumericUpDown
        Private tbBpm As TrackBar
        Private tbSwing As TrackBar
        Private lbSwing As Label
        Private lbStatus As Label
        Private lbMidi As Label
        Private meter As XuMeter

        Private ReadOnly timer As New System.Windows.Forms.Timer()
        Private ReadOnly tips As New ToolTip()
        Private loadingUi As Boolean = False
        Private ReadOnly tapTimes As New List(Of Double)

        Public Sub New(p As XuProject)
            project = p
            engine = New XuEngine(p)

            Text = "XU-808 鼓机 - 徐某"
            ' 设计尺寸 1500×900。真正的“适配屏幕”放到 OnShown 里做：
            ' 构造函数阶段窗体的外框尺寸还没算上标题栏/边框，提前收敛不准。
            ClientSize = New Size(1500, 900)
            MinimumSize = New Size(1000, 600)
            BackColor = XuTheme.Bg
            ForeColor = XuTheme.TextColor
            Font = XuTheme.FontNormal
            KeyPreview = True
            ' 先试着读同目录的 app.ico；单文件发布时若被剥离，则从 EXE 自身取图标
            Try
                Dim icoPath As String = Path.Combine(AppContext.BaseDirectory, "app.ico")
                If File.Exists(icoPath) Then
                    Icon = New Icon(icoPath)
                Else
                    Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath)
                End If
            Catch
                ' 图标只是装饰，取不到不影响任何功能
            End Try

            Dim root As New TableLayoutPanel With {
                .Dock = DockStyle.Fill, .ColumnCount = 1, .RowCount = 4, .BackColor = XuTheme.Bg
            }
            root.RowStyles.Add(New RowStyle(SizeType.Absolute, 60.0F))     ' 顶部走带
            root.RowStyles.Add(New RowStyle(SizeType.Percent, 100.0F))     ' 步进网格
            root.RowStyles.Add(New RowStyle(SizeType.Absolute, 316.0F))    ' 16 通道条（306 + 上下各 2 + 余量）
            root.RowStyles.Add(New RowStyle(SizeType.Absolute, 100.0F))    ' 主效果 + 电平表
            Controls.Add(root)

            root.Controls.Add(BuildTop(), 0, 0)
            BuildGrid(root)
            BuildStrips(root)
            root.Controls.Add(BuildMaster(), 0, 3)

            StartAudio()
            StartMidi()

            ' 33ms ≈ 30fps；只看快照，不推进音序
            timer.Interval = 33
            AddHandler timer.Tick, AddressOf OnTick
            timer.Start()
        End Sub

        ''' <summary>
        ''' 窗口显示后按“所在显示器的工作区”收敛尺寸并贴左上角。
        ''' 这样 1366×768 之类的小屏也能看到底部的主效果区（网格区域自带滚动条）。
        ''' </summary>
        Protected Overrides Sub OnShown(e As EventArgs)
            MyBase.OnShown(e)
            Dim wa As Rectangle = Screen.FromControl(Me).WorkingArea
            Dim maxW As Integer = Math.Max(MinimumSize.Width, wa.Width - 16)
            Dim maxH As Integer = Math.Max(MinimumSize.Height, wa.Height - 16)
            If Width > maxW Then Width = maxW
            If Height > maxH Then Height = maxH
            If Left < wa.Left OrElse Left + Width > wa.Right Then Left = wa.Left + 8
            If Top < wa.Top OrElse Top + Height > wa.Bottom Then Top = wa.Top + 8
        End Sub

        ' ------------------------------------------------------------------
        '  顶部走带条
        ' ------------------------------------------------------------------
        Private Function BuildTop() As Control
            Dim top As New FlowLayoutPanel With {
                .Dock = DockStyle.Fill,
                .FlowDirection = FlowDirection.LeftToRight,
                .WrapContents = False,
                .BackColor = XuTheme.PanelBack,
                .Padding = New Padding(6, 4, 6, 4)
            }

            ' LOGO
            Dim logoBox As New Panel With {.Width = 150, .Height = 48}
            logoBox.Controls.Add(New Label With {
                .Text = "XU-808", .AutoSize = True, .Left = 0, .Top = 0,
                .Font = XuTheme.FontLogo, .ForeColor = XuTheme.Orange
            })
            logoBox.Controls.Add(New Label With {
                .Text = "RHYTHM COMPOSER · 徐某", .AutoSize = True, .Left = 2, .Top = 36,
                .Font = New Font("Microsoft YaHei UI", 7.0F), .ForeColor = XuTheme.DimColor
            })
            top.Controls.Add(logoBox)

            ' Play / Stop
            Dim bPlay As Button = BigBtn("▶ PLAY")
            Dim bStop As Button = BigBtn("■ STOP")
            AddHandler bPlay.Click, Sub(s As Object, ev As EventArgs) engine.Play()
            AddHandler bStop.Click, Sub(s As Object, ev As EventArgs)
                                         engine.[Stop]()
                                         grid.SetPlayhead(-1)
                                     End Sub
            top.Controls.Add(bPlay)
            top.Controls.Add(bStop)

            ' BPM：数字框 + 滑杆联动
            top.Controls.Add(SmallCaption("BPM"))
            spBpm = New NumericUpDown With {
                .DecimalPlaces = 1, .Minimum = 40D, .Maximum = 300D, .Increment = 0.5D,
                .Value = CDec(project.Master.Bpm), .Width = 62,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .BorderStyle = BorderStyle.FixedSingle
            }
            AddHandler spBpm.ValueChanged, Sub(s As Object, ev As EventArgs)
                                               If loadingUi Then Return
                                               project.Master.Bpm = CSng(spBpm.Value)
                                               SyncBpmSlider()
                                           End Sub
            top.Controls.Add(spBpm)

            tbBpm = New TrackBar With {
                .Minimum = 40, .Maximum = 300, .Value = Math.Clamp(CInt(project.Master.Bpm), 40, 300),
                .Width = 86, .Height = 32, .TickStyle = TickStyle.None
            }
            AddHandler tbBpm.ValueChanged, Sub(s As Object, ev As EventArgs)
                                               If loadingUi Then Return
                                               spBpm.Value = CDec(tbBpm.Value)
                                           End Sub
            top.Controls.Add(tbBpm)

            ' Tap Tempo
            Dim bTap As Button = BigBtn("TAP")
            bTap.Width = 46
            AddHandler bTap.Click, Sub(s As Object, ev As EventArgs) OnTap()
            top.Controls.Add(bTap)

            ' Swing
            top.Controls.Add(SmallCaption("SWING"))
            tbSwing = New TrackBar With {
                .Minimum = 0, .Maximum = 75, .Value = CInt(Math.Round(project.Master.Swing * 100.0F)),
                .Width = 86, .Height = 32, .TickStyle = TickStyle.None
            }
            lbSwing = New Label With {
                .Text = tbSwing.Value.ToString(CultureInfo.InvariantCulture) + "%",
                .ForeColor = XuTheme.Orange, .AutoSize = True, .Padding = New Padding(0, 16, 0, 0)
            }
            AddHandler tbSwing.ValueChanged, Sub(s As Object, ev As EventArgs)
                                                 If loadingUi Then Return
                                                 project.Master.Swing = CSng(tbSwing.Value / 100.0)
                                                 lbSwing.Text = tbSwing.Value.ToString(CultureInfo.InvariantCulture) + "%"
                                             End Sub
            top.Controls.Add(tbSwing)
            top.Controls.Add(lbSwing)

            ' Pattern 选择 + 步数
            top.Controls.Add(SmallCaption("PATTERN"))
            cbPattern = New ComboBox With {
                .Width = 116, .DropDownStyle = ComboBoxStyle.DropDownList,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor
            }
            For i As Integer = 0 To project.Patterns.Count - 1
                cbPattern.Items.Add($"{i + 1:00} {project.Patterns(i).Name}")
            Next
            cbPattern.SelectedIndex = project.CurrentPattern
            AddHandler cbPattern.SelectedIndexChanged, Sub(s As Object, ev As EventArgs)
                                                           If loadingUi Then Return
                                                           engine.RequestPattern(cbPattern.SelectedIndex)
                                                           spSteps.Value = CDec(project.Current.Steps)
                                                           grid.RebuildSize()
                                                           SyncStrips()
                                                       End Sub
            top.Controls.Add(cbPattern)

            spSteps = New NumericUpDown With {
                .Minimum = 1D, .Maximum = 64D, .Value = CDec(project.Current.Steps), .Width = 50,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .BorderStyle = BorderStyle.FixedSingle
            }
            tips.SetToolTip(spSteps, "Pattern 步数（1-64）；常用 16 / 32 / 64")
            AddHandler spSteps.ValueChanged, Sub(s As Object, ev As EventArgs)
                                                 If loadingUi Then Return
                                                 project.Current.Steps = CInt(spSteps.Value)
                                                 grid.RebuildSize()
                                             End Sub
            top.Controls.Add(spSteps)

            ' Song 开关 + 歌曲链编辑
            Dim cbSong As New CheckBox With {
                .Text = "SONG", .ForeColor = XuTheme.TextColor, .Width = 62, .Height = 38,
                .Checked = project.Master.SongMode, .Padding = New Padding(2, 9, 0, 0)
            }
            AddHandler cbSong.CheckedChanged, Sub(s As Object, ev As EventArgs)
                                                  project.Master.SongMode = cbSong.Checked
                                              End Sub
            top.Controls.Add(cbSong)

            Dim bSong As Button = BigBtn("歌曲链…")
            bSong.Width = 68
            AddHandler bSong.Click, Sub(s As Object, ev As EventArgs)
                                        Using d As New XuSongDialog(project)
                                            d.ShowDialog(Me)
                                        End Using
                                    End Sub
            top.Controls.Add(bSong)

            ' 保存 / 打开 / 导出 / 设置
            Dim bSave As Button = BigBtn("保存")
            Dim bLoad As Button = BigBtn("打开")
            Dim bWav As Button = BigBtn("WAV")
            Dim bMid As Button = BigBtn("MIDI")
            Dim bSet As Button = BigBtn("设置")
            bSave.Width = 52 : bLoad.Width = 52 : bWav.Width = 52 : bMid.Width = 56 : bSet.Width = 48
            tips.SetToolTip(bWav, "导出 WAV（整曲或当前 Pattern）")
            tips.SetToolTip(bMid, "导出 MIDI（SMF Format 1，PPQ 480）")
            AddHandler bSave.Click, Sub(s As Object, ev As EventArgs) OnSave()
            AddHandler bLoad.Click, Sub(s As Object, ev As EventArgs) OnOpenProject()
            AddHandler bWav.Click, Sub(s As Object, ev As EventArgs) OnExportWav()
            AddHandler bMid.Click, Sub(s As Object, ev As EventArgs) OnExportMidi()
            AddHandler bSet.Click, Sub(s As Object, ev As EventArgs) OnSettings()
            top.Controls.Add(bSave)
            top.Controls.Add(bLoad)
            top.Controls.Add(bWav)
            top.Controls.Add(bMid)
            top.Controls.Add(bSet)

            ' MIDI 状态放在底部主效果区，顶部实在挤不下（见 BuildMaster）
            lbMidi = New Label With {
                .Text = "MIDI: -", .ForeColor = XuTheme.DimColor, .AutoSize = True,
                .Padding = New Padding(4, 4, 0, 0)
            }
            Return top
        End Function

        Private Shared Function BigBtn(text As String) As Button
            Return New Button With {
                .Text = text, .Width = 76, .Height = 38,
                .FlatStyle = FlatStyle.Flat,
                .BackColor = XuTheme.Panel2,
                .ForeColor = XuTheme.TextColor,
                .Font = XuTheme.FontBold,
                .Margin = New Padding(2, 5, 2, 5)
            }
        End Function

        Private Shared Function SmallCaption(text As String) As Label
            Return New Label With {
                .Text = text, .ForeColor = XuTheme.DimColor, .AutoSize = True,
                .Padding = New Padding(6, 15, 0, 0)
            }
        End Function

        ''' <summary>BPM 数字框与滑杆保持同步（避免互相触发死循环）。</summary>
        Private Sub SyncBpmSlider()
            Dim v As Integer = Math.Clamp(CInt(spBpm.Value), 40, 300)
            If tbBpm.Value <> v Then
                loadingUi = True
                tbBpm.Value = v
                loadingUi = False
            End If
        End Sub

        ' ------------------------------------------------------------------
        '  中部网格 / 下部通道条
        ' ------------------------------------------------------------------
        Private Sub BuildGrid(root As TableLayoutPanel)
            gridHost = New Panel With {.Dock = DockStyle.Fill, .AutoScroll = True, .BackColor = XuTheme.Bg}
            grid = New XuGridPanel(project)
            AddHandler grid.ToggleStep, AddressOf OnToggleStep
            AddHandler grid.EditStep, AddressOf OnEditStep
            AddHandler grid.HitPad, Sub(pad As Integer) engine.TriggerPad(pad, 112, False)
            AddHandler grid.ToggleMute, AddressOf OnToggleMute
            AddHandler grid.ToggleSolo, AddressOf OnToggleSolo
            gridHost.Controls.Add(grid)
            root.Controls.Add(gridHost, 0, 1)
        End Sub

        Private Sub BuildStrips(root As TableLayoutPanel)
            stripHost = New Panel With {.Dock = DockStyle.Fill, .AutoScroll = True, .BackColor = XuTheme.Bg}
            root.Controls.Add(stripHost, 0, 2)
            RebuildStrips()
        End Sub

        Private Sub RebuildStrips()
            stripHost.Controls.Clear()
            Dim x As Integer = 2
            For i As Integer = 0 To 15
                Dim s As New XuChannelStrip(i, project) With {.Left = x, .Top = 2}
                AddHandler s.HitPad, Sub(pad As Integer) engine.TriggerPad(pad, 112, False)
                stripHost.Controls.Add(s)
                x += 76
            Next
        End Sub

        Private Sub SyncStrips()
            For Each c As Control In stripHost.Controls
                Dim s As XuChannelStrip = TryCast(c, XuChannelStrip)
                If s IsNot Nothing Then s.SyncFromProject()
            Next
        End Sub

        ' ------------------------------------------------------------------
        '  主效果与电平表
        ' ------------------------------------------------------------------
        Private Function BuildMaster() As Control
            Dim panel As New FlowLayoutPanel With {
                .Dock = DockStyle.Fill, .FlowDirection = FlowDirection.LeftToRight,
                .WrapContents = False, .BackColor = XuTheme.PanelBack, .Padding = New Padding(8, 6, 8, 4)
            }
            AddMasterSlider(panel, "LP 低通", 200, 20000, CInt(project.Master.LpCutoff),
                            Sub(v As Integer) project.Master.LpCutoff = CSng(v), 130)
            AddMasterSlider(panel, "RES 共鸣", 50, 800, CInt(project.Master.LpQ * 100.0F),
                            Sub(v As Integer) project.Master.LpQ = v / 100.0F, 90)
            AddMasterSlider(panel, "HP 高通", 10, 800, CInt(project.Master.HpCutoff),
                            Sub(v As Integer) project.Master.HpCutoff = CSng(v), 90)
            AddMasterSlider(panel, "DRIVE 过载", 100, 800, CInt(project.Master.Drive * 100.0F),
                            Sub(v As Integer) project.Master.Drive = v / 100.0F, 110)
            AddMasterSlider(panel, "VOL 音量", 0, 100, CInt(project.Master.Volume * 100.0F),
                            Sub(v As Integer) project.Master.Volume = v / 100.0F, 100)

            meter = New XuMeter With {.Margin = New Padding(12, 16, 4, 0)}
            panel.Controls.Add(meter)

            lbStatus = New Label With {
                .Text = "CPU 0%  ·  XRUN 0", .ForeColor = XuTheme.DimColor, .AutoSize = True,
                .Padding = New Padding(10, 22, 0, 0), .Font = XuTheme.FontSmall
            }
            panel.Controls.Add(lbStatus)
            ' MIDI 状态（顶部工具栏放不下的部分挪到这里）
            panel.Controls.Add(lbMidi)
            Return panel
        End Function

        Private Shared Sub AddMasterSlider(host As FlowLayoutPanel, label As String, min As Integer, max As Integer,
                                           val As Integer, setter As Action(Of Integer), width As Integer)
            Dim box As New Panel With {.Width = width, .Height = 86}
            box.Controls.Add(New Label With {
                .Text = label, .Left = 0, .Top = 0, .Width = width, .Height = 14,
                .ForeColor = XuTheme.DimColor, .Font = New Font("Microsoft YaHei UI", 7.0F)
            })
            Dim tb As New TrackBar With {
                .Minimum = min, .Maximum = max, .Value = Math.Clamp(val, min, max),
                .Left = -6, .Top = 14, .Width = width + 12, .Height = 32, .TickStyle = TickStyle.None
            }
            ' 数值显示统一用 InvariantCulture，避免某些区域设置下冒出逗号小数点
            Dim valueLabel As New Label With {
                .Text = val.ToString(CultureInfo.InvariantCulture), .Left = 0, .Top = 54,
                .Width = width, .Height = 16, .ForeColor = XuTheme.Orange, .Font = XuTheme.FontSmall
            }
            AddHandler tb.ValueChanged, Sub(s As Object, ev As EventArgs)
                                            setter(tb.Value)
                                            valueLabel.Text = tb.Value.ToString(CultureInfo.InvariantCulture)
                                        End Sub
            box.Controls.Add(tb)
            box.Controls.Add(valueLabel)
            host.Controls.Add(box)
        End Sub

        ' ------------------------------------------------------------------
        '  运行期
        ' ------------------------------------------------------------------
        ''' <summary>
        ''' 打开音频设备。
        ''' 为什么放到后台线程：waveOutOpen 在驱动异常 / 设备被独占 / 远程桌面这类情况下
        ''' 可能阻塞好几秒甚至更久。如果放在构造函数里同步调用，整个窗口都出不来。
        ''' 放到线程池后：界面先显示，声卡就绪后回调线程自动开始供数；
        ''' 打不开也只是少一块声音，导出 WAV 完全不受影响。
        ''' </summary>
        Private Sub StartAudio()
            Dim worker As New System.Threading.WaitCallback(
                Sub(state As Object)
                    Try
                        Dim wo As New XuWaveOut(engine)
                        wo.Start()
                        waveOut = wo
                    Catch ex As Exception
                        Dim msg As String = ex.Message
                        Try
                            BeginInvoke(New Action(
                                Sub()
                                    MessageBox.Show(Me, "无法打开音频设备：" & vbCrLf & msg & vbCrLf & vbCrLf &
                                                    "界面仍可使用，导出 WAV 不受影响。", "音频设备")
                                End Sub))
                        Catch
                            ' 窗口已经关了，忽略
                        End Try
                    End Try
                End Sub)
            System.Threading.ThreadPool.QueueUserWorkItem(worker)
        End Sub

        Private Sub StartMidi()
            Try
                midi = New XuMidiIn()
                If midi.Ok Then
                    lbMidi.Text = "MIDI: 已连接"
                    lbMidi.ForeColor = XuTheme.Orange
                Else
                    lbMidi.Text = "MIDI: 无设备"
                    lbMidi.ForeColor = XuTheme.DimColor
                End If
            Catch
                lbMidi.Text = "MIDI: 不可用"
            End Try
        End Sub

        ''' <summary>33ms 刷新一次：只读引擎快照，绝不推进音序。</summary>
        Private Sub OnTick(sender As Object, e As EventArgs)
            Dim playStep As Integer = If(engine.Playing, engine.StepIndex, -1)
            grid.SetPlayhead(playStep)
            meter.L = engine.MeterL
            meter.R = engine.MeterR
            meter.Invalidate()
            lbStatus.Text = $"CPU {engine.Cpu * 100.0F:0.0}%  ·  XRUN {engine.Xruns}"

            ' MIDI 事件在 UI 线程消费：音频线程完全不知道 MIDI 的存在
            If midi IsNot Nothing AndAlso midi.Ok Then
                Dim m As MidiMessage
                While midi.Queue.TryDequeue(m)
                    HandleMidi(m.Status, m.Data1, m.Data2)
                End While
            End If
        End Sub

        Private Sub HandleMidi(status As Integer, d1 As Integer, d2 As Integer)
            Dim cmd As Integer = status And &HF0
            If cmd = &H90 AndAlso d2 > 0 Then
                ' Note On：按规格书映射表触发；力度 >= 120 视为重音
                Dim pad As Integer = Array.IndexOf(XuConst.MidiNotes, d1)
                If pad >= 0 Then engine.TriggerPad(pad, d2, d2 >= 120)
            ElseIf cmd = &HB0 Then
                If d1 = 7 Then
                    project.Master.Volume = Math.Clamp(d2 / 127.0F, 0.0F, 1.0F)
                ElseIf d1 = 74 Then
                    ' CC74 用对数映射到 20Hz..20kHz，听感才均匀
                    project.Master.LpCutoff = CSng(20.0 * Math.Pow(1000.0, d2 / 127.0))
                ElseIf d1 = 71 Then
                    project.Master.LpQ = Math.Clamp(0.5F + d2 / 127.0F * 7.5F, 0.5F, 8.0F)
                End If
            ElseIf cmd = &HC0 Then
                ' Program Change：切 Pattern
                cbPattern.SelectedIndex = Math.Min(d2, cbPattern.Items.Count - 1)
            ElseIf status = &HFA Then
                engine.Play()
            ElseIf status = &HFC Then
                engine.[Stop]()
            ElseIf status = &HF8 AndAlso engine.ClockExternal Then
                engine.ExternalClockTick()
            End If
        End Sub

        ' ------------------------------------------------------------------
        '  网格交互
        ' ------------------------------------------------------------------
        Private Sub OnToggleStep(pad As Integer, stepIdx As Integer)
            Dim st As XuStep = project.Current.Data(pad)(stepIdx)
            st.[On] = Not st.[On]
            ' 打开格子时立刻试听一次，手感才像真鼓机
            If st.[On] Then engine.TriggerPad(pad, st.Velocity, st.Accent)
            grid.Invalidate()
        End Sub

        Private Sub OnEditStep(pad As Integer, stepIdx As Integer)
            Using d As New XuStepDialog(project.Current.Data(pad)(stepIdx))
                If d.ShowDialog(Me) = DialogResult.OK Then grid.Invalidate()
            End Using
        End Sub

        Private Sub OnToggleMute(pad As Integer)
            project.Pads(pad).Mute = Not project.Pads(pad).Mute
            grid.Invalidate()
        End Sub

        Private Sub OnToggleSolo(pad As Integer)
            project.Pads(pad).Solo = Not project.Pads(pad).Solo
            grid.Invalidate()
        End Sub

        ''' <summary>
        ''' Tap Tempo（规格书第 5 节）：取最近 8 次点击间隔，
        ''' 丢掉超范围值，再去掉最大最小后平均。
        ''' </summary>
        Private Sub OnTap()
            Dim nowSec As Double = Environment.TickCount64 / 1000.0
            If tapTimes.Count > 0 AndAlso nowSec - tapTimes(tapTimes.Count - 1) > 2.0 Then
                tapTimes.Clear()          ' 超过 2 秒视为重新开始
            End If
            tapTimes.Add(nowSec)
            If tapTimes.Count > 8 Then tapTimes.RemoveAt(0)

            If tapTimes.Count >= 2 Then
                Dim iv As New List(Of Double)
                For i As Integer = 1 To tapTimes.Count - 1
                    Dim dt As Double = tapTimes(i) - tapTimes(i - 1)
                    ' 30 BPM..400 BPM 之外的间隔直接丢弃
                    If dt > 0.15 AndAlso dt < 2.0 Then iv.Add(dt)
                Next
                If iv.Count >= 2 Then
                    iv.Remove(iv.Max())
                    iv.Remove(iv.Min())
                End If
                If iv.Count > 0 Then
                    Dim sum As Double = 0
                    For Each dt As Double In iv
                        sum += dt
                    Next
                    Dim avg As Double = sum / iv.Count
                    If avg > 0.0001 Then
                        spBpm.Value = Math.Clamp(CDec(60.0 / avg), 40D, 300D)
                    End If
                End If
            End If
        End Sub

        ' ------------------------------------------------------------------
        '  工程：保存 / 打开 / 导出 / 设置
        ' ------------------------------------------------------------------
        Private Sub OnSave()
            Using d As New SaveFileDialog With {.Filter = "XU-808 工程 (*.xu808)|*.xu808|JSON (*.json)|*.json"}
                If d.ShowDialog(Me) <> DialogResult.OK Then Return
                Try
                    project.Save(d.FileName)
                Catch ex As Exception
                    MessageBox.Show(Me, "保存失败：" & ex.Message)
                End Try
            End Using
        End Sub

        ''' <summary>打开工程。名字不能叫 OnLoad：那是 Form 的可重写方法，会触发隐藏警告。</summary>
        Private Sub OnOpenProject()
            Using d As New OpenFileDialog With {.Filter = "XU-808 工程 (*.xu808)|*.xu808|JSON (*.json)|*.json"}
                If d.ShowDialog(Me) <> DialogResult.OK Then Return
                Try
                    Dim np As XuProject = XuProject.Load(d.FileName)
                    engine.[Stop]()
                    ' project / engine 都是 ReadOnly 字段，最稳妥的做法是重开一个窗口
                    Dim f As New XuMainForm(np)
                    f.Show()
                    Close()
                Catch ex As Exception
                    MessageBox.Show(Me, "加载失败：" & ex.Message)
                End Try
            End Using
        End Sub

        Private Sub OnExportWav()
            Using d As New SaveFileDialog With {.Filter = "WAV 音频 (*.wav)|*.wav", .FileName = "xu808.wav"}
                If d.ShowDialog(Me) <> DialogResult.OK Then Return
                Dim old As Cursor = Cursor
                Cursor = Cursors.WaitCursor
                Try
                    engine.ExportWav(d.FileName, project.Master.SongMode)
                    MessageBox.Show(Me, "已导出：" & vbCrLf & d.FileName, "导出 WAV")
                Catch ex As Exception
                    MessageBox.Show(Me, "导出失败：" & ex.Message)
                Finally
                    Cursor = old
                End Try
            End Using
        End Sub

        Private Sub OnExportMidi()
            Using d As New SaveFileDialog With {.Filter = "MIDI (*.mid)|*.mid", .FileName = "xu808.mid"}
                If d.ShowDialog(Me) <> DialogResult.OK Then Return
                Try
                    engine.ExportMidi(d.FileName, project.Master.SongMode)
                    MessageBox.Show(Me, "已导出：" & vbCrLf & d.FileName, "导出 MIDI")
                Catch ex As Exception
                    MessageBox.Show(Me, "导出失败：" & ex.Message)
                End Try
            End Using
        End Sub

        Private Sub OnSettings()
            Using d As New XuSettingsDialog(project, engine)
                d.ShowDialog(Me)
                ' 设置里可能改了外部时钟 / 拍号，回来刷新一下界面显示
                SyncStrips()
            End Using
        End Sub

        ' ------------------------------------------------------------------
        '  键盘
        ' ------------------------------------------------------------------
        Protected Overrides Function ProcessCmdKey(ByRef msg As Message, keyData As Keys) As Boolean
            If keyData = Keys.Space Then
                engine.Toggle()
                Return True
            End If
            Return MyBase.ProcessCmdKey(msg, keyData)
        End Function

        Protected Overrides Sub OnKeyPress(e As KeyPressEventArgs)
            ' 16 键鼓垫：ZXCVBNMASDFGHJK 依次对应 BD..CH
            Const keys As String = "ZXCVBNMASDFGHJK"
            Dim ch As Char = Char.ToUpperInvariant(e.KeyChar)
            Dim idx As Integer = keys.IndexOf(ch)
            If idx >= 0 Then
                engine.TriggerPad(idx, 110, False)
                e.Handled = True
                Return
            End If
            MyBase.OnKeyPress(e)
        End Sub

        Protected Overrides Sub OnFormClosing(e As FormClosingEventArgs)
            ' 顺序很重要：先停引擎（让 voice 淡出）→ 关音频设备 → 关 MIDI → 停定时器
            Try
                engine.[Stop]()
                If waveOut IsNot Nothing Then waveOut.Dispose()
                If midi IsNot Nothing Then midi.Dispose()
                timer.Stop()
            Finally
                MyBase.OnFormClosing(e)
            End Try
        End Sub
    End Class

    ''' <summary>
    ''' 设置对话框：拍号、外部时钟、工程名、分轨导出。
    ''' 这些是规格书第 5 / 7 节要求、但主界面放不下的选项，收在这里。
    ''' </summary>
    Public Class XuSettingsDialog
        Inherits Form

        Private ReadOnly project As XuProject
        Private ReadOnly engine As XuEngine

        Public Sub New(p As XuProject, eng As XuEngine)
            project = p
            engine = eng
            Text = "设置 - XU-808"
            FormBorderStyle = FormBorderStyle.FixedDialog
            StartPosition = FormStartPosition.CenterParent
            MaximizeBox = False
            MinimizeBox = False
            ClientSize = New Size(420, 300)
            BackColor = XuTheme.PanelBack
            ForeColor = XuTheme.TextColor
            Font = XuTheme.FontNormal

            Dim lay As New TableLayoutPanel With {
                .Dock = DockStyle.Fill, .ColumnCount = 2, .RowCount = 6,
                .Padding = New Padding(14), .BackColor = XuTheme.PanelBack
            }
            lay.ColumnStyles.Add(New ColumnStyle(SizeType.Percent, 45.0F))
            lay.ColumnStyles.Add(New ColumnStyle(SizeType.Percent, 55.0F))
            Controls.Add(lay)

            ' 工程名
            Dim tbName As New TextBox With {
                .Text = project.Name, .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor,
                .BorderStyle = BorderStyle.FixedSingle, .Anchor = AnchorStyles.Left Or AnchorStyles.Right
            }
            AddRow(lay, "工程名", tbName, 0)
            AddHandler tbName.TextChanged, Sub(s As Object, ev As EventArgs) project.Name = tbName.Text

            ' 拍号：分子 1..16，分母 4 或 8
            Dim num As New NumericUpDown With {
                .Minimum = 1D, .Maximum = 16D, .Value = CDec(Math.Clamp(project.Master.BeatsPerBar, 1, 16)),
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .Width = 60
            }
            Dim den As New ComboBox With {
                .DropDownStyle = ComboBoxStyle.DropDownList,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .Width = 60
            }
            den.Items.Add("4")
            den.Items.Add("8")
            den.SelectedIndex = If(project.Master.BeatUnit = 8, 1, 0)
            Dim meterBox As New FlowLayoutPanel With {.AutoSize = True, .BackColor = XuTheme.PanelBack}
            meterBox.Controls.Add(num)
            meterBox.Controls.Add(New Label With {.Text = "/", .AutoSize = True, .ForeColor = XuTheme.DimColor,
                                                  .Padding = New Padding(4, 6, 4, 0)})
            meterBox.Controls.Add(den)
            AddRow(lay, "拍号", meterBox, 1)
            AddHandler num.ValueChanged, Sub(s As Object, ev As EventArgs) project.Master.BeatsPerBar = CInt(num.Value)
            AddHandler den.SelectedIndexChanged, Sub(s As Object, ev As EventArgs)
                                                     project.Master.BeatUnit = If(den.SelectedIndex = 1, 8, 4)
                                                 End Sub

            ' 外部 MIDI 时钟
            Dim cbClock As New CheckBox With {
                .Text = "跟随外部 MIDI 时钟（0xF8）", .Checked = engine.ClockExternal,
                .ForeColor = XuTheme.TextColor, .AutoSize = True, .Anchor = AnchorStyles.Left
            }
            AddRow(lay, "同步", cbClock, 2)
            AddHandler cbClock.CheckedChanged, Sub(s As Object, ev As EventArgs)
                                                   engine.ClockExternal = cbClock.Checked
                                               End Sub

            ' 分轨导出
            Dim bStems As New Button With {
                .Text = "分轨导出 16 个 WAV…", .Width = 170, .Height = 30,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .FlatStyle = FlatStyle.Flat
            }
            AddRow(lay, "分轨", bStems, 3)
            AddHandler bStems.Click, Sub(s As Object, ev As EventArgs)
                                         Using d As New FolderBrowserDialog
                                             If d.ShowDialog(Me) <> DialogResult.OK Then Return
                                             Try
                                                 Dim files As List(Of String) = engine.ExportStems(d.SelectedPath)
                                                 MessageBox.Show(Me, "已写出 " & files.Count.ToString(CultureInfo.InvariantCulture) & " 个分轨文件。", "分轨导出")
                                             Catch ex As Exception
                                                 MessageBox.Show(Me, "分轨导出失败：" & ex.Message)
                                             End Try
                                         End Using
                                     End Sub

            Dim info As New Label With {
                .Text = "采样率 48000Hz · 块大小 256 · 16 声部 × 4 复音" & vbCrLf &
                        "音频由 winmm 回调线程合成，UI 线程不参与音序。",
                .ForeColor = XuTheme.DimColor, .Font = XuTheme.FontSmall, .AutoSize = True,
                .Anchor = AnchorStyles.Left
            }
            AddRow(lay, "引擎", info, 4)

            Dim ok As New Button With {
                .Text = "关闭", .DialogResult = DialogResult.OK, .Width = 84, .Height = 30,
                .BackColor = XuTheme.Panel2, .ForeColor = XuTheme.TextColor, .FlatStyle = FlatStyle.Flat
            }
            Dim btns As New FlowLayoutPanel With {.FlowDirection = FlowDirection.RightToLeft,
                                                  .AutoSize = True, .BackColor = XuTheme.PanelBack}
            btns.Controls.Add(ok)
            lay.Controls.Add(btns, 0, 5)
            lay.SetColumnSpan(btns, 2)
            AcceptButton = ok
            CancelButton = ok
        End Sub

        Private Shared Sub AddRow(lay As TableLayoutPanel, text As String, c As Control, row As Integer)
            Dim lab As New Label With {
                .Text = text, .ForeColor = XuTheme.DimColor, .AutoSize = True,
                .Anchor = AnchorStyles.Left, .Margin = New Padding(3, 10, 3, 3)
            }
            lay.Controls.Add(lab, 0, row)
            lay.Controls.Add(c, 1, row)
        End Sub
    End Class

End Namespace
