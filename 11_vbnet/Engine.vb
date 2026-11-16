' ============================================================================
'  XU-808  Engine.vb —— 音序器、混音、主效果、离线导出（VB.NET 版）
'  作者：徐某
'
'  设计要点（与 07_csharp/Engine.cs 逐条对应）：
'    · SamplePos 是唯一权威时钟，类型 Long，48k 下可跑 600 万年不溢出；
'    · 所有步进 / 微时 / ratchet 都换算成“绝对样本时刻”，先入队再按块切段；
'    · 音频回调路径不 New、不加锁、不做 IO、不抛异常；
'    · 停止时给每个 voice 10ms 淡出，既即时安静又不“咔哒”。
'
'  VB 特有的两个坑，徐某在这里写清楚，免得后来人踩：
'    1) VB 的 CInt/CLng 是“四舍六入五取偶”，不是截断！凡是 C# 里写 (int)x 的地方，
'       这里一律用 Math.Truncate 包一层，否则微时 / ratchet 会整体偏一两个样本。
'    2) VB 没有 volatile 关键字，跨线程标志用 Threading.Volatile.Read/Write 显式读写。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.IO
Imports System.Text
Imports System.Threading

Namespace Xu808

    ''' <summary>数值小工具：把“截断”写成有名字的函数，避免误用 CInt 的银行家舍入。</summary>
    Friend Module XuNum
        Public Function TruncI(x As Double) As Integer
            Return CInt(Math.Truncate(x))
        End Function

        Public Function TruncL(x As Double) As Long
            Return CLng(Math.Truncate(x))
        End Function
    End Module

    ''' <summary>内部事件：什么时候、敲哪个声部、多大力、是否重音。</summary>
    Public Structure XuEvent
        Public T As Long          ' 绝对样本时刻
        Public Pad As Integer     ' 声部号 0..15
        Public Vel As Double      ' 归一化力度（已含重音 1.3×）
        Public Accent As Boolean  ' 重音标志（合成层要据此改音色）
    End Structure

    Public NotInheritable Class XuEngine
        Public Const BlockSize As Integer = 256   ' 256 @48k ≈ 5.33ms，实时延迟更低

        Public Property Project As XuProject

        ' 16 声部 × 4 复音
        Private ReadOnly voices As XuVoice()()
        ' 主效果链的四个 biquad（左右各一对）
        Private ReadOnly lpL As New XuBiquad()
        Private ReadOnly lpR As New XuBiquad()
        Private ReadOnly hpL As New XuBiquad()
        Private ReadOnly hpR As New XuBiquad()

        ' ---------------- 传输 ----------------
        Private playingFlag As Integer          ' 0/1，跨线程用 Volatile 读写
        Private clockExternalFlag As Integer
        Private _samplePos As Long
        Private nextStepAbs As Long
        Private curStep As Integer
        Private globalStep As Long
        Private loopCount As Integer
        Private fillNow As Boolean
        Private clockPulse As Integer

        ''' <summary>是否正在播放（UI 线程写、音频线程读，必须原子）。</summary>
        Public Property Playing As Boolean
            Get
                Return Volatile.Read(playingFlag) <> 0
            End Get
            Set(value As Boolean)
                Volatile.Write(playingFlag, If(value, 1, 0))
            End Set
        End Property

        ''' <summary>外部 MIDI 时钟模式（此时内部步进让位给 0xF8）。</summary>
        Public Property ClockExternal As Boolean
            Get
                Return Volatile.Read(clockExternalFlag) <> 0
            End Get
            Set(value As Boolean)
                Volatile.Write(clockExternalFlag, If(value, 1, 0))
            End Set
        End Property

        ''' <summary>绝对样本位置：唯一时钟。</summary>
        Public Property SamplePos As Long
            Get
                Return _samplePos
            End Get
            Private Set(value As Long)
                _samplePos = value
            End Set
        End Property

        ' ---------------- 队列（全部预分配，回调内只 Add / RemoveRange）----
        Private ReadOnly pending As New List(Of XuEvent)(1024)   ' 音序展开出的待触发事件
        Private ReadOnly events As New List(Of XuEvent)(1024)    ' 本块要执行的事件

        ' 手点（UI / MIDI）：单生产者 / 单消费者无锁环形队列。
        ' 索引用 &HFF 掩码，容量 256，永远不会因为“跨线程改 List”而炸掉。
        Private Const ManualMask As Integer = 255
        Private ReadOnly manualRing As XuEvent() = New XuEvent(ManualMask) {}
        Private manualW As Integer      ' 写指针（只有 UI 线程写）
        Private manualR As Integer      ' 读指针（只有音频线程写）
        Private flushFlag As Integer    ' UI 请求清空 pending，由音频线程执行

        ' 确定性随机源（xorshift32）：概率触发用，种子来自工程
        Private rng As UInteger

        ' ---------------- Pattern / Song ----------------
        Private pendingPattern As Integer = -1
        Private songIndex As Integer
        Private songRepeat As Integer

        ' ---------------- 统计（音频线程写，UI 线程读）----------------
        Public MeterL As Single
        Public MeterR As Single
        Public Peak As Single
        Public Xruns As Integer
        Public Cpu As Single

        ''' <summary>离线分轨：>=0 时只让该轨出声（导出 stems 用）。</summary>
        Public SoloOverride As Integer = -1

        ' 供 UI 显示
        Public ReadOnly Property StepIndex As Integer
            Get
                Return curStep
            End Get
        End Property

        Public ReadOnly Property PatternIndex As Integer
            Get
                Return Project.CurrentPattern
            End Get
        End Property

        Public ReadOnly Property CycleCount As Integer
            Get
                Return loopCount
            End Get
        End Property

        Public Sub New(project As XuProject)
            Me.Project = project
            ' 随机种子直接取工程里的 Seed（默认 &H808808），保证同一工程导出结果可复现
            rng = CUInt(Project.Seed And &H7FFFFFFF) Or 1UI

            voices = New XuVoice(15)() {}
            For p As Integer = 0 To 15
                voices(p) = New XuVoice(XuConst.MaxVoices - 1) {}
                For v As Integer = 0 To XuConst.MaxVoices - 1
                    voices(p)(v) = New XuVoice(p, &H808 + p)
                Next
            Next
        End Sub

        ' ==================================================================
        '  传输
        ' ==================================================================
        Public Sub Play()
            If Playing Then Return
            ' 先把“待清空”标志置上：pending 只能由音频线程清，否则会与渲染并发修改 List
            Volatile.Write(flushFlag, 1)
            curStep = 0
            globalStep = 0
            loopCount = 0
            fillNow = False
            songIndex = 0
            songRepeat = 0
            pendingPattern = -1
            nextStepAbs = SamplePos
            Playing = True                     ' 最后才置播放位，避免半初始化状态被读到
        End Sub

        Public Sub [Stop]()
            Playing = False
            ' 10ms 淡出（480 样本 @48k），既不拖尾也不“咔”
            Dim now As Long = SamplePos
            For p As Integer = 0 To 15
                For v As Integer = 0 To 3
                    voices(p)(v).Kill(now, 480)
                Next
            Next
            Volatile.Write(flushFlag, 1)
        End Sub

        Public Sub Toggle()
            If Playing Then
                [Stop]()
            Else
                Play()
            End If
        End Sub

        ' ==================================================================
        '  时钟与调度
        ' ==================================================================
        ''' <summary>一步（16 分音符）的样本长度：48000*60/BPM/4。</summary>
        Private Function StepLen() As Integer
            Dim bpm As Double = Math.Clamp(CDbl(Project.Master.Bpm), 40.0, 300.0)
            Return Math.Max(64, CInt(Math.Round(XuConst.SampleRate * 60.0 / bpm / 4.0)))
        End Function

        ''' <summary>32 位 xorshift：确定性、无分配，用于概率触发。</summary>
        Private Function NextRand() As UInteger
            Dim x As UInteger = rng
            x = x Xor (x << 13)      ' 注意：VB 的 ^ 是乘方，异或要写 Xor
            x = x Xor (x >> 17)      ' UInteger 的 >> 是逻辑右移
            x = x Xor (x << 5)
            rng = x
            Return x
        End Function

        ''' <summary>条件触发判定（ALWAYS / FILL / NOT_FILL / EVERY_2 / EVERY_4）。</summary>
        Private Function CondOk(cond As Integer) As Boolean
            Select Case cond
                Case XuCond.Fill
                    Return fillNow
                Case XuCond.NotFill
                    Return Not fillNow
                Case XuCond.Every2
                    Return (loopCount Mod 2) = 0
                Case XuCond.Every4
                    Return (loopCount Mod 4) = 0
                Case Else
                    Return True
            End Select
        End Function

        ''' <summary>把一个 16 分步展开成绝对样本事件（含 swing / micro / ratchet / 概率）。</summary>
        Private Sub EmitStep(stepAbs As Long)
            Dim pat As XuPattern = Project.Current
            ' 注意：局部变量不能叫 stepLen，否则会和 StepLen() 函数同名（VB 不区分大小写）
            Dim stepSamples As Integer = StepLen()

            ' Swing：奇数步（1,3,5…，即 0 基的 curStep 为奇数）向后偏移半个 swing 量
            Dim swingOff As Integer = 0
            If (curStep And 1) = 1 Then
                swingOff = TruncI(stepSamples * CDbl(Project.Master.Swing) * 0.5)
            End If
            Dim baseT As Long = stepAbs + swingOff

            For pad As Integer = 0 To 15
                Dim tl As Integer = Math.Clamp(pat.TrackLen(pad), 1, 64)
                Dim idx As Integer = CInt(globalStep Mod tl)     ' polymeter：轨道步号 = 全局步号 % 轨道长度
                Dim st As XuStep = pat.Data(pad)(idx)
                If Not st.[On] Then Continue For
                If Not CondOk(st.Cond) Then Continue For
                If st.Prob <= 0 Then Continue For
                ' 概率判定：每步触发时刻只掷一次骰子（ratchet 的每个子事件共享结果）
                If st.Prob < 100 AndAlso CInt(NextRand() Mod 100UI) >= st.Prob Then Continue For

                Dim vel As Double = Math.Clamp(st.Velocity / 127.0, 0.02, 1.0)
                If st.Accent Then vel = Math.Min(1.25, vel * 1.3)   ' 重音音量 +30%
                Dim rt As Integer = Math.Clamp(st.Ratchet, 1, 8)
                For i As Integer = 0 To rt - 1
                    Dim ev As New XuEvent
                    ' 子事件时刻 = 步起点 + swing + micro + i*stepSamples/ratchet
                    ev.T = baseT + st.Micro + TruncL(i * CDbl(stepSamples) / rt)
                    ev.Pad = pad
                    ev.Vel = vel * Math.Pow(0.92, i)                ' 力度逐个递减 8%
                    ev.Accent = st.Accent
                    pending.Add(ev)
                Next
            Next
        End Sub

        ''' <summary>一步结束后推进；切 Pattern / Song 只允许发生在循环边界。</summary>
        Private Sub AdvanceAfterStep()
            Dim pat As XuPattern = Project.Current
            curStep += 1
            globalStep += 1
            If curStep < Math.Max(1, pat.Steps) Then Return

            ' 走到 Pattern 末尾：回到第 0 步，循环计数 +1
            curStep = 0
            loopCount += 1
            fillNow = (loopCount Mod 4) = 3        ' 每 4 个循环的第 4 个当作过门

            ' 1) 手动切 Pattern 的请求优先
            If pendingPattern >= 0 Then
                Project.CurrentPattern = Math.Clamp(pendingPattern, 0, Project.Patterns.Count - 1)
                pendingPattern = -1
                songRepeat = 0
                Return
            End If

            ' 2) Song 链：按重复次数推进，结束后循环回开头
            If Project.Master.SongMode AndAlso Project.Song.Entries.Count > 0 Then
                Dim entry As XuSongEntry = Project.Song.Entries(songIndex Mod Project.Song.Entries.Count)
                songRepeat += 1
                If songRepeat >= Math.Max(1, entry.Repeats) Then
                    songRepeat = 0
                    songIndex = (songIndex + 1) Mod Project.Song.Entries.Count
                    Project.CurrentPattern = Math.Clamp(Project.Song.Entries(songIndex).Pattern,
                                                        0, Project.Patterns.Count - 1)
                End If
            End If
        End Sub

        ''' <summary>收集本块事件：手点 + 音序 + 之前挂起的子事件，并按时刻排序。</summary>
        Private Sub CollectEvents(b0 As Long, b1 As Long)
            events.Clear()

            ' UI 请求的清空延迟到这里执行：pending 只归音频线程所有
            If Volatile.Read(flushFlag) <> 0 Then
                pending.Clear()
                Volatile.Write(flushFlag, 0)
            End If

            ' ---- 手点事件（无锁环形队列）----
            Dim rd As Integer = manualR
            Dim wr As Integer = Volatile.Read(manualW)
            While rd <> wr
                Dim ev As XuEvent = manualRing(rd)
                ' 时间戳落在未来块的事件必须留在队列里：提前触发是绝对错误的
                If ev.T >= b1 Then Exit While
                If ev.T < b0 Then ev.T = b0          ' 迟到的手点：立刻响，不补时间
                events.Add(ev)
                rd = (rd + 1) And ManualMask
            End While
            manualR = rd

            ' ---- 音序器：本块内可能有多个步边界（BPM 高时一块跨两步）----
            If Playing AndAlso Not ClockExternal Then
                Dim guard As Integer = 0
                While nextStepAbs < b1 AndAlso guard < 64
                    guard += 1
                    EmitStep(Math.Max(b0, nextStepAbs))
                    nextStepAbs += StepLen()
                    AdvanceAfterStep()
                End While
            End If

            ' ---- 之前挂起的 ratchet 子事件：只取本块的，未来的必须保留 ----
            Dim keep As Integer = 0
            For i As Integer = 0 To pending.Count - 1
                Dim ev As XuEvent = pending(i)
                If ev.T < b1 Then
                    If ev.T < b0 Then ev.T = b0
                    events.Add(ev)
                Else
                    pending(keep) = ev
                    keep += 1
                End If
            Next
            If keep < pending.Count Then pending.RemoveRange(keep, pending.Count - keep)

            ' 排序：事件数量很少（每块最多几十个），插入排序比 Sort 更省也更稳
            For i As Integer = 1 To events.Count - 1
                Dim key As XuEvent = events(i)
                Dim j As Integer = i - 1
                While j >= 0 AndAlso events(j).T > key.T
                    events(j + 1) = events(j)
                    j -= 1
                End While
                events(j + 1) = key
            Next
        End Sub

        ''' <summary>外部 MIDI 时钟：每 6 个 0xF8（24PPQN）推进一个 16 分步。</summary>
        Public Sub ExternalClockTick()
            If Not Playing Then Return
            clockPulse += 1
            If clockPulse < 6 Then Return
            clockPulse = 0
            EmitStep(SamplePos)
            AdvanceAfterStep()
            nextStepAbs = SamplePos + StepLen()
        End Sub

        ' ==================================================================
        '  发声
        ' ==================================================================
        ''' <summary>该声部此刻是否允许出声（Mute / Solo / 分轨导出）。</summary>
        Private Function Audible(pad As Integer) As Boolean
            If SoloOverride >= 0 Then Return pad = SoloOverride
            Dim anySolo As Boolean = False
            For i As Integer = 0 To 15
                If Project.Pads(i).Solo Then
                    anySolo = True
                    Exit For
                End If
            Next
            Dim p As XuPadParams = Project.Pads(pad)
            If p.Mute Then Return False
            If anySolo AndAlso Not p.Solo Then Return False
            Return True
        End Function

        ''' <summary>触发一个 voice；满了就抢占“最老”的那个（voice stealing）。</summary>
        Private Sub TriggerVoice(pad As Integer, atSample As Long, vel As Double, accent As Boolean)
            ' CHOKE 组 1：闭镲(15) / 开镲(14) 谁触发都先掐掉正在响的开镲
            If pad = 15 Then
                For Each v As XuVoice In voices(14)
                    v.Kill(atSample, 16)
                Next
            End If
            If pad = 14 Then
                For Each v As XuVoice In voices(14)
                    v.Kill(atSample, 16)
                Next
            End If

            Dim target As XuVoice = Nothing
            For i As Integer = 0 To XuConst.MaxVoices - 1
                If Not voices(pad)(i).Active Then
                    target = voices(pad)(i)
                    Exit For
                End If
            Next
            If target Is Nothing Then
                ' 4 个都在响：挑起点最早的，先 64 样本淡出再复用（避免硬切出“咔”）
                target = voices(pad)(0)
                For i As Integer = 1 To XuConst.MaxVoices - 1
                    If voices(pad)(i).Start < target.Start Then target = voices(pad)(i)
                Next
                target.Kill(atSample, 64)
            End If
            target.Trigger(atSample, vel, accent)
        End Sub

        ' ==================================================================
        '  主渲染
        ' ==================================================================
        ''' <summary>
        ''' 渲染 frames 个立体声样本到 l / r（长度必须 >= frames）。
        ''' 实时回调和离线导出共用这一个函数 —— 这就是“导出不变样”的技术保证。
        ''' </summary>
        Public Sub RenderBlock(frames As Integer, l As Single(), r As Single())
            Dim b0 As Long = SamplePos
            Dim b1 As Long = b0 + frames
            Array.Clear(l, 0, frames)
            Array.Clear(r, 0, frames)

            CollectEvents(b0, b1)

            ' 按事件时刻把本块切成若干段：每段内所有 voice 一起推进，事件点上换声
            Dim cur As Long = b0
            For i As Integer = 0 To events.Count - 1
                Dim ev As XuEvent = events(i)
                If ev.T > cur Then
                    RenderVoices(l, r, b0, cur, ev.T)
                    cur = ev.T
                End If
                If ev.T < b1 Then TriggerVoice(ev.Pad, ev.T, ev.Vel, ev.Accent)
            Next
            If cur < b1 Then RenderVoices(l, r, b0, cur, b1)

            ' ---- 主效果链（顺序固定：高通 → 低通 → 过载 → 软限幅 → 主音量）----
            Dim m As XuMaster = Project.Master

            If m.HpOn Then
                hpL.SetHighPass(CDbl(m.HpCutoff), CDbl(m.HpQ))
                hpR.SetHighPass(CDbl(m.HpCutoff), CDbl(m.HpQ))
                For i As Integer = 0 To frames - 1
                    l(i) = CSng(hpL.Process(l(i)))
                    r(i) = CSng(hpR.Process(r(i)))
                Next
            End If

            If m.LpOn Then
                lpL.SetLowPass(CDbl(m.LpCutoff), CDbl(m.LpQ))
                lpR.SetLowPass(CDbl(m.LpCutoff), CDbl(m.LpQ))
                For i As Integer = 0 To frames - 1
                    l(i) = CSng(lpL.Process(l(i)))
                    r(i) = CSng(lpR.Process(r(i)))
                Next
            End If

            Dim drive As Single = Math.Clamp(m.Drive, 1.0F, 8.0F)
            Dim norm As Single = CSng(Math.Tanh(drive))     ' 归一化：drive=1 时也是单位增益
            Dim vol As Single = Math.Clamp(m.Volume, 0.0F, 1.0F)
            Dim pl As Single = 0, pr As Single = 0
            For i As Integer = 0 To frames - 1
                Dim xl As Single = l(i)
                Dim xr As Single = r(i)
                If drive > 1.001F Then
                    xl = CSng(Math.Tanh(xl * drive)) / norm
                    xr = CSng(Math.Tanh(xr * drive)) / norm
                End If
                xl = xl / (1.0F + Math.Abs(xl))     ' 软限幅 x/(1+|x|)，永远不削波
                xr = xr / (1.0F + Math.Abs(xr))
                xl *= vol
                xr *= vol
                l(i) = xl
                r(i) = xr
                If Math.Abs(xl) > pl Then pl = Math.Abs(xl)
                If Math.Abs(xr) > pr Then pr = Math.Abs(xr)
            Next
            ' 电平表带缓慢回落，UI 看起来才有“跳动感”
            MeterL = Math.Max(pl, MeterL * 0.75F)
            MeterR = Math.Max(pr, MeterR * 0.75F)
            Peak = Math.Max(MeterL, MeterR)

            SamplePos = b1

            ' 回收过期 voice：超过 4 秒一律认定结束，保证不会“叠加残留”
            For p As Integer = 0 To 15
                For v As Integer = 0 To 3
                    Dim vo As XuVoice = voices(p)(v)
                    If vo.Active AndAlso b0 - vo.Start > 4L * XuConst.SampleRate Then vo.Active = False
                Next
            Next
        End Sub

        ''' <summary>把 [a,b) 段的全部活动 voice 叠加进输出。</summary>
        Private Sub RenderVoices(l As Single(), r As Single(), b0 As Long, a As Long, b As Long)
            If b <= a Then Return
            ' 注意：offset 传 0，让 voice 自己用 (a-b0) 定位；若这里再传 (a-b0)，声音会整体后移
            Dim n As Integer = CInt(b - a)
            For pad As Integer = 0 To 15
                If Not Audible(pad) Then Continue For
                Dim p As XuPadParams = Project.Pads(pad)
                For v As Integer = 0 To XuConst.MaxVoices - 1
                    Dim vo As XuVoice = voices(pad)(v)
                    If vo.Active Then vo.Render(l, r, 0, n, b0, b, p)
                Next
            Next
        End Sub

        ' ==================================================================
        '  GUI 接口
        ' ==================================================================
        ''' <summary>手点一个鼓垫（UI 线程调用，写入无锁环形队列）。</summary>
        Public Sub TriggerPad(pad As Integer, velocity As Integer, accent As Boolean)
            Dim w As Integer = manualW
            Dim nxt As Integer = (w + 1) And ManualMask
            If nxt = manualR Then Return          ' 队列满：宁可丢一次手点，也不能在音频线程加锁
            Dim ev As XuEvent
            ev.T = SamplePos + 32                 ' 约 0.7ms 后发声，给回调一点缓冲余量
            ev.Pad = Math.Clamp(pad, 0, 15)
            ev.Vel = Math.Max(0.02, velocity / 127.0)
            ev.Accent = accent
            manualRing(w) = ev
            Volatile.Write(manualW, nxt)
        End Sub

        ''' <summary>请求切 Pattern：播放中推到下一步边界，停止时立即生效。</summary>
        Public Sub RequestPattern(index As Integer)
            If Playing Then
                pendingPattern = index
            Else
                Project.CurrentPattern = Math.Clamp(index, 0, Project.Patterns.Count - 1)
            End If
        End Sub

        ' ==================================================================
        '  离线渲染 / 导出
        ' ==================================================================
        ''' <summary>离线渲染若干秒，返回交织立体声浮点数组（L,R,L,R...）。</summary>
        Private Function RenderSeconds(seconds As Double, Optional soloPad As Integer = -1) As Single()
            Dim total As Integer = TruncI(seconds * XuConst.SampleRate)
            Dim l As Single() = New Single(total - 1) {}
            Dim r As Single() = New Single(total - 1) {}
            Dim bufL As Single() = New Single(BlockSize - 1) {}
            Dim bufR As Single() = New Single(BlockSize - 1) {}

            SoloOverride = soloPad
            Play()
            Dim done As Integer = 0
            While done < total
                Dim n As Integer = Math.Min(BlockSize, total - done)
                RenderBlock(n, bufL, bufR)
                Array.Copy(bufL, 0, l, done, n)
                Array.Copy(bufR, 0, r, done, n)
                done += n
            End While
            SoloOverride = -1

            ' 交织成立体声，方便直接写 WAV
            Dim inter As Single() = New Single(total * 2 - 1) {}
            For i As Integer = 0 To total - 1
                inter(i * 2) = l(i)
                inter(i * 2 + 1) = r(i)
            Next
            Return inter
        End Function

        ''' <summary>渲染当前 Pattern 若干遍（默认 1 遍 + 3 秒尾巴）。</summary>
        Public Function RenderPattern(Optional repeats As Integer = 1) As Single()
            ' 用工程副本渲染：现场的播放位置、voice 状态都不会被导出的过程污染
            Dim copy As XuProject = Project.Clone()
            Dim eng As New XuEngine(copy)
            Dim sec As Double = (copy.Current.Steps * Math.Max(1, repeats) * eng.StepLen()) / CDbl(XuConst.SampleRate) + 3.0
            Return eng.RenderSeconds(sec)
        End Function

        ''' <summary>渲染整条 Song 链（结束后自然收尾 3 秒）。</summary>
        Public Function RenderSong() As Single()
            Dim copy As XuProject = Project.Clone()
            Dim eng As New XuEngine(copy)
            Dim steps As Integer = 0
            If copy.Song.Entries.Count > 0 Then
                For Each e As XuSongEntry In copy.Song.Entries
                    Dim idx As Integer = Math.Clamp(e.Pattern, 0, copy.Patterns.Count - 1)
                    steps += copy.Patterns(idx).Steps * Math.Max(1, e.Repeats)
                Next
            End If
            If steps = 0 Then steps = copy.Current.Steps
            Dim sec As Double = steps * eng.StepLen() / CDbl(XuConst.SampleRate) + 3.0
            Return eng.RenderSeconds(sec)
        End Function

        ''' <summary>写 16bit PCM WAV（输入是立体声交织 float）。</summary>
        Public Shared Sub WriteWav(path As String, interleaved As Single())
            Dim frames As Integer = interleaved.Length \ 2
            Using fs As New FileStream(path, FileMode.Create, FileAccess.Write)
                Using bw As New BinaryWriter(fs)
                    Dim dataBytes As Integer = frames * 4        ' 2 声道 × 2 字节
                    WriteTag(bw, "RIFF")
                    bw.Write(36 + dataBytes)                     ' RIFF 块大小 = 36 + data
                    WriteTag(bw, "WAVE")
                    WriteTag(bw, "fmt ")
                    bw.Write(16)                                 ' fmt 块长度
                    bw.Write(CShort(1))                          ' PCM
                    bw.Write(CShort(2))                          ' 声道数
                    bw.Write(XuConst.SampleRate)                 ' 采样率
                    bw.Write(XuConst.SampleRate * 4)             ' 字节率
                    bw.Write(CShort(4))                          ' 块对齐
                    bw.Write(CShort(16))                         ' 位深
                    WriteTag(bw, "data")
                    bw.Write(dataBytes)
                    For i As Integer = 0 To interleaved.Length - 1
                        Dim x As Single = Math.Clamp(interleaved(i), -1.0F, 1.0F)
                        ' 注意：C# 的 (short)float 是截断，VB 的 CShort 是四舍五入，
                        ' 这里显式 Math.Truncate，保证两个版本的 WAV 逐字节一致
                        bw.Write(CShort(Math.Truncate(CDbl(x) * 32767.0)))
                    Next
                End Using
            End Using
        End Sub

        ''' <summary>四个字符的标签（RIFF/WAVE/...），按 ASCII 逐字节写，避免编码差异。</summary>
        Private Shared Sub WriteTag(bw As BinaryWriter, tag As String)
            For Each ch As Char In tag
                bw.Write(CByte(AscW(ch)))
            Next
        End Sub

        ''' <summary>导出整段音频（song=True 走 Song 链，否则当前 Pattern）。</summary>
        Public Sub ExportWav(path As String, song As Boolean)
            If song Then
                WriteWav(path, RenderSong())
            Else
                WriteWav(path, RenderPattern())
            End If
        End Sub

        ''' <summary>分轨导出：每轨一个 WAV，供后期混音。</summary>
        Public Function ExportStems(folder As String, Optional prefix As String = "xu808") As List(Of String)
            Dim files As New List(Of String)
            For pad As Integer = 0 To 15
                ' 每轨都用全新引擎：滤波器 / 限幅器状态绝不能从上一轨带过来
                Dim copy As XuProject = Project.Clone()
                Dim eng As New XuEngine(copy)
                Dim sec As Double = copy.Current.Steps * eng.StepLen() / CDbl(XuConst.SampleRate) + 2.5
                Dim audio As Single() = eng.RenderSeconds(sec, pad)
                ' 局部变量不能叫 path：VB 不区分大小写，会遮蔽 System.IO.Path
                Dim filePath As String = Path.Combine(folder, $"{prefix}_stem_{pad:00}.wav")
                WriteWav(filePath, audio)
                files.Add(filePath)
            Next
            Return files
        End Function

        ' ==================================================================
        '  MIDI 导出（SMF Format 1，PPQ=480，16 分 = 120 tick）
        ' ==================================================================
        Private Structure MidiNote
            Public Tick As Integer
            Public Note As Integer
            Public Vel As Integer
        End Structure

        ''' <summary>可变长数量（VLQ）：MIDI 文件的时间差与长度都用它。</summary>
        Private Shared Sub WriteVlq(buf As List(Of Byte), value As Integer)
            Dim v As Integer = Math.Max(0, value)
            Dim tmp As New List(Of Byte)
            tmp.Add(CByte(v And &H7F))
            v >>= 7
            While v > 0
                tmp.Insert(0, CByte((v And &H7F) Or &H80))
                v >>= 7
            End While
            buf.AddRange(tmp)
        End Sub

        Public Sub ExportMidi(path As String, song As Boolean)
            Dim p As XuProject = Project
            Dim ppq As Integer = 480
            Dim bpm As Double = Math.Clamp(CDbl(p.Master.Bpm), 40.0, 300.0)
            Dim stepSamples As Double = XuConst.SampleRate * 60.0 / bpm / 4.0

            ' 1) 先排 Pattern 播放顺序
            Dim seq As New List(Of Integer)
            If song AndAlso p.Song.Entries.Count > 0 Then
                For Each e As XuSongEntry In p.Song.Entries
                    For k As Integer = 0 To Math.Max(1, e.Repeats) - 1
                        seq.Add(Math.Clamp(e.Pattern, 0, p.Patterns.Count - 1))
                    Next
                Next
            Else
                seq.Add(p.CurrentPattern)
            End If

            ' 2) 展开成音符事件（含 swing / micro / ratchet）
            Dim notes As New List(Of MidiNote)
            Dim cursor As Long = 0
            For Each pi As Integer In seq
                Dim pat As XuPattern = p.Patterns(pi)
                For s As Integer = 0 To pat.Steps - 1
                    ' swing 换算成 tick：奇数步偏移 swing*0.5*120
                    Dim swingTick As Integer = 0
                    If (s Mod 2) = 1 Then
                        swingTick = TruncI(stepSamples * CDbl(p.Master.Swing) * 0.5 / stepSamples * 120)
                    End If
                    For pad As Integer = 0 To 15
                        Dim tl As Integer = Math.Clamp(pat.TrackLen(pad), 1, 64)
                        Dim idx As Integer = CInt((cursor + s) Mod tl)
                        Dim st As XuStep = pat.Data(pad)(idx)
                        If Not st.[On] OrElse st.Prob <= 0 Then Continue For

                        Dim vel As Integer = Math.Clamp(TruncI(st.Velocity * (If(st.Accent, 1.3F, 1.0F))), 1, 127)
                        Dim baseTick As Integer = CInt((cursor + s) * 120)
                        Dim microTick As Integer = CInt(Math.Round(st.Micro / stepSamples * 120.0))
                        Dim rt As Integer = Math.Clamp(st.Ratchet, 1, 8)
                        For i As Integer = 0 To rt - 1
                            Dim nt As New MidiNote
                            nt.Tick = Math.Max(0, baseTick + swingTick + microTick + TruncI(i * 120.0 / rt))
                            nt.Note = XuConst.MidiNotes(pad)
                            nt.Vel = vel
                            notes.Add(nt)
                        Next
                    Next
                Next
                cursor += pat.Steps
            Next

            notes.Sort(Function(a As MidiNote, b As MidiNote)
                           ' 先按时刻，再按音高：保证同一刻的多个音符顺序稳定
                           If a.Tick <> b.Tick Then Return a.Tick.CompareTo(b.Tick)
                           Return a.Note.CompareTo(b.Note)
                       End Function)

            ' 3) 组装 MTrk
            Dim track As New List(Of Byte)
            Dim nameBytes As Byte() = Encoding.ASCII.GetBytes("XU-808 by XuMou")
            WriteVlq(track, 0) : track.Add(CByte(&HFF)) : track.Add(CByte(&H3)) : WriteVlq(track, nameBytes.Length)
            track.AddRange(nameBytes)

            Dim tempo As Integer = TruncI(60000000.0 / bpm)
            WriteVlq(track, 0) : track.Add(CByte(&HFF)) : track.Add(CByte(&H51)) : track.Add(CByte(&H3))
            track.Add(CByte((tempo >> 16) And &HFF))
            track.Add(CByte((tempo >> 8) And &HFF))
            track.Add(CByte(tempo And &HFF))

            ' 拍号：分子 / 2 的幂次分母 / 24 MIDI clocks per metronome / 8 32nd per quarter
            WriteVlq(track, 0) : track.Add(CByte(&HFF)) : track.Add(CByte(&H58)) : track.Add(CByte(&H4))
            track.Add(CByte(Math.Clamp(p.Master.BeatsPerBar, 1, 16)))
            track.Add(CByte(TruncI(Math.Log(Math.Max(1, p.Master.BeatUnit), 2.0))))
            track.Add(CByte(24))
            track.Add(CByte(8))

            Dim last As Integer = 0
            For Each n As MidiNote In notes
                WriteVlq(track, Math.Max(0, n.Tick - last))
                track.Add(CByte(&H99))       ' Note On，通道 10（0 基通道 9）
                track.Add(CByte(n.Note))
                track.Add(CByte(n.Vel))
                WriteVlq(track, 10)          ' 10 tick ≈ 5ms 的鼓点长度
                track.Add(CByte(&H89))       ' Note Off
                track.Add(CByte(n.Note))
                track.Add(CByte(64))
                last = n.Tick + 10
            Next
            WriteVlq(track, 0) : track.Add(CByte(&HFF)) : track.Add(CByte(&H2F)) : track.Add(CByte(&H0))   ' End of Track

            ' 4) 写文件头 MThd + MTrk
            Using fs As New FileStream(path, FileMode.Create, FileAccess.Write)
                Using bw As New BinaryWriter(fs)
                    WriteTag(bw, "MThd")
                    bw.Write(6)
                    bw.Write(CShort(1))          ' Format 1
                    bw.Write(CShort(1))          ' 1 条轨道（鼓轨）
                    bw.Write(CShort(ppq))        ' PPQ=480
                    WriteTag(bw, "MTrk")
                    bw.Write(track.Count)
                    bw.Write(track.ToArray())
                End Using
            End Using
        End Sub

    End Class

End Namespace
