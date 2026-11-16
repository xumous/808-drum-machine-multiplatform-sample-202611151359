' ============================================================================
'  XU-808  Dsp.vb —— 十六声部模拟鼓合成器（VB.NET 版）
'  作者：徐某
'
'  这一层是“纯函数 + 轻状态”：
'    · 所有包络都是时间的解析函数（Exp / Sin），因此不需要逐样本维护 ADSR 状态；
'    · 唯一需要跨块保留的是滤波器 z1/z2 和噪声读取指针；
'    · 每次敲击只重置 Start/StopAt/Gain，绝不重新分配数组。
'
'  这样做的直接好处：上一槌声音到时间自然结束（或到 StopAt 淡出），
'  绝不可能“越叠越多”；同时 CPU 占用可预测，16 声部 × 4 复音也不会爆表。
'
'  与 07_csharp/Dsp.cs 的公式逐一对应，连乘法顺序都保持不变，
'  以便两个版本做 A/B 对比时波形能逐样本重合。
' ============================================================================

Option Strict On
Option Explicit On

Namespace Xu808

    ''' <summary>全局音频常量。所有语言版本必须一致。</summary>
    Public Module XuConst
        Public Const SampleRate As Integer = 48000   ' 采样率（规格书第 1 节）
        Public Const PadCount As Integer = 16        ' 声部数
        Public Const MaxVoices As Integer = 4        ' 每个声部的复音数

        ' 声部代号与中文名：下标即编号，任何移植都不得改动顺序（规格书第 2 节）
        Public ReadOnly Codes As String() = _
            {"BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC", "RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH"}

        Public ReadOnly NamesCn As String() = _
            {"底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加",
             "边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲"}

        ' MIDI 音高映射（规格书第 7 节）
        Public ReadOnly MidiNotes As Integer() = _
            {36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42}
    End Module

    ''' <summary>
    ''' RBJ 双二阶滤波器（Direct Form II Transposed）。
    '''
    ''' 知识点：音频里所有“低通 / 高通 / 带通”都可以化成同一条差分方程，
    ''' 只是系数不同；z1/z2 就是“滤波器记忆”。DF2T 结构在同精度下
    ''' 数值稳定性最好，且系数变化时不会有内部状态被瞬间放大。
    ''' </summary>
    Public NotInheritable Class XuBiquad
        Private b0 As Double = 1, b1 As Double, b2 As Double, a1 As Double, a2 As Double  ' 归一化系数（a0 恒为 1）
        Private z1 As Double, z2 As Double                                                ' 两条延迟线
        Private kind As String = "bypass"                                                 ' 当前滤波器类型，用于跳过重复计算
        Private lastFc As Double, lastQ As Double

        ''' <summary>低通：保留低频。fc 为截止频率（Hz），q 为品质因数。</summary>
        Public Sub SetLowPass(fc As Double, q As Double)
            fc = Math.Clamp(fc, 20.0, XuConst.SampleRate * 0.49)   ' 上限取 Nyquist 的 98%，避免系数爆掉
            q = Math.Clamp(q, 0.1, 20.0)
            ' 参数几乎没变就不重算：每块重算 6 次三角函数在 16 声部时并不便宜
            If kind = "lp" AndAlso Math.Abs(fc - lastFc) < 0.5 AndAlso Math.Abs(q - lastQ) < 0.001 Then Return
            Dim w0 As Double = 2 * Math.PI * fc / XuConst.SampleRate
            Dim cw As Double = Math.Cos(w0)
            Dim alpha As Double = Math.Sin(w0) / (2 * q)
            Dim a0 As Double = 1 + alpha
            b0 = (1 - cw) * 0.5 / a0
            b1 = (1 - cw) / a0
            b2 = b0                                 ' 低通分子对称：b2 恒等于 b0
            a1 = (-2 * cw) / a0
            a2 = (1 - alpha) / a0
            kind = "lp" : lastFc = fc : lastQ = q
        End Sub

        ''' <summary>高通：砍掉低频，用于去直流与“电话音”。</summary>
        Public Sub SetHighPass(fc As Double, q As Double)
            fc = Math.Clamp(fc, 10.0, XuConst.SampleRate * 0.49)
            q = Math.Clamp(q, 0.1, 20.0)
            If kind = "hp" AndAlso Math.Abs(fc - lastFc) < 0.5 AndAlso Math.Abs(q - lastQ) < 0.001 Then Return
            Dim w0 As Double = 2 * Math.PI * fc / XuConst.SampleRate
            Dim cw As Double = Math.Cos(w0)
            Dim alpha As Double = Math.Sin(w0) / (2 * q)
            Dim a0 As Double = 1 + alpha
            b0 = (1 + cw) * 0.5 / a0
            b1 = -(1 + cw) / a0
            b2 = b0
            a1 = (-2 * cw) / a0
            a2 = (1 - alpha) / a0
            kind = "hp" : lastFc = fc : lastQ = q
        End Sub

        ''' <summary>常量峰值增益带通（CLAP 的噪声成形用它）。</summary>
        Public Sub SetBandPass(fc As Double, q As Double)
            fc = Math.Clamp(fc, 20.0, XuConst.SampleRate * 0.49)
            q = Math.Clamp(q, 0.1, 30.0)
            If kind = "bp" AndAlso Math.Abs(fc - lastFc) < 0.5 AndAlso Math.Abs(q - lastQ) < 0.001 Then Return
            Dim w0 As Double = 2 * Math.PI * fc / XuConst.SampleRate
            Dim cw As Double = Math.Cos(w0)
            Dim alpha As Double = Math.Sin(w0) / (2 * q)
            Dim a0 As Double = 1 + alpha
            b0 = alpha / a0
            b1 = 0
            b2 = -alpha / a0
            a1 = (-2 * cw) / a0
            a2 = (1 - alpha) / a0
            kind = "bp" : lastFc = fc : lastQ = q
        End Sub

        ''' <summary>对一个样本执行差分方程；实时路径调用，不能分配、不能抛异常。</summary>
        Public Function Process(x As Double) As Double
            Dim y As Double = b0 * x + z1          ' y = b0*x + z1
            z1 = b1 * x - a1 * y + z2              ' DF2T 状态推进
            z2 = b2 * x - a2 * y
            Return y
        End Function

        ''' <summary>触发新声音前清空状态，避免上一槌的残余造成“噗”声。</summary>
        Public Sub Reset()
            z1 = 0 : z2 = 0
        End Sub
    End Class

    ''' <summary>
    ''' 单声部用户参数（0..1，level 可取到 1.25）。
    '''
    ''' 这里用 Property 而不是 Field，是为了让 System.Text.Json 能直接序列化
    ''' 进 .xu808 工程文件（字段默认不参与序列化）。字段名/属性名与 C# 版一致。
    ''' </summary>
    Public Class XuPadParams
        Public Property Tune As Single = 0.5F
        Public Property Decay As Single = 0.5F
        Public Property Tone As Single = 0.5F
        Public Property Snappy As Single = 0.55F
        Public Property Tail As Single = 0.45F
        Public Property Level As Single = 0.75F
        Public Property Pan As Single = 0.5F
        Public Property Mute As Boolean = False
        Public Property Solo As Boolean = False
    End Class

    ''' <summary>
    ''' 一个鼓槌（voice）。所有包络都由“年龄 t（秒）”解析计算。
    ''' </summary>
    Public NotInheritable Class XuVoice
        Public Pad As Integer                ' 所属声部（0..15）
        Public Active As Boolean             ' 是否在发声
        Public Start As Long                 ' 触发绝对样本
        Public StopAt As Long = Long.MaxValue ' 强制结束时刻（掐断）
        Public Gain As Double = 1.0          ' 力度增益（已含重音）
        Public Accent As Boolean             ' 重音标志，合成层要据此改音色
        Public FadeLen As Integer = 64       ' 掐断淡出长度（样本）

        ' 预生成噪声池：8192 个样本 = 32KB，固定种子保证每次运行音色一致。
        ' 实时路径只做循环读指针，绝不 New 数组。
        Private ReadOnly noise As Single()
        Private npos As Integer

        ' 每个 voice 自己带滤波器状态：16 声部 × 4 复音各 3 个 = 192 个 biquad，
        ' 这点内存换来的是“完全独立的音色”，互相不会串状态。
        Private ReadOnly bp As New XuBiquad()
        Private ReadOnly hp As New XuBiquad()
        Private ReadOnly bp2 As New XuBiquad()

        ' 镲族的 6 个非谐波比例（Metallic 簇）。C# 版是在 Synth 里每次 new，
        ' 徐某把它提到类级只读数组：音色完全一致，但音频线程彻底零分配。
        Private Shared ReadOnly MetalRatios As Double() = {1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523}

        Public Sub New(pad As Integer, seed As Integer)
            Me.Pad = pad
            noise = New Single(8191) {}
            ' 与 C# 版同种子、同算法 → 噪声序列逐样本一致（.NET 的 Random 兼容实现）
            Dim rng As New Random(seed + pad * 7919)
            For i As Integer = 0 To noise.Length - 1
                noise(i) = CSng(rng.NextDouble() * 2 - 1)
            Next
        End Sub

        ''' <summary>触发：重置全部状态并记录起点。注意不会“提前结束”上一槌。</summary>
        Public Sub Trigger(atSample As Long, gain As Double, accent As Boolean)
            Active = True
            Start = atSample
            StopAt = Long.MaxValue
            Me.Gain = gain
            Me.Accent = accent
            npos = 0
            bp.Reset() : hp.Reset() : bp2.Reset()
        End Sub

        ''' <summary>掐断：在 atSample 之后做 fade 个样本的线性淡出，避免“咔”。</summary>
        Public Sub Kill(atSample As Long, Optional fade As Integer = 24)
            If Not Active Then Return
            StopAt = Math.Min(StopAt, atSample)
            FadeLen = Math.Max(1, fade)
        End Sub

        ''' <summary>循环取噪声，避免在音频路径里分配数组。
        ''' 函数名叫 NextNoise 而不是 Noise：VB 不区分大小写，叫 Noise 会和噪声池字段冲突。</summary>
        Private Function NextNoise() As Double
            If npos >= noise.Length Then npos = 0
            Dim v As Double = noise(npos)
            npos += 1
            Return v
        End Function

        ''' <summary>
        ''' 把 [a, b) 区间叠加进左右声道。
        ''' 参数 offset 是“区间起点在输出缓冲里的下标”，引擎传 0，
        ''' 由 (a - b0) 自己定位；若两处都偏移就会整体后移一段（徐某踩过这个坑）。
        ''' </summary>
        Public Sub Render(left As Single(), right As Single(), offset As Integer, frames As Integer,
                          b0 As Long, b1 As Long, p As XuPadParams)
            If Not Active Then Return
            Dim a As Long = Math.Max(b0, Start)
            Dim b As Long = Math.Min(b1, StopAt)
            If b <= a Then
                ' 本块内没有可发声区间；若已经越过掐断点，直接回收 voice
                If b1 > StopAt Then Active = False
                Return
            End If
            Dim m As Integer = CInt(b - a)
            Dim idx As Integer = CInt(a - b0) + offset

            ' 等功率声像：pan=0.5 时左右各 cos/sin(π/4)≈0.707，总功率恒定
            Dim pan As Double = Math.Clamp(CDbl(p.Pan), 0.0, 1.0)
            Dim lg As Double = Math.Cos(pan * Math.PI * 0.5)
            Dim rg As Double = Math.Sin(pan * Math.PI * 0.5)
            ' 0.42 是 16 声部同时敲击也不至于打爆的经验总线系数
            Dim g As Double = Gain * Math.Max(CDbl(p.Level), 0.0) * 0.42

            For i As Integer = 0 To m - 1
                Dim t As Double = (a + i - Start) / CDbl(XuConst.SampleRate)
                Dim s As Double = Synth(t, p)
                ' 掐断淡出：距离 StopAt 越近增益越小
                If StopAt < Long.MaxValue / 2 Then
                    Dim dist As Long = StopAt - (a + i)
                    If dist < FadeLen Then
                        If dist < 0 Then
                            s = 0
                        Else
                            s *= CDbl(dist) / FadeLen
                        End If
                    End If
                End If
                left(idx + i) += CSng(s * g * lg)
                right(idx + i) += CSng(s * g * rg)
            Next

            ' 任何声音超过 4 秒一律回收：防止某个包络参数极端时 voice 永远不释放
            If b1 - Start > 4L * XuConst.SampleRate Then Active = False
        End Sub

        ''' <summary>16 种声部的合成分派（与规格书第 2 节逐条对应）。</summary>
        Private Function Synth(t As Double, p As XuPadParams) As Double
            Dim pad As Integer = Me.Pad
            Dim tune As Double = CDbl(p.Tune)
            Dim decay As Double = CDbl(p.Decay)
            Dim tone As Double = CDbl(p.Tone)

            ' ---------------- 0 底鼓 BD：桥接 T 型正弦 + 音高包络 + 点击噪声 ----
            If pad = 0 Then
                Dim fEnd As Double = 40 + tune * 40                        ' 40–80Hz
                Dim fStart As Double = fEnd * (1.6 + tone * 1.9)           ' 起始音高（音高包络起点）
                Dim tau As Double = 0.018 + (1 - decay) * 0.012            ' 包络时间常数
                Dim df As Double = fStart - fEnd
                ' 相位 = 2π ∫f(t)dt；f(t)=fEnd+df*exp(-t/tau) 的积分是解析式
                Dim phase As Double = 2 * Math.PI * (fEnd * t + df * tau * (1 - Math.Exp(-t / tau)))
                Dim dec As Double = (0.12 + decay * 1.48) * (If(Accent, 1.18, 1.0))  ' 重音延长 18%
                Dim amp As Double = Math.Exp(-t / dec)
                Dim n As Double = NextNoise()
                hp.SetHighPass(1800 + tone * 3000, 0.8)                    ' 点击只用极短的高频噪声
                Dim click As Double = hp.Process(n) * Math.Exp(-t / 0.0035) * (0.06 + tone * 0.32)
                ' tanh 给鼓身加“饱和”的厚度，同时不会像硬削波那样出毛刺
                Dim body As Double = Math.Tanh(Math.Sin(phase) * (1.15 + decay * 1.2)) * 0.92
                Return body * amp + click
            End If

            ' ---------------- 1 军鼓 SD：双正弦(180/330Hz) + 带通噪声 -----------
            If pad = 1 Then
                Dim f1 As Double = 150 + tune * 110
                Dim f2 As Double = f1 * (1.78 + tone * 0.22)
                Dim decO As Double = 0.055 + decay * 0.14
                Dim osc As Double = 0.55 * Math.Sin(2 * Math.PI * f1 * t) * Math.Exp(-t / decO) _
                                  + 0.34 * Math.Sin(2 * Math.PI * f2 * t) * Math.Exp(-t / (decO * 0.72))
                Dim n As Double = NextNoise()
                bp.SetBandPass(1200 + CDbl(p.Snappy) * 3800, 0.9 + tone * 0.8)
                Dim nz As Double = bp.Process(n)
                ' 重音时噪声衰减更快 → 听起来更“紧”、更突出
                Dim decN As Double = (0.075 + decay * 0.16) * (If(Accent, 0.85, 1.0))
                nz *= Math.Exp(-t / decN) * (0.25 + CDbl(p.Snappy) * 0.7)
                Return osc * 0.7 + nz
            End If

            ' ---------------- 2..7 嗵鼓 / 康加：正弦音高下滑 + 点击 ------------
            If pad >= 2 AndAlso pad <= 7 Then
                Dim baseF As Double
                Dim decayS As Double
                Dim clickAmt As Double
                Dim sweep As Double
                If pad <= 4 Then
                    ' LT / MT / HT：80 / 120 / 170Hz，下滑更大、点击更重
                    baseF = If(pad = 2, 80.0, If(pad = 3, 120.0, 170.0))
                    decayS = 0.15 + decay * 0.75
                    clickAmt = 0.04 + tone * 0.22
                    sweep = 1.9
                Else
                    ' LC / MC / HC：220 / 300 / 400Hz，短促、点击轻
                    baseF = If(pad = 5, 220.0, If(pad = 6, 300.0, 400.0))
                    decayS = 0.10 + decay * 0.55
                    clickAmt = 0.02 + tone * 0.12
                    sweep = 1.35
                End If
                Dim f0 As Double = baseF * (0.6 + tune)                    ' 0.6–1.6 倍
                Dim tau As Double = If(pad <= 4, 0.035, 0.02)
                Dim phase As Double = 2 * Math.PI * (f0 * t + f0 * (sweep - 1) * tau * (1 - Math.Exp(-t / tau)))
                Dim amp As Double = Math.Exp(-t / decayS)
                Dim n As Double = NextNoise()
                hp.SetHighPass(2500, 0.7)
                Dim click As Double = hp.Process(n) * Math.Exp(-t / 0.004) * clickAmt
                Return Math.Sin(phase) * amp * 0.9 + click
            End If

            ' ---------------- 8 边击 RS：1.7kHz 方波脉冲 + 短噪声 --------------
            If pad = 8 Then
                Dim f As Double = 1200 + tune * 1400
                Dim dec As Double = 0.012 + decay * 0.05
                Dim sq As Double = If(Math.Sin(2 * Math.PI * f * t) >= 0, 1.0, -1.0)
                bp.SetBandPass(f, 2.2)                                     ' 高 Q 带通把方波磨成“木头片”声
                Dim body As Double = bp.Process(sq) * Math.Exp(-t / dec)
                Dim n As Double = NextNoise()
                hp.SetHighPass(3000, 0.8)
                Dim tick As Double = hp.Process(n) * Math.Exp(-t / 0.004) * 0.35
                Return body * 0.8 + tick
            End If

            ' ---------------- 9 击木 CL：2.5kHz 正弦，30ms --------------------
            If pad = 9 Then
                Dim f As Double = 2000 + tune * 1000
                Dim dec As Double = 0.018 + decay * 0.035
                Return Math.Sin(2 * Math.PI * f * t) * Math.Exp(-t / dec) * 0.9
            End If

            ' ---------------- 10 拍手 CP：4 段噪声爆发 + 尾巴 ------------------
            If pad = 10 Then
                Dim n As Double = NextNoise()
                bp.SetBandPass(800 + tone * 1700, 1.1)
                Dim nz As Double = bp.Process(n)
                Dim env As Double = 0
                ' 四次 9ms 间隔的爆发模拟一群人的手掌错落
                For k As Integer = 0 To 3
                    Dim tk As Double = k * 0.009
                    If t >= tk Then env += Math.Exp(-(t - tk) / 0.011) * 0.55
                Next
                Dim tail As Double = Math.Exp(-t / (0.05 + decay * 0.35)) * (0.15 + CDbl(p.Tail) * 0.55)
                Return nz * (env + tail)
            End If

            ' ---------------- 11 沙锤 MA：高通噪声，40ms -----------------------
            If pad = 11 Then
                Dim n As Double = NextNoise()
                hp.SetHighPass(4000 + tone * 5000, 0.9)
                Dim nz As Double = hp.Process(n)
                ' 1.5ms 起音 + 指数衰减，避免“咔”
                Dim env As Double = (1 - Math.Exp(-t / 0.0015)) * Math.Exp(-t / (0.018 + decay * 0.12))
                Return nz * env * 0.85
            End If

            ' ---------------- 12 牛铃 CB：540/800Hz 双方波 + 带通 --------------
            If pad = 12 Then
                Dim f As Double = 400 + tune * 300
                Dim sq1 As Double = If(Math.Sin(2 * Math.PI * f * t) >= 0, 1.0, -1.0)
                Dim sq2 As Double = If(Math.Sin(2 * Math.PI * f * 1.48 * t) >= 0, 1.0, -1.0)
                bp.SetBandPass(2400 + tone * 1200, 1.4)
                Dim body As Double = bp.Process(sq1 * 0.5 + sq2 * 0.5)
                Dim dec As Double = 0.05 + decay * 0.35
                Return body * Math.Exp(-t / dec) * 0.85
            End If

            ' ---------------- 13..15 镲族 CY / OH / CH：6 方波金属簇 + 高通 ----
            Dim root As Double = 320 * (0.75 + tune * 0.5)
            Dim metal As Double = 0
            For Each r As Double In MetalRatios
                metal += If(Math.Sin(2 * Math.PI * root * r * t) >= 0, 1.0, -1.0)
            Next
            metal /= MetalRatios.Length
            hp.SetHighPass(5200 + tone * 2600, 0.8)
            metal = hp.Process(metal)
            bp.SetBandPass(7600 + tone * 3600, 0.9)
            metal = bp.Process(metal)
            ' CH 0.03–0.25s / OH 0.1–0.8s / CY 0.2–2.0s（规格书第 3 节）
            Dim decH As Double
            If pad = 15 Then
                decH = 0.028 + decay * 0.20
            ElseIf pad = 14 Then
                decH = 0.12 + decay * 0.65
            Else
                decH = 0.25 + decay * 1.75
            End If
            Dim envH As Double = Math.Exp(-t / decH)
            Dim outp As Double = metal * envH
            If pad = 13 Then
                ' 吊镲额外加一层 10kHz 带通噪声，增加“空气感”
                Dim n As Double = NextNoise()
                bp2.SetBandPass(10000, 0.7)
                outp += bp2.Process(n) * envH * 0.12
            End If
            Return outp * 0.75
        End Function
    End Class

End Namespace
