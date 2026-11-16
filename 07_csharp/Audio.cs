// ============================================================================
//  Xu808 Audio.cs —— winmm waveOut 输出与 MIDI 输入
//
//  徐某为什么不用 NAudio：这是“零 NuGet”的硬性要求。
//  winmm.dll 从 Windows 3.1 就在系统里，永远不需要额外安装。
//
//  延迟：4 个 256 样本缓冲 = 4 × 5.33ms ≈ 21ms。
//  回调里只做“渲染 + 转换 + waveOutWrite”，不分配、不加锁、不弹窗。
// ============================================================================

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Xu808
{
    /// <summary>waveOut 封装：四缓冲环形队列，回调驱动。</summary>
    public sealed class XuWaveOut : IDisposable
    {
        private const int WAVE_MAPPER = -1;
        private const int CALLBACK_FUNCTION = 0x00030000;
        private const int WOM_DONE = 0x3BD;
        private const int WAVE_FORMAT_PCM = 1;
        private const int BufferCount = 4;
        private const int Frames = XuEngine.BlockSize;

        [StructLayout(LayoutKind.Sequential)]
        private struct WAVEFORMATEX
        {
            public ushort wFormatTag;
            public ushort nChannels;
            public uint nSamplesPerSec;
            public uint nAvgBytesPerSec;
            public ushort nBlockAlign;
            public ushort wBitsPerSample;
            public ushort cbSize;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct WAVEHDR
        {
            public IntPtr lpData;
            public uint dwBufferLength;
            public uint dwBytesRecorded;
            public IntPtr dwUser;
            public uint dwFlags;
            public uint dwLoops;
            public IntPtr lpNext;
            public IntPtr reserved;
        }

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate void WaveCallback(IntPtr hwo, uint uMsg, IntPtr dwInstance,
                                           IntPtr dwParam1, IntPtr dwParam2);

        [DllImport("winmm.dll", CharSet = CharSet.Auto)]
        private static extern int waveOutOpen(out IntPtr hwo, int uDeviceID,
                                              ref WAVEFORMATEX pwfx, WaveCallback dwCallback,
                                              IntPtr dwInstance, int dwFlags);
        [DllImport("winmm.dll")]
        private static extern int waveOutPrepareHeader(IntPtr hwo, IntPtr pwh, int cbwh);
        [DllImport("winmm.dll")]
        private static extern int waveOutUnprepareHeader(IntPtr hwo, IntPtr pwh, int cbwh);
        [DllImport("winmm.dll")]
        private static extern int waveOutWrite(IntPtr hwo, IntPtr pwh, int cbwh);
        [DllImport("winmm.dll")]
        private static extern int waveOutReset(IntPtr hwo);
        [DllImport("winmm.dll")]
        private static extern int waveOutClose(IntPtr hwo);

        private readonly XuEngine engine;
        private IntPtr hwo = IntPtr.Zero;
        private WAVEFORMATEX fmt;
        private readonly byte[][] data = new byte[BufferCount][];
        private readonly GCHandle[] dataPin = new GCHandle[BufferCount];
        private readonly IntPtr[] hdrPtr = new IntPtr[BufferCount];
        private readonly WaveCallback callback;   // 必须持有引用，否则委托会被 GC 回收
        private int nextBuf;
        private bool running;
        private bool closing;

        // 预分配渲染缓冲，回调里绝不 new
        private readonly float[] left = new float[Frames];
        private readonly float[] right = new float[Frames];

        public XuWaveOut(XuEngine eng)
        {
            engine = eng;
            callback = OnWaveOut;
            fmt = new WAVEFORMATEX
            {
                wFormatTag = WAVE_FORMAT_PCM,
                nChannels = 2,
                nSamplesPerSec = XuConst.SampleRate,
                wBitsPerSample = 16,
                nBlockAlign = 4,
                nAvgBytesPerSec = XuConst.SampleRate * 4,
                cbSize = 0
            };
            int bytes = Frames * 4;   // 2 声道 × int16
            for (int i = 0; i < BufferCount; i++)
            {
                data[i] = new byte[bytes];
                dataPin[i] = GCHandle.Alloc(data[i], GCHandleType.Pinned);
                hdrPtr[i] = Marshal.AllocHGlobal(Marshal.SizeOf<WAVEHDR>());
                var hdr = new WAVEHDR
                {
                    lpData = dataPin[i].AddrOfPinnedObject(),
                    dwBufferLength = (uint)bytes
                };
                Marshal.StructureToPtr(hdr, hdrPtr[i], false);
            }
        }

        /// <summary>打开设备并预填 4 个缓冲，随后声卡会自己来要数据。</summary>
        public void Start()
        {
            if (running) return;
            int rc = waveOutOpen(out hwo, WAVE_MAPPER, ref fmt, callback, IntPtr.Zero, CALLBACK_FUNCTION);
            if (rc != 0) throw new InvalidOperationException($"waveOutOpen 失败，错误码 {rc}");
            for (int i = 0; i < BufferCount; i++)
            {
                waveOutPrepareHeader(hwo, hdrPtr[i], Marshal.SizeOf<WAVEHDR>());
                FillBuffer(i);
                waveOutWrite(hwo, hdrPtr[i], Marshal.SizeOf<WAVEHDR>());
            }
            nextBuf = 0;
            running = true;
        }

        /// <summary>把 engine 的最新一块渲染进指定缓冲（int16 小端）。</summary>
        private void FillBuffer(int idx)
        {
            engine.RenderBlock(Frames, left, right);
            byte[] buf = data[idx];
            int p = 0;
            for (int i = 0; i < Frames; i++)
            {
                short sl = (short)(Math.Clamp(left[i], -1f, 1f) * 32767f);
                short sr = (short)(Math.Clamp(right[i], -1f, 1f) * 32767f);
                buf[p++] = (byte)(sl & 0xFF);
                buf[p++] = (byte)((sl >> 8) & 0xFF);
                buf[p++] = (byte)(sr & 0xFF);
                buf[p++] = (byte)((sr >> 8) & 0xFF);
            }
        }

        /// <summary>waveOut 回调：只做“补一块、再写回去”。</summary>
        private void OnWaveOut(IntPtr h, uint msg, IntPtr inst, IntPtr p1, IntPtr p2)
        {
            if (closing || msg != WOM_DONE) return;
            try
            {
                int idx = nextBuf;
                nextBuf = (nextBuf + 1) % BufferCount;
                FillBuffer(idx);
                waveOutWrite(hwo, hdrPtr[idx], Marshal.SizeOf<WAVEHDR>());
            }
            catch
            {
                // 音频回调绝对不能抛异常：静默丢弃这一块，主线程会看到 xrun
                engine.Xruns++;
            }
        }

        public void Stop()
        {
            if (!running) return;
            closing = true;
            try { waveOutReset(hwo); } catch { }
            try { waveOutClose(hwo); } catch { }
            hwo = IntPtr.Zero;
            running = false;
        }

        public void Dispose()
        {
            Stop();
            for (int i = 0; i < BufferCount; i++)
            {
                if (hdrPtr[i] != IntPtr.Zero)
                {
                    try { waveOutUnprepareHeader(hwo, hdrPtr[i], Marshal.SizeOf<WAVEHDR>()); } catch { }
                    Marshal.FreeHGlobal(hdrPtr[i]);
                    hdrPtr[i] = IntPtr.Zero;
                }
                if (dataPin[i].IsAllocated) dataPin[i].Free();
            }
        }
    }

    /// <summary>
    /// MIDI 输入：winmm midiInOpen + 回调入队。
    /// 解析放在 UI 线程，音频线程只消费 engine 的 manual 队列。
    /// </summary>
    public sealed class XuMidiIn : IDisposable
    {
        private const int MIM_DATA = 0x3C3;
        private const int CALLBACK_FUNCTION = 0x00030000;

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate void MidiCallback(IntPtr h, uint msg, IntPtr inst, IntPtr p1, IntPtr p2);

        [DllImport("winmm.dll")]
        private static extern uint midiInGetNumDevs();
        [DllImport("winmm.dll")]
        private static extern uint midiInOpen(out IntPtr h, uint device, MidiCallback cb, IntPtr inst, int flags);
        [DllImport("winmm.dll")]
        private static extern uint midiInStart(IntPtr h);
        [DllImport("winmm.dll")]
        private static extern uint midiInStop(IntPtr h);
        [DllImport("winmm.dll")]
        private static extern uint midiInReset(IntPtr h);
        [DllImport("winmm.dll")]
        private static extern uint midiInClose(IntPtr h);

        private IntPtr handle = IntPtr.Zero;
        private readonly MidiCallback callback;    // 保引用
        public readonly ConcurrentQueue<(int status, int d1, int d2)> Queue = new();
        public bool Ok { get; private set; }
        public string Error { get; private set; } = "";

        public XuMidiIn()
        {
            callback = OnMidi;
            uint n = midiInGetNumDevs();
            if (n == 0) { Error = "没有 MIDI 输入设备"; return; }
            uint rc = midiInOpen(out handle, 0, callback, IntPtr.Zero, CALLBACK_FUNCTION);
            if (rc != 0) { Error = $"midiInOpen 失败 {rc}"; return; }
            rc = midiInStart(handle);
            if (rc != 0) { Error = $"midiInStart 失败 {rc}"; midiInClose(handle); return; }
            Ok = true;
        }

        private void OnMidi(IntPtr h, uint msg, IntPtr inst, IntPtr p1, IntPtr p2)
        {
            if (msg != MIM_DATA) return;
            long v = p1.ToInt64();
            Queue.Enqueue(((int)(v & 0xFF), (int)((v >> 8) & 0xFF), (int)((v >> 16) & 0xFF)));
        }

        public void Dispose()
        {
            if (handle != IntPtr.Zero)
            {
                try { midiInStop(handle); midiInReset(handle); midiInClose(handle); } catch { }
                handle = IntPtr.Zero;
            }
        }
    }
}
