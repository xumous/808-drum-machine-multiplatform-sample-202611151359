' ============================================================================
'  XU-808  Project.vb —— 工程数据模型与 JSON 存档（VB.NET 版）
'  作者：徐某
'
'  数据字段名与 07_csharp / 01_python_qt 保持完全一致：
'    On / Velocity / Accent / Prob / Micro / Ratchet / Cond
'  这样同一个 .xu808 工程可以在不同语言版本之间互通。
'  读取时用 PropertyNameCaseInsensitive = True，所以 Python 版写的
'  小写键（on / velocity / ...）也能被顺利载入。
'
'  注意：C# 版把参数放在 public 字段上，而 System.Text.Json 默认不序列化字段，
'  导致那边的工程文件里 pads 是空对象。徐某在 VB 版改用 Property，
'  这样“保存 → 加载 → 完整还原”才真的成立（规格书第 11 节第 9 条）。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.IO
Imports System.Text.Json
Imports System.Text.Json.Serialization

Namespace Xu808

    ''' <summary>一个格子的全部属性（对应规格书第 4 节）。</summary>
    Public Class XuStep
        ' "On" 是 VB 关键字，必须用方括号转义；属性名仍然是 "On"，JSON 键也仍是 "On"
        Public Property [On] As Boolean = False             ' 是否启用
        Public Property Velocity As Integer = 100           ' 力度 1..127
        Public Property Accent As Boolean = False           ' 重音
        Public Property Prob As Integer = 100               ' 概率 0..100
        Public Property Micro As Integer = 0                ' 微时偏移（样本，负=提前）
        Public Property Ratchet As Integer = 1              ' 滚奏 1..8
        Public Property Cond As Integer = 0                 ' 条件触发（XuCond）

        ''' <summary>浅拷贝：属性对话框点“取消”时不能污染原数据。</summary>
        Public Function Clone() As XuStep
            Return DirectCast(Me.MemberwiseClone(), XuStep)
        End Function
    End Class

    ''' <summary>16 轨 × 最多 64 步的 Pattern。</summary>
    Public Class XuPattern
        Public Property Name As String = "PATTERN"          ' 显示名
        Public Property Steps As Integer = 16               ' Pattern 长度 1..64
        Public Property Fill As Boolean = False             ' 预留：整段过门标记
        ' 外层 16 条轨道，内层固定 64 步：实时线程可以无脑索引，不需要判断越界
        Public Property Data As New List(Of List(Of XuStep))
        Public Property TrackLen As Integer() = New Integer(15) {}

        Public Sub New()
            For i As Integer = 0 To 15
                Dim row As New List(Of XuStep)(64)
                For s As Integer = 0 To 63
                    row.Add(New XuStep())
                Next
                Data.Add(row)
                TrackLen(i) = 16                            ' 默认每轨 16 步（polymeter 可单独改）
            Next
        End Sub

        ''' <summary>取格子；越界时做一次收敛，防止 UI 与音频线程偶发错位。</summary>
        Public Function Cell(pad As Integer, stepIdx As Integer) As XuStep
            Return Data(Math.Clamp(pad, 0, 15))(Math.Clamp(stepIdx, 0, 63))
        End Function

        ''' <summary>写入默认 808 律动；与 C# 版逐格一致（含 BD 的 3 连滚奏）。</summary>
        Public Sub SetDemoBeat()
            For p As Integer = 0 To 15
                For s As Integer = 0 To 63
                    Data(p)(s) = New XuStep()                ' 整表清空
                Next
            Next
            Steps = 16

            ' 底鼓：正拍 + 反拍 + 末尾补一下
            SetStep(0, 0, 118, True)
            SetStep(0, 8, 105, False)
            SetStep(0, 14, 90, False)
            ' 军鼓：2、4 拍
            SetStep(1, 4, 110, False)
            SetStep(1, 12, 110, True)
            ' 闭镲：八分音符，重音落在一三拍
            For s As Integer = 0 To 15 Step 2
                SetStep(15, s, If(s Mod 4 = 0, 96, 84), False)
            Next
            SetStep(14, 14, 92, False)                       ' 开镲点缀
            SetStep(10, 12, 80, False)                       ' 拍手
            ' 沙锤：反拍十六分
            For s As Integer = 1 To 15 Step 2
                SetStep(11, s, 42, False)
            Next
            ' 最后一格做 3 连滚奏，进下一循环更有味道
            SetStep(0, 15, 88, False)
            Data(0)(15).Ratchet = 3
            Name = "PATTERN 1"
        End Sub

        Private Sub SetStep(pad As Integer, stepIdx As Integer, vel As Integer, accent As Boolean)
            Dim st As XuStep = Data(pad)(stepIdx)
            st.[On] = True
            st.Velocity = vel
            st.Accent = accent
        End Sub
    End Class

    ''' <summary>主输出与全局设置（规格书第 5、6 节）。</summary>
    Public Class XuMaster
        Public Property Volume As Single = 0.8F             ' 主音量 0..1
        Public Property Drive As Single = 1.35F             ' 过载 1..8
        Public Property LpCutoff As Single = 18000.0F       ' 低通截止 200..20000
        Public Property LpQ As Single = 0.9F                ' 低通 Q 0.5..8
        Public Property HpCutoff As Single = 28.0F          ' 高通截止 20..800
        Public Property HpQ As Single = 0.7F                ' 高通 Q
        Public Property LpOn As Boolean = True
        Public Property HpOn As Boolean = True
        Public Property Bpm As Single = 120.0F              ' 40..300
        Public Property Swing As Single = 0.0F              ' 0..0.75
        Public Property BeatsPerBar As Integer = 4          ' 拍号分子
        Public Property BeatUnit As Integer = 4             ' 拍号分母
        Public Property SongMode As Boolean = False         ' Song 链开关
    End Class

    ''' <summary>歌曲链的一项：播放哪个 Pattern、重复几次。</summary>
    Public Class XuSongEntry
        Public Property Pattern As Integer = 0
        Public Property Repeats As Integer = 1
    End Class

    Public Class XuSong
        Public Property Enabled As Boolean = False
        Public Property Entries As New List(Of XuSongEntry)
    End Class

    ''' <summary>顶层工程对象（JSON 根）。</summary>
    Public Class XuProject
        Public Property Format As String = "XU808"          ' 固定标识，便于外部工具识别
        Public Property Version As Integer = 1
        Public Property Name As String = "徐某的 808 工程"
        Public Property Seed As Integer = &H808808
        Public Property CurrentPattern As Integer = 0
        Public Property Master As New XuMaster()
        Public Property Song As New XuSong()
        Public Property Pads As New List(Of XuPadParams)
        Public Property Patterns As New List(Of XuPattern)

        Public Sub New()
            For i As Integer = 0 To 15
                Pads.Add(New XuPadParams())
            Next
            ' 16 个 Pattern 槽位，第 1 个带示例律动，其余留白
            For i As Integer = 0 To 15
                Dim p As New XuPattern With {.Name = $"PATTERN {i + 1}"}
                If i = 0 Then p.SetDemoBeat()
                Patterns.Add(p)
            Next
        End Sub

        ''' <summary>当前 Pattern（只在 UI 线程和音频线程各读一次，越界即收敛）。</summary>
        <JsonIgnore>
        Public ReadOnly Property Current As XuPattern
            Get
                Return Patterns(Math.Clamp(CurrentPattern, 0, Patterns.Count - 1))
            End Get
        End Property

        ''' <summary>保存为 UTF-8 JSON；WriteIndented 让用户可以直接用记事本改。</summary>
        Public Sub Save(path As String)
            Dim opt As New JsonSerializerOptions With {
                .WriteIndented = True,
                .Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
            }
            File.WriteAllText(path, JsonSerializer.Serialize(Me, opt), System.Text.Encoding.UTF8)
        End Sub

        ''' <summary>加载工程；缺失字段自动落到构造函数默认值（前向/后向兼容）。</summary>
        Public Shared Function Load(path As String) As XuProject
            Dim opt As New JsonSerializerOptions With {
                .PropertyNameCaseInsensitive = True,   ' 兼容 Python 版的小写键名
                .IncludeFields = True,                 ' 兼容别家用字段存的数据
                .ReadCommentHandling = JsonCommentHandling.Skip,
                .AllowTrailingCommas = True
            }
            Dim p As XuProject = JsonSerializer.Deserialize(Of XuProject)(File.ReadAllText(path), opt)
            If p Is Nothing Then Throw New InvalidDataException("工程文件为空或格式错误")

            ' 防御性补齐：老版本/手改过的工程可能缺 pattern 或 pad
            If p.Patterns Is Nothing Then p.Patterns = New List(Of XuPattern)
            While p.Patterns.Count < 16
                p.Patterns.Add(New XuPattern With {.Name = $"PATTERN {p.Patterns.Count + 1}"})
            End While
            If p.Pads Is Nothing Then p.Pads = New List(Of XuPadParams)
            While p.Pads.Count < 16
                p.Pads.Add(New XuPadParams())
            End While
            If p.Master Is Nothing Then p.Master = New XuMaster()
            If p.Song Is Nothing Then p.Song = New XuSong()
            If p.Song.Entries Is Nothing Then p.Song.Entries = New List(Of XuSongEntry)
            ' 每个 pattern 的格子数也要补：老工程可能只有 16 步
            For Each pat As XuPattern In p.Patterns
                If pat.Data Is Nothing Then pat.Data = New List(Of List(Of XuStep))
                While pat.Data.Count < 16
                    Dim row As New List(Of XuStep)
                    For s As Integer = 0 To 63
                        row.Add(New XuStep())
                    Next
                    pat.Data.Add(row)
                End While
                For pi As Integer = 0 To pat.Data.Count - 1
                    If pat.Data(pi) Is Nothing Then pat.Data(pi) = New List(Of XuStep)
                    While pat.Data(pi).Count < 64
                        pat.Data(pi).Add(New XuStep())
                    End While
                Next
                If pat.TrackLen Is Nothing OrElse pat.TrackLen.Length < 16 Then
                    Dim old As Integer() = pat.TrackLen
                    pat.TrackLen = New Integer(15) {}
                    For i As Integer = 0 To 15
                        pat.TrackLen(i) = If(old IsNot Nothing AndAlso i < old.Length, old(i), 16)
                    Next
                End If
                pat.Steps = Math.Clamp(pat.Steps, 1, 64)
            Next
            p.CurrentPattern = Math.Clamp(p.CurrentPattern, 0, p.Patterns.Count - 1)
            Return p
        End Function

        ''' <summary>深拷贝：离线导出用，绝不影响现场播放的那一份。</summary>
        Public Function Clone() As XuProject
            ' 用 JSON 往返做深拷贝：代码量最少，且不会漏字段
            Dim opt As New JsonSerializerOptions With {.IncludeFields = True}
            Dim json As String = JsonSerializer.Serialize(Me, opt)
            Dim p As XuProject = JsonSerializer.Deserialize(Of XuProject)(json, opt)
            Return If(p, New XuProject())
        End Function
    End Class

    ''' <summary>步进条件触发（规格书第 4 节）。</summary>
    Public Module XuCond
        Public Const Always As Integer = 0
        Public Const Fill As Integer = 1
        Public Const NotFill As Integer = 2
        Public Const Every2 As Integer = 3
        Public Const Every4 As Integer = 4
        Public ReadOnly Names As String() = {"总是", "仅过门", "非过门", "每2循环", "每4循环"}
    End Module

End Namespace
