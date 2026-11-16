' ============================================================================
'  XU-808  Audio.vb —— winmm waveOut 输出与 MIDI 输入（VB.NET 版）
'  作者：徐某
'
'  为什么不用 NAudio：这是“零 NuGet”的硬性要求。
'  winmm.dll 从 Windows 3.1 就躺在系统目录里，永远不需要额外安装，
'  也不会因为 NuGet 源不可达而让离线构建失败。
'
'  延迟：4 个 256 样本缓冲 = 4 × 5.33ms ≈ 21ms。
'  回调里只做“渲染 → 转 int16 → waveOutWrite”，不分配、不加锁、不弹窗。
'  合成全部发生在系统音频线程里，UI 线程一次都不参与 —— 这是硬性要求。
' ============================================================================

Option Strict On
Option Explicit On

Imports System.Collections.Concurrent
Imports System.Runtime.InteropServices

Namespace Xu808

    ''' <summary>waveOut 封装：四缓冲环形队列，回调驱动。</summary>
    Public NotInheritable Class XuWaveOut
        Implements IDisposable

        Private Const WAVE_MAPPER As Integer = -1
        Private Const CALLBACK_FUNCTION As Integer = &H30000
        Private Const WOM_DONE As Integer = &H3BD
        Private Const WAVE_FORMAT_PCM As Integer = 1
        Private Const BufferCount As Integer = 4
        Private Const Frames As Integer = XuEngine.BlockSize

        <StructLayout(LayoutKind.Sequential)>
        Private Structure WAVEFORMATEX
            Public wFormatTag As UShort
            Public nChannels As UShort
            Public nSamplesPerSec As UInteger
            Public nAvgBytesPerSec As UInteger
            Public nBlockAlign As UShort
            Public wBitsPerSample As UShort
            Public cbSize As UShort
        End Structure

        <StructLayout(LayoutKind.Sequential)>
        Private Structure WAVEHDR
            Public lpData As IntPtr
            Public dwBufferLength As UInteger
            Public dwBytesRecorded As UInteger
            Public dwUser As IntPtr
            Public dwFlags As UInteger
            Public dwLoops As UInteger
            Public lpNext As IntPtr
            Public reserved As IntPtr
        End Structure

        ' 回调委托必须显式声明为 Winapi 调用约定；否则 x64 上碰巧对、x86 上会崩栈
        <UnmanagedFunctionPointer(CallingConvention.Winapi)>
        Private Delegate Sub WaveCallback(hwo As IntPtr, uMsg As UInteger, dwInstance As IntPtr,
                                         dwParam1 As IntPtr, dwParam2 As IntPtr)

        <DllImport("winmm.dll", CharSet:=CharSet.Auto)>
        Private Shared Function waveOutOpen(ByRef hwo As IntPtr, uDeviceID As Integer,
                                            ByRef pwfx As WAVEFORMATEX, dwCallback As WaveCallback,
                                            dwInstance As IntPtr, dwFlags As Integer) As Integer
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function waveOutPrepareHeader(hwo As IntPtr, pwh As IntPtr, cbwh As Integer) As Integer
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function waveOutUnprepareHeader(hwo As IntPtr, pwh As IntPtr, cbwh As Integer) As Integer
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function waveOutWrite(hwo As IntPtr, pwh As IntPtr, cbwh As Integer) As Integer
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function waveOutReset(hwo As IntPtr) As Integer
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function waveOutClose(hwo As IntPtr) As Integer
        End Function

        Private ReadOnly engine As XuEngine
        Private hwo As IntPtr = IntPtr.Zero
        Private fmt As WAVEFORMATEX
        Private ReadOnly data As Byte()()
        Private ReadOnly dataPin As GCHandle()
        Private ReadOnly hdrPtr As IntPtr()
        Private ReadOnly callback As WaveCallback      ' 必须持有引用，否则委托会被 GC 回收 → 回调野指针
        Private nextBuf As Integer
        Private running As Boolean
        Private closing As Boolean

        ' 预分配渲染缓冲，回调里绝不 New
        Private ReadOnly left As Single()
        Private ReadOnly right As Single()
        Private ReadOnly hdrSize As Integer

        Public Sub New(eng As XuEngine)
            engine = eng
            callback = New WaveCallback(AddressOf OnWaveOut)
            fmt = New WAVEFORMATEX With {
                .wFormatTag = CUShort(WAVE_FORMAT_PCM),
                .nChannels = 2US,
                .nSamplesPerSec = CUInt(XuConst.SampleRate),
                .wBitsPerSample = 16US,
                .nBlockAlign = 4US,                        ' 2 声道 × 2 字节
                .nAvgBytesPerSec = CUInt(XuConst.SampleRate) * 4UI,
                .cbSize = 0US
            }
            left = New Single(Frames - 1) {}
            right = New Single(Frames - 1) {}
            hdrSize = Marshal.SizeOf(Of WAVEHDR)()
            data = New Byte(BufferCount - 1)() {}
            dataPin = New GCHandle(BufferCount - 1) {}
            hdrPtr = New IntPtr(BufferCount - 1) {}

            Dim bytes As Integer = Frames * 4              ' 每块字节数：256 帧 × 2 声道 × int16
            For i As Integer = 0 To BufferCount - 1
                data(i) = New Byte(bytes - 1) {}
                dataPin(i) = GCHandle.Alloc(data(i), GCHandleType.Pinned)   ' 固定托管数组，指针才稳定
                hdrPtr(i) = Marshal.AllocHGlobal(hdrSize)
                Dim hdr As New WAVEHDR With {
                    .lpData = dataPin(i).AddrOfPinnedObject(),
                    .dwBufferLength = CUInt(bytes)
                }
                Marshal.StructureToPtr(hdr, hdrPtr(i), False)
            Next
        End Sub

        ''' <summary>打开设备并预填 4 个缓冲，随后声卡自己来要数据（拉模式）。</summary>
        Public Sub Start()
            If running Then Return
            Dim rc As Integer = waveOutOpen(hwo, WAVE_MAPPER, fmt, callback, IntPtr.Zero, CALLBACK_FUNCTION)
            If rc <> 0 Then Throw New InvalidOperationException($"waveOutOpen 失败，错误码 {rc}")
            For i As Integer = 0 To BufferCount - 1
                waveOutPrepareHeader(hwo, hdrPtr(i), hdrSize)
                FillBuffer(i)
                waveOutWrite(hwo, hdrPtr(i), hdrSize)
            Next
            nextBuf = 0
            running = True
        End Sub

        ''' <summary>把 engine 的最新一块渲染进指定缓冲（int16 小端交织）。</summary>
        Private Sub FillBuffer(idx As Integer)
            engine.RenderBlock(Frames, left, right)
            Dim buf As Byte() = data(idx)
            Dim p As Integer = 0
            For i As Integer = 0 To Frames - 1
                Dim fl As Single = Math.Clamp(left(i), -1.0F, 1.0F)
                Dim fr As Single = Math.Clamp(right(i), -1.0F, 1.0F)
                ' 截断而非四舍五入，与 C# 版一致
                Dim sl As Short = CShort(Math.Truncate(CDbl(fl) * 32767.0))
                Dim sr As Short = CShort(Math.Truncate(CDbl(fr) * 32767.0))
                buf(p) = CByte(sl And &HFF) : p += 1
                buf(p) = CByte((sl >> 8) And &HFF) : p += 1
                buf(p) = CByte(sr And &HFF) : p += 1
                buf(p) = CByte((sr >> 8) And &HFF) : p += 1
            Next
        End Sub

        ''' <summary>waveOut 回调：只做“补一块、再写回去”。</summary>
        Private Sub OnWaveOut(h As IntPtr, msg As UInteger, inst As IntPtr, p1 As IntPtr, p2 As IntPtr)
            If closing OrElse msg <> CUInt(WOM_DONE) Then Return
            Try
                Dim idx As Integer = nextBuf
                nextBuf = (nextBuf + 1) Mod BufferCount
                FillBuffer(idx)
                waveOutWrite(hwo, hdrPtr(idx), hdrSize)
            Catch ex As Exception
                ' 音频回调绝对不能把异常抛回系统线程：这会让整个进程直接消失
                engine.Xruns += 1
            End Try
        End Sub

        Public Sub [Stop]()
            If Not running Then Return
            closing = True
            Try
                waveOutReset(hwo)
            Catch
            End Try
            Try
                waveOutClose(hwo)
            Catch
            End Try
            hwo = IntPtr.Zero
            running = False
        End Sub

        Public Sub Dispose() Implements IDisposable.Dispose
            [Stop]()
            For i As Integer = 0 To BufferCount - 1
                If hdrPtr(i) <> IntPtr.Zero Then
                    Try
                        waveOutUnprepareHeader(hwo, hdrPtr(i), hdrSize)
                    Catch
                    End Try
                    Marshal.FreeHGlobal(hdrPtr(i))
                    hdrPtr(i) = IntPtr.Zero
                End If
                If dataPin(i).IsAllocated Then dataPin(i).Free()
            Next
        End Sub
    End Class

    ''' <summary>
    ''' MIDI 输入：winmm midiInOpen + 回调入队。
    ''' 解析统一放在 UI 线程（定时器里出队），音频线程只消费 engine 的无锁队列，
    ''' 这样既不会在 MIDI 回调里碰引擎，也不会让音频线程等锁。
    ''' </summary>
    Public NotInheritable Class XuMidiIn
        Implements IDisposable

        Private Const MIM_DATA As Integer = &H3C3
        Private Const CALLBACK_FUNCTION As Integer = &H30000

        <UnmanagedFunctionPointer(CallingConvention.Winapi)>
        Private Delegate Sub MidiCallback(h As IntPtr, msg As UInteger, inst As IntPtr,
                                          p1 As IntPtr, p2 As IntPtr)

        <DllImport("winmm.dll")>
        Private Shared Function midiInGetNumDevs() As UInteger
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function midiInOpen(ByRef h As IntPtr, device As UInteger, cb As MidiCallback,
                                           inst As IntPtr, flags As Integer) As UInteger
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function midiInStart(h As IntPtr) As UInteger
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function midiInStop(h As IntPtr) As UInteger
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function midiInReset(h As IntPtr) As UInteger
        End Function

        <DllImport("winmm.dll")>
        Private Shared Function midiInClose(h As IntPtr) As UInteger
        End Function

        Private handle As IntPtr = IntPtr.Zero
        Private ReadOnly callback As MidiCallback      ' 保引用，理由同上

        ' status / data1 / data2 三元组；UI 定时器每 33ms 排空一次
        Public ReadOnly Queue As New ConcurrentQueue(Of MidiMessage)

        Private _ok As Boolean = False
        Private _lastError As String = ""

        ''' <summary>是否成功打开了某个 MIDI 输入设备。</summary>
        Public ReadOnly Property Ok As Boolean
            Get
                Return _ok
            End Get
        End Property

        ''' <summary>失败原因（供界面显示）；成功时为空串。</summary>
        Public ReadOnly Property LastError As String
            Get
                Return _lastError
            End Get
        End Property

        Public Sub New()
            callback = New MidiCallback(AddressOf OnMidi)
            Dim n As UInteger = midiInGetNumDevs()
            If n = 0 Then
                _lastError = "没有 MIDI 输入设备"
                Return
            End If
            Dim rc As UInteger = midiInOpen(handle, 0, callback, IntPtr.Zero, CALLBACK_FUNCTION)
            If rc <> 0 Then
                _lastError = $"midiInOpen 失败 {rc}"
                Return
            End If
            rc = midiInStart(handle)
            If rc <> 0 Then
                _lastError = $"midiInStart 失败 {rc}"
                midiInClose(handle)
                handle = IntPtr.Zero
                Return
            End If
            _ok = True
            _lastError = ""
        End Sub

        Private Sub OnMidi(h As IntPtr, msg As UInteger, inst As IntPtr, p1 As IntPtr, p2 As IntPtr)
            If msg <> CUInt(MIM_DATA) Then Return
            Dim v As Long = p1.ToInt64()
            Queue.Enqueue(New MidiMessage(CInt(v And &HFF), CInt((v >> 8) And &HFF), CInt((v >> 16) And &HFF)))
        End Sub

        Public Sub Dispose() Implements IDisposable.Dispose
            If handle <> IntPtr.Zero Then
                Try
                    midiInStop(handle)
                    midiInReset(handle)
                    midiInClose(handle)
                Catch
                End Try
                handle = IntPtr.Zero
            End If
        End Sub
    End Class

    ''' <summary>一条 MIDI 短消息（status / data1 / data2）。</summary>
    Public Structure MidiMessage
        Public Status As Integer
        Public Data1 As Integer
        Public Data2 As Integer

        Public Sub New(status As Integer, d1 As Integer, d2 As Integer)
            Me.Status = status
            Me.Data1 = d1
            Me.Data2 = d2
        End Sub
    End Structure

End Namespace
