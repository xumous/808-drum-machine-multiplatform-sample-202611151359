// ============================================================================
//  Xu808 Engine.cs —— 音序器、混音、主效果、离线导出（C# 版）
//
//  设计要点（与 Python 版逐条对应）：
//    · samplePos 是唯一权威时钟，类型 long，48k 下可跑 600 万年不溢出；
//    · 所有步进/微时/ratchet 都换算成“绝对样本时刻”，先入队再按块切段；
//    · 音频回调路径不 new、不 lock、不 IO、不抛异常；
//    · 停止时给每个 voice 10ms 淡出，既即时安静又不“咔哒”。
// ============================================================================

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Xu808
{
    /// <summary>内部事件：什么时候、敲哪个声部、多大力、是否重音。</summary>
    public struct XuEvent
    {
        public long T;
        public int Pad;
        public double Vel;
        public bool Accent;
    }

    public sealed class XuEngine
    {
        public const int BlockSize = 256;   // 256 @48k = 5.3ms，C# 版延迟更低

        public XuProject Project { get; set; }
        private readonly XuVoice[][] voices = new XuVoice[16][];
        private readonly XuBiquad lpL = new XuBiquad();
        private readonly XuBiquad lpR = new XuBiquad();
        private readonly XuBiquad hpL = new XuBiquad();
        private readonly XuBiquad hpR = new XuBiquad();

        // ---- 传输 ----
        public volatile bool Playing;
        public long SamplePos { get; private set; }
        private long nextStepAbs;
        private int stepIndex;
        private long globalStep;
        private int cycleCount;
        private bool fillNow;
        public volatile bool ClockExternal;
        private int clockPulse;

        // ---- 队列（预分配，回调内只 Clear/Add）----
        private readonly List<XuEvent> pending = new List<XuEvent>(1024);
        private readonly List<XuEvent> manual = new List<XuEvent>(64);
        private readonly List<XuEvent> events = new List<XuEvent>(1024);
        private uint rng = 0x808808;

        // ---- Pattern / Song ----
        private int pendingPattern = -1;
        private int songIndex;
        private int songRepeat;

        // ---- 统计 ----
        public volatile float MeterL, MeterR, Peak;
        public volatile int Xruns;
        public volatile float Cpu;

        // ---- 离线分轨：>=0 时只让该轨出声 ----
        public int SoloOverride = -1;

        // 供 UI 显示
        public int StepIndex => stepIndex;
        public int PatternIndex => Project.CurrentPattern;
        public int CycleCount => cycleCount;

        public XuEngine(XuProject project)
        {
            Project = project;
            for (int p = 0; p < 16; p++)
            {
                voices[p] = new XuVoice[XuConst.MaxVoices];
                for (int v = 0; v < XuConst.MaxVoices; v++)
                    voices[p][v] = new XuVoice(p, 0x808 + p);
            }
        }

        // ==================================================================
        // 传输
        // ==================================================================
        public void Play()
        {
            if (Playing) return;
            Playing = true;
            stepIndex = 0;
            globalStep = 0;
            cycleCount = 0;
            fillNow = false;
            songIndex = 0;
            songRepeat = 0;
            nextStepAbs = SamplePos;
            pending.Clear();
        }

        public void Stop()
        {
            Playing = false;
            long now = SamplePos;
            for (int p = 0; p < 16; p++)
                for (int v = 0; v < 4; v++)
                    voices[p][v].Kill(now, 480);   // 10ms 淡出
            pending.Clear();
        }

        public void Toggle()
        {
            if (Playing) Stop(); else Play();
        }

        // ==================================================================
        // 时钟与调度
        // ==================================================================
        private int StepLen()
        {
            double bpm = Math.Clamp(Project.Master.Bpm, 40f, 300f);
            return Math.Max(64, (int)Math.Round(XuConst.SampleRate * 60.0 / bpm / 4.0));
        }

        /// <summary>32 位 xorshift：确定性、无分配，用于概率触发。</summary>
        private uint NextRand()
        {
            uint x = rng;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            rng = x;
            return x;
        }

        private bool CondOk(int cond)
        {
            return cond switch
            {
                XuCond.Always => true,
                XuCond.Fill => fillNow,
                XuCond.NotFill => !fillNow,
                XuCond.Every2 => cycleCount % 2 == 0,
                XuCond.Every4 => cycleCount % 4 == 0,
                _ => true
            };
        }

        /// <summary>把一个 16 分步展开成绝对样本事件。</summary>
        private void EmitStep(long stepAbs)
        {
            var pat = Project.Current;
            int stepLen = StepLen();
            int swingOff = 0;
            if ((stepIndex & 1) == 1)
                swingOff = (int)(stepLen * Project.Master.Swing * 0.5);
            long baseT = stepAbs + swingOff;

            for (int pad = 0; pad < 16; pad++)
            {
                int tl = Math.Clamp(pat.TrackLen[pad], 1, 64);
                int idx = (int)(globalStep % tl);
                var st = pat.Data[pad][idx];
                if (!st.On) continue;
                if (!CondOk(st.Cond)) continue;
                if (st.Prob <= 0) continue;
                if (st.Prob < 100 && (NextRand() % 100) >= st.Prob) continue;

                double vel = Math.Clamp(st.Velocity / 127.0, 0.02, 1.0);
                if (st.Accent) vel = Math.Min(1.25, vel * 1.30);
                int r = Math.Clamp(st.Ratchet, 1, 8);
                for (int i = 0; i < r; i++)
                {
                    long t = baseT + st.Micro + (long)(i * (double)stepLen / r);
                    var ev = new XuEvent
                    {
                        T = t,
                        Pad = pad,
                        Vel = vel * Math.Pow(0.92, i),
                        Accent = st.Accent
                    };
                    pending.Add(ev);
                }
            }
        }

        /// <summary>一步结束后推进；切 pattern / song 只允许发生在循环边界。</summary>
        private void AdvanceAfterStep()
        {
            var pat = Project.Current;
            stepIndex++;
            globalStep++;
            if (stepIndex < Math.Max(1, pat.Steps)) return;

            stepIndex = 0;
            cycleCount++;
            fillNow = (cycleCount % 4) == 3;

            if (pendingPattern >= 0)
            {
                Project.CurrentPattern = Math.Clamp(pendingPattern, 0, Project.Patterns.Count - 1);
                pendingPattern = -1;
                songRepeat = 0;
                return;
            }
            if (Project.Master.SongMode && Project.Song.Entries.Count > 0)
            {
                var entry = Project.Song.Entries[songIndex % Project.Song.Entries.Count];
                songRepeat++;
                if (songRepeat >= Math.Max(1, entry.Repeats))
                {
                    songRepeat = 0;
                    songIndex = (songIndex + 1) % Project.Song.Entries.Count;
                    Project.CurrentPattern = Math.Clamp(Project.Song.Entries[songIndex].Pattern,
                                                        0, Project.Patterns.Count - 1);
                }
            }
        }

        /// <summary>收集本块事件：手点 + 音序 + 之前挂起的子事件。</summary>
        private void CollectEvents(long b0, long b1)
        {
            events.Clear();
            // 手点事件：从 UI 线程来的。这里用“读指针/写指针”原地过滤，
            // 既不会边遍历边删除，也不会创建临时列表。
            int keep = 0;
            for (int i = 0; i < manual.Count; i++)
            {
                var ev = manual[i];
                if (ev.T < b1)
                {
                    if (ev.T < b0) ev.T = b0;
                    events.Add(ev);
                }
                else
                {
                    manual[keep++] = ev;   // 极少见：留到下一块，绝不能提前触发
                }
            }
            manual.RemoveRange(keep, manual.Count - keep);

            if (Playing && !ClockExternal)
            {
                int guard = 0;
                while (nextStepAbs < b1 && guard++ < 64)
                {
                    EmitStep(Math.Max(b0, nextStepAbs));
                    nextStepAbs += StepLen();
                    AdvanceAfterStep();
                }
            }
            // pending 同理：只取本块的，未来的必须保留
            keep = 0;
            for (int i = 0; i < pending.Count; i++)
            {
                var ev = pending[i];
                if (ev.T < b1)
                {
                    if (ev.T < b0) ev.T = b0;
                    events.Add(ev);
                }
                else
                {
                    pending[keep++] = ev;
                }
            }
            pending.RemoveRange(keep, pending.Count - keep);
            // 排序：事件数量很少（最多几十个），插入排序比 Sort 更省
            for (int i = 1; i < events.Count; i++)
            {
                var key = events[i];
                int j = i - 1;
                while (j >= 0 && events[j].T > key.T)
                {
                    events[j + 1] = events[j];
                    j--;
                }
                events[j + 1] = key;
            }
        }

        public void ExternalClockTick()
        {
            if (!Playing) return;
            clockPulse++;
            if (clockPulse < 6) return;
            clockPulse = 0;
            EmitStep(SamplePos);
            AdvanceAfterStep();
            nextStepAbs = SamplePos + StepLen();
        }

        // ==================================================================
        // 发声
        // ==================================================================
        private bool Audible(int pad)
        {
            if (SoloOverride >= 0) return pad == SoloOverride;
            bool anySolo = false;
            for (int i = 0; i < 16; i++) if (Project.Pads[i].Solo) { anySolo = true; break; }
            var p = Project.Pads[pad];
            if (p.Mute) return false;
            if (anySolo && !p.Solo) return false;
            return true;
        }

        private void Trigger(int pad, long when, double vel, bool accent)
        {
            if (pad == 15) foreach (var v in voices[14]) v.Kill(when, 16);  // 闭镲掐开镲
            if (pad == 14) foreach (var v in voices[14]) v.Kill(when, 16);  // 开镲重触发
            XuVoice target = null;
            for (int i = 0; i < 4; i++)
                if (!voices[pad][i].Active) { target = voices[pad][i]; break; }
            if (target == null)
            {
                target = voices[pad][0];
                for (int i = 1; i < 4; i++)
                    if (voices[pad][i].Start < target.Start) target = voices[pad][i];
                target.Kill(when, 64);
            }
            target.Trigger(when, vel, accent);
        }

        // ==================================================================
        // 主渲染
        // ==================================================================
        /// <summary>
        /// 渲染 frames 个立体声样本到 l/r（长度必须 >= frames）。
        /// 这个函数被实时回调和离线导出共同使用——保证“导出不变样”。
        /// </summary>
        public void RenderBlock(int frames, float[] l, float[] r)
        {
            long b0 = SamplePos;
            long b1 = b0 + frames;
            Array.Clear(l, 0, frames);
            Array.Clear(r, 0, frames);

            CollectEvents(b0, b1);

            long cur = b0;
            for (int i = 0; i < events.Count; i++)
            {
                var ev = events[i];
                if (ev.T > cur)
                {
                    RenderVoices(l, r, b0, cur, ev.T);
                    cur = ev.T;
                }
                if (ev.T < b1) Trigger(ev.Pad, ev.T, ev.Vel, ev.Accent);
            }
            if (cur < b1) RenderVoices(l, r, b0, cur, b1);

            // ---- 主效果链 ----
            var m = Project.Master;
            if (m.HpOn)
            {
                hpL.SetHighPass(m.HpCutoff, m.HpQ);
                hpR.SetHighPass(m.HpCutoff, m.HpQ);
                for (int i = 0; i < frames; i++)
                {
                    l[i] = (float)hpL.Process(l[i]);
                    r[i] = (float)hpR.Process(r[i]);
                }
            }
            if (m.LpOn)
            {
                lpL.SetLowPass(m.LpCutoff, m.LpQ);
                lpR.SetLowPass(m.LpCutoff, m.LpQ);
                for (int i = 0; i < frames; i++)
                {
                    l[i] = (float)lpL.Process(l[i]);
                    r[i] = (float)lpR.Process(r[i]);
                }
            }
            float drive = Math.Clamp(m.Drive, 1f, 8f);
            float norm = (float)Math.Tanh(drive);
            float vol = Math.Clamp(m.Volume, 0f, 1f);
            float pl = 0, pr = 0;
            for (int i = 0; i < frames; i++)
            {
                float xl = l[i], xr = r[i];
                if (drive > 1.001f)
                {
                    xl = (float)Math.Tanh(xl * drive) / norm;
                    xr = (float)Math.Tanh(xr * drive) / norm;
                }
                xl = xl / (1f + Math.Abs(xl));   // 软限幅
                xr = xr / (1f + Math.Abs(xr));
                xl *= vol; xr *= vol;
                l[i] = xl; r[i] = xr;
                if (Math.Abs(xl) > pl) pl = Math.Abs(xl);
                if (Math.Abs(xr) > pr) pr = Math.Abs(xr);
            }
            MeterL = Math.Max(pl, MeterL * 0.75f);
            MeterR = Math.Max(pr, MeterR * 0.75f);
            Peak = Math.Max(MeterL, MeterR);

            SamplePos = b1;
            // 回收过期 voice
            for (int p = 0; p < 16; p++)
                for (int v = 0; v < 4; v++)
                {
                    var vo = voices[p][v];
                    if (vo.Active && b0 - vo.Start > 4L * XuConst.SampleRate) vo.Active = false;
                }
        }

        private void RenderVoices(float[] l, float[] r, long b0, long a, long b)
        {
            if (b <= a) return;
            // 注意：offset 传 0，让 voice 自己用 (a-b0) 定位；
            // 若这里再传 (a-b0)，就会双重偏移，声音会整体后移一段，徐某踩过这个坑。
            for (int pad = 0; pad < 16; pad++)
            {
                if (!Audible(pad)) continue;
                var p = Project.Pads[pad];
                for (int v = 0; v < 4; v++)
                {
                    var vo = voices[pad][v];
                    if (vo.Active) vo.Render(l, r, 0, (int)(b - a), b0, b, p);
                }
            }
        }

        // ==================================================================
        // GUI 接口
        // ==================================================================
        public void TriggerPad(int pad, int velocity, bool accent)
        {
            manual.Add(new XuEvent
            {
                T = SamplePos + 32,
                Pad = pad,
                Vel = Math.Max(0.02, velocity / 127.0),
                Accent = accent
            });
        }

        public void RequestPattern(int index)
        {
            if (Playing) pendingPattern = index;
            else Project.CurrentPattern = Math.Clamp(index, 0, Project.Patterns.Count - 1);
        }

        // ==================================================================
        // 离线渲染 / 导出
        // ==================================================================
        private float[] RenderSeconds(double seconds, int soloPad = -1)
        {
            int total = (int)(seconds * XuConst.SampleRate);
            var l = new float[total];
            var r = new float[total];
            var bufL = new float[BlockSize];
            var bufR = new float[BlockSize];
            SoloOverride = soloPad;
            Play();
            int done = 0;
            while (done < total)
            {
                int n = Math.Min(BlockSize, total - done);
                RenderBlock(n, bufL, bufR);
                Array.Copy(bufL, 0, l, done, n);
                Array.Copy(bufR, 0, r, done, n);
                done += n;
            }
            // 交织成立体声
            var inter = new float[total * 2];
            for (int i = 0; i < total; i++) { inter[i * 2] = l[i]; inter[i * 2 + 1] = r[i]; }
            return inter;
        }

        public float[] RenderPattern(int repeats = 1)
        {
            var copy = Project.Clone();
            var eng = new XuEngine(copy);
            double sec = (copy.Current.Steps * Math.Max(1, repeats) * eng.StepLen()) / (double)XuConst.SampleRate + 3.0;
            return eng.RenderSeconds(sec);
        }

        public float[] RenderSong()
        {
            var copy = Project.Clone();
            var eng = new XuEngine(copy);
            int steps = 0;
            if (copy.Song.Entries.Count > 0)
                foreach (var e in copy.Song.Entries)
                {
                    int idx = Math.Clamp(e.Pattern, 0, copy.Patterns.Count - 1);
                    steps += copy.Patterns[idx].Steps * Math.Max(1, e.Repeats);
                }
            if (steps == 0) steps = copy.Current.Steps;
            double sec = steps * eng.StepLen() / (double)XuConst.SampleRate + 3.0;
            return eng.RenderSeconds(sec);
        }

        /// <summary>写 16bit PCM WAV（立体声交织 float 输入）。</summary>
        public static void WriteWav(string path, float[] interleaved)
        {
            int frames = interleaved.Length / 2;
            using var fs = new FileStream(path, FileMode.Create, FileAccess.Write);
            using var bw = new BinaryWriter(fs);
            int dataBytes = frames * 4;   // 2 声道 × 2 字节
            bw.Write(new[] { (byte)'R', (byte)'I', (byte)'F', (byte)'F' });
            bw.Write(36 + dataBytes);
            bw.Write(new[] { (byte)'W', (byte)'A', (byte)'V', (byte)'E' });
            bw.Write(new[] { (byte)'f', (byte)'m', (byte)'t', (byte)' ' });
            bw.Write(16);                 // fmt 块长度
            bw.Write((short)1);           // PCM
            bw.Write((short)2);           // 声道
            bw.Write(XuConst.SampleRate);
            bw.Write(XuConst.SampleRate * 4);  // 字节率
            bw.Write((short)4);           // 块对齐
            bw.Write((short)16);          // 位深
            bw.Write(new[] { (byte)'d', (byte)'a', (byte)'t', (byte)'a' });
            bw.Write(dataBytes);
            for (int i = 0; i < interleaved.Length; i++)
            {
                float x = Math.Clamp(interleaved[i], -1f, 1f);
                bw.Write((short)(x * 32767f));
            }
        }

        public void ExportWav(string path, bool song)
        {
            WriteWav(path, song ? RenderSong() : RenderPattern());
        }

        public List<string> ExportStems(string folder, string prefix = "xu808")
        {
            var files = new List<string>();
            for (int pad = 0; pad < 16; pad++)
            {
                // 每轨都用全新引擎：滤波器/限幅器状态不能从上一轨带过来
                var copy = Project.Clone();
                var eng = new XuEngine(copy);
                double sec = copy.Current.Steps * eng.StepLen() / (double)XuConst.SampleRate + 2.5;
                var audio = eng.RenderSeconds(sec, pad);
                string path = Path.Combine(folder, $"{prefix}_stem_{pad:00}.wav");
                WriteWav(path, audio);
                files.Add(path);
            }
            return files;
        }

        // ==================================================================
        // MIDI 导出
        // ==================================================================
        private static void WriteVlq(List<byte> buf, int value)
        {
            value = Math.Max(0, value);
            var tmp = new List<byte> { (byte)(value & 0x7F) };
            value >>= 7;
            while (value > 0)
            {
                tmp.Insert(0, (byte)((value & 0x7F) | 0x80));
                value >>= 7;
            }
            buf.AddRange(tmp);
        }

        /// <summary>导出 SMF Format 1，PPQ=480，16 分=120 tick。</summary>
        public void ExportMidi(string path, bool song)
        {
            var p = Project;
            int ppq = 480;
            double bpm = Math.Clamp(p.Master.Bpm, 40f, 300f);
            double stepSamples = XuConst.SampleRate * 60.0 / bpm / 4.0;
            var seq = new List<int>();
            if (song && p.Song.Entries.Count > 0)
                foreach (var e in p.Song.Entries)
                    for (int k = 0; k < Math.Max(1, e.Repeats); k++)
                        seq.Add(Math.Clamp(e.Pattern, 0, p.Patterns.Count - 1));
            else seq.Add(p.CurrentPattern);

            var notes = new List<(int tick, int note, int vel)>();
            long cursor = 0;
            foreach (int pi in seq)
            {
                var pat = p.Patterns[pi];
                for (int s = 0; s < pat.Steps; s++)
                {
                    int swingTick = (s % 2 == 1) ? (int)(stepSamples * p.Master.Swing * 0.5 / stepSamples * 120) : 0;
                    for (int pad = 0; pad < 16; pad++)
                    {
                        int tl = Math.Clamp(pat.TrackLen[pad], 1, 64);
                        int idx = (int)((cursor + s) % tl);
                        var st = pat.Data[pad][idx];
                        if (!st.On || st.Prob <= 0) continue;
                        int vel = Math.Clamp((int)(st.Velocity * (st.Accent ? 1.3f : 1f)), 1, 127);
                        int baseTick = (int)((cursor + s) * 120);
                        int microTick = (int)Math.Round(st.Micro / stepSamples * 120.0);
                        int r = Math.Clamp(st.Ratchet, 1, 8);
                        for (int i = 0; i < r; i++)
                        {
                            int tick = Math.Max(0, baseTick + swingTick + microTick + (int)(i * 120.0 / r));
                            notes.Add((tick, XuConst.MidiNotes[pad], vel));
                        }
                    }
                }
                cursor += pat.Steps;
            }
            notes.Sort((a, b) => a.tick != b.tick ? a.tick.CompareTo(b.tick) : a.note.CompareTo(b.note));

            var track = new List<byte>();
            byte[] name = Encoding.ASCII.GetBytes("XU-808 by XuMou");
            WriteVlq(track, 0); track.Add(0xFF); track.Add(0x03); WriteVlq(track, name.Length); track.AddRange(name);
            int tempo = (int)(60000000.0 / bpm);
            WriteVlq(track, 0); track.Add(0xFF); track.Add(0x51); track.Add(0x03);
            track.Add((byte)(tempo >> 16)); track.Add((byte)(tempo >> 8)); track.Add((byte)tempo);
            WriteVlq(track, 0); track.Add(0xFF); track.Add(0x58); track.Add(0x04);
            track.Add((byte)p.Master.BeatsPerBar);
            track.Add((byte)Math.Log2(Math.Max(1, p.Master.BeatUnit)));
            track.Add(24); track.Add(8);
            int last = 0;
            foreach (var (tick, note, vel) in notes)
            {
                WriteVlq(track, Math.Max(0, tick - last)); track.Add(0x99); track.Add((byte)note); track.Add((byte)vel);
                WriteVlq(track, 10); track.Add(0x89); track.Add((byte)note); track.Add(64);
                last = tick + 10;
            }
            WriteVlq(track, 0); track.Add(0xFF); track.Add(0x2F); track.Add(0x00);

            using var fs = new FileStream(path, FileMode.Create, FileAccess.Write);
            using var bw = new BinaryWriter(fs);
            bw.Write(new[] { (byte)'M', (byte)'T', (byte)'h', (byte)'d' });
            bw.Write(6); bw.Write((short)1); bw.Write((short)1); bw.Write((short)ppq);
            bw.Write(new[] { (byte)'M', (byte)'T', (byte)'r', (byte)'k' });
            bw.Write(track.Count); bw.Write(track.ToArray());
        }
    }
}
