' ============================================================================
'  XU-808  Program.vb —— 程序入口（VB.NET / WinForms）
'  作者：徐某
'
'  支持三种无界面模式，方便在构建机上自动验证（WinExe 没有控制台，
'  所以结果一律落到文本文件里，路径就写报告本身）：
'      XU808-VBNet.exe --selftest          渲染自检，写 selftest.wav + selftest.txt
'      XU808-VBNet.exe --export out.wav    导出当前 Pattern
'      XU808-VBNet.exe --midi out.mid      导出 MIDI
'  正常双击（无参数）则进入图形界面。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.Collections.Generic
Imports System.Globalization
Imports System.IO
Imports System.Windows.Forms

Namespace Xu808

    Public Module Program

        <STAThread>
        Public Sub Main(args As String())
            Dim project As New XuProject()

            If args.Length >= 1 AndAlso args(0) = "--selftest" Then
                SelfTest(project)
                Return
            End If

            If args.Length >= 2 AndAlso args(0) = "--export" Then
                Dim eng As New XuEngine(project)
                eng.ExportWav(args(1), False)
                File.WriteAllText(args(1) & ".ok", "OK")
                Return
            End If

            If args.Length >= 2 AndAlso args(0) = "--midi" Then
                Dim eng As New XuEngine(project)
                eng.ExportMidi(args(1), False)
                File.WriteAllText(args(1) & ".ok", "OK")
                Return
            End If

            ' ---- 图形界面 ----
            Application.EnableVisualStyles()
            Application.SetCompatibleTextRenderingDefault(False)

            ' 兜底：任何未捕获异常都写一份日志到 EXE 同目录，方便用户回报问题
            AddHandler Application.ThreadException, Sub(s As Object, ev As System.Threading.ThreadExceptionEventArgs)
                                                        LogCrash(ev.Exception)
                                                        MessageBox.Show("程序遇到错误，已写入 app_error.log：" & vbCrLf & ev.Exception.Message,
                                                                        "XU-808", MessageBoxButtons.OK, MessageBoxIcon.Error)
                                                    End Sub
            AddHandler AppDomain.CurrentDomain.UnhandledException,
                Sub(s As Object, ev As UnhandledExceptionEventArgs)
                    LogCrash(TryCast(ev.ExceptionObject, Exception))
                End Sub

            Application.Run(New XuMainForm(project))
        End Sub

        ''' <summary>把异常堆栈写进 app_error.log（WinExe 没有控制台，只能落盘）。</summary>
        Private Sub LogCrash(ex As Exception)
            Try
                ' 局部变量不能叫 path（会遮蔽 System.IO.Path）
                Dim logPath As String = Path.Combine(AppContext.BaseDirectory, "app_error.log")
                File.AppendAllText(logPath, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) &
                                   vbCrLf & If(ex IsNot Nothing, ex.ToString(), "(无异常对象)") & vbCrLf & vbCrLf)
            Catch
            End Try
        End Sub

        ''' <summary>
        ''' 自检：离线渲染 → 写非空 WAV → 检查峰值/RMS → 写 SELFTEST OK 标记。
        ''' 全部输出落在 EXE 同目录，路径与字节数都会写进 selftest.txt，
        ''' 这样即使没有控制台也能一眼看出产物在哪里、多大。
        ''' </summary>
        Private Sub SelfTest(p As XuProject)
            Dim lines As New List(Of String)
            lines.Add("XU-808 鼓机 · VB.NET 自检报告（作者：徐某）")
            lines.Add("framework=" & Environment.Version.ToString())
            lines.Add("base=" & AppContext.BaseDirectory)

            Dim okAll As Boolean = True

            ' ---- 1) 渲染默认 Pattern ----
            Dim eng As New XuEngine(p)
            Dim audio As Single() = eng.RenderPattern()
            Dim frames As Integer = audio.Length \ 2

            Dim peak As Single = 0
            Dim energy As Double = 0
            Dim nan As Boolean = False
            For i As Integer = 0 To audio.Length - 1
                Dim a As Single = Math.Abs(audio(i))
                If Single.IsNaN(audio(i)) OrElse Single.IsInfinity(audio(i)) Then nan = True
                If a > peak Then peak = a
                energy += CDbl(audio(i)) * CDbl(audio(i))
            Next
            Dim rms As Single = CSng(Math.Sqrt(energy / Math.Max(1, audio.Length)))

            Dim wavPath As String = Path.Combine(AppContext.BaseDirectory, "selftest.wav")
            XuEngine.WriteWav(wavPath, audio)
            Dim wavBytes As Long = If(File.Exists(wavPath), New FileInfo(wavPath).Length, 0L)

            lines.Add($"pattern_samples={frames} peak={peak.ToString("F4", CultureInfo.InvariantCulture)} rms={rms.ToString("F4", CultureInfo.InvariantCulture)}")
            lines.Add($"wav={wavPath}")
            lines.Add($"wav_bytes={wavBytes}")
            lines.Add($"wav_seconds={(frames / CDbl(XuConst.SampleRate)).ToString("F2", CultureInfo.InvariantCulture)}")

            Dim okMain As Boolean = frames > XuConst.SampleRate AndAlso peak > 0.01F AndAlso peak <= 1.0F AndAlso Not nan AndAlso wavBytes > 1024
            lines.Add("check_pattern=" & If(okMain, "OK", "FAIL"))
            okAll = okAll AndAlso okMain

            ' ---- 2) 16 声部同时敲：验证不爆音、软限幅生效 ----
            Dim p2 As XuProject = p.Clone()
            Dim pat As XuPattern = p2.Patterns(0)
            For pad As Integer = 0 To 15
                For s As Integer = 1 To 63
                    pat.Data(pad)(s).[On] = False
                Next
                Dim st As XuStep = pat.Data(pad)(0)
                st.[On] = True
                st.Velocity = 120
                st.Accent = True
                st.Prob = 100
            Next
            Dim eng2 As New XuEngine(p2)
            Dim audio2 As Single() = eng2.RenderPattern()
            Dim peak2 As Single = 0
            For i As Integer = 0 To audio2.Length - 1
                Dim a As Single = Math.Abs(audio2(i))
                If a > peak2 Then peak2 = a
            Next
            Dim wav2Path As String = Path.Combine(AppContext.BaseDirectory, "selftest_16voice.wav")
            XuEngine.WriteWav(wav2Path, audio2)
            Dim wav2Bytes As Long = If(File.Exists(wav2Path), New FileInfo(wav2Path).Length, 0L)
            Dim ok16 As Boolean = peak2 > 0.05F AndAlso peak2 <= 1.0F AndAlso wav2Bytes > 1024
            lines.Add($"wav_16voice={wav2Path}")
            lines.Add($"wav_16voice_bytes={wav2Bytes} peak={peak2.ToString("F4", CultureInfo.InvariantCulture)}")
            lines.Add("check_16voice=" & If(ok16, "OK", "FAIL"))
            okAll = okAll AndAlso ok16

            ' ---- 3) MIDI 导出 ----
            Dim midPath As String = Path.Combine(AppContext.BaseDirectory, "selftest.mid")
            eng.ExportMidi(midPath, False)
            Dim midBytes As Long = If(File.Exists(midPath), New FileInfo(midPath).Length, 0L)
            Dim okMidi As Boolean = midBytes > 32
            lines.Add($"midi={midPath}")
            lines.Add($"midi_bytes={midBytes}")
            lines.Add("check_midi=" & If(okMidi, "OK", "FAIL"))
            okAll = okAll AndAlso okMidi

            ' ---- 4) 工程存档往返：保存 → 加载 → 对比关键字段 ----
            Dim projPath As String = Path.Combine(AppContext.BaseDirectory, "selftest.xu808")
            p.Save(projPath)
            Dim re As XuProject = XuProject.Load(projPath)
            Dim okProj As Boolean = (re.Patterns.Count = p.Patterns.Count) AndAlso
                                    (re.Current.Steps = p.Current.Steps) AndAlso
                                    (Math.Abs(re.Pads(0).Tune - p.Pads(0).Tune) < 0.0001F) AndAlso
                                    (re.Master.Bpm = p.Master.Bpm)
            lines.Add($"project={projPath}")
            lines.Add($"project_bytes={New FileInfo(projPath).Length}")
            lines.Add("check_project=" & If(okProj, "OK", "FAIL"))
            okAll = okAll AndAlso okProj

            ' ---- 结论 ----
            lines.Add(If(okAll, "SELFTEST OK", "SELFTEST FAIL"))
            Dim reportPath As String = Path.Combine(AppContext.BaseDirectory, "selftest.txt")
            File.WriteAllText(reportPath, String.Join(vbCrLf, lines), System.Text.Encoding.UTF8)

            ' 有控制台时顺手回显一份（没有也不会报错）
            Try
                For Each line As String In lines
                    Console.WriteLine(line)
                Next
                Console.WriteLine("report=" & reportPath)
            Catch
            End Try

            Environment.ExitCode = If(okAll, 0, 2)
        End Sub

    End Module

End Namespace
