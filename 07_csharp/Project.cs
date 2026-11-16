// ============================================================================
//  Xu808 Project.cs —— 工程数据模型与 JSON 存档（C# 版）
//
//  徐某在这里刻意让字段名与 Python/Java 版保持完全一致：
//    on / velocity / accent / prob / micro / ratchet / cond
//  这样同一个 .xu808 工程可以在不同语言版本之间互通，
//  也方便以后写一个“跨版本一致性测试”。
// ============================================================================

using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Xu808
{
    /// <summary>一个格子的全部属性（对应规格书第 4 节）。</summary>
    public sealed class XuStep
    {
        public bool On { get; set; }               // 是否启用
        public int Velocity { get; set; } = 100;   // 力度 1..127
        public bool Accent { get; set; }           // 重音
        public int Prob { get; set; } = 100;       // 概率 0..100
        public int Micro { get; set; }             // 微时偏移（样本）
        public int Ratchet { get; set; } = 1;      // 滚奏 1..8
        public int Cond { get; set; }              // 条件触发

        /// <summary>深拷贝：编辑对话框点“取消”时不能污染原数据。</summary>
        public XuStep Clone() => (XuStep)MemberwiseClone();
    }

    /// <summary>16 轨 × 最多 64 步的 Pattern。</summary>
    public sealed class XuPattern
    {
        public string Name { get; set; } = "PATTERN";
        public int Steps { get; set; } = 16;
        public bool Fill { get; set; }
        // 外层 16 条轨道，内层固定 64 步，实时线程可以无脑索引
        public List<List<XuStep>> Data { get; set; } = new();
        public int[] TrackLen { get; set; } = new int[16];

        public XuPattern()
        {
            for (int i = 0; i < 16; i++)
            {
                var row = new List<XuStep>(64);
                for (int s = 0; s < 64; s++) row.Add(new XuStep());
                Data.Add(row);
                TrackLen[i] = 16;
            }
        }

        public XuStep Cell(int pad, int step) => Data[pad][step];

        /// <summary>写入默认 808 律动；与 Python 版逐格一致。</summary>
        public void SetDemoBeat()
        {
            for (int p = 0; p < 16; p++)
                for (int s = 0; s < 64; s++)
                    Data[p][s] = new XuStep();
            Steps = 16;
            Set(0, 0, 118, true); Set(0, 8, 105, false); Set(0, 14, 90, false);
            Set(1, 4, 110, false); Set(1, 12, 110, true);
            for (int s = 0; s < 16; s += 2) Set(15, s, s % 4 == 0 ? 96 : 84, false);
            Set(14, 14, 92, false);
            Set(10, 12, 80, false);
            for (int s = 1; s < 16; s += 2) Set(11, s, 42, false);
            Set(0, 15, 88, false);
            Data[0][15].Ratchet = 3;
            Name = "PATTERN 1";
        }

        private void Set(int pad, int step, int vel, bool accent)
        {
            var st = Data[pad][step];
            st.On = true; st.Velocity = vel; st.Accent = accent;
        }
    }

    /// <summary>主输出与全局设置。</summary>
    public sealed class XuMaster
    {
        public float Volume { get; set; } = 0.80f;
        public float Drive { get; set; } = 1.35f;
        public float LpCutoff { get; set; } = 18000f;
        public float LpQ { get; set; } = 0.90f;
        public float HpCutoff { get; set; } = 28f;
        public float HpQ { get; set; } = 0.70f;
        public bool LpOn { get; set; } = true;
        public bool HpOn { get; set; } = true;
        public float Bpm { get; set; } = 120f;
        public float Swing { get; set; }          // 0..0.75
        public int BeatsPerBar { get; set; } = 4;
        public int BeatUnit { get; set; } = 4;
        public bool SongMode { get; set; }
    }

    public sealed class XuSongEntry
    {
        public int Pattern { get; set; }
        public int Repeats { get; set; } = 1;
    }

    public sealed class XuSong
    {
        public bool Enabled { get; set; }
        public List<XuSongEntry> Entries { get; set; } = new();
    }

    /// <summary>顶层工程对象。</summary>
    public sealed class XuProject
    {
        public string Format { get; set; } = "XU808";
        public int Version { get; set; } = 1;
        public string Name { get; set; } = "徐某的 808 工程";
        public int Seed { get; set; } = 0x808808;
        public int CurrentPattern { get; set; }
        public XuMaster Master { get; set; } = new();
        public XuSong Song { get; set; } = new();
        public List<XuPadParams> Pads { get; set; } = new();
        public List<XuPattern> Patterns { get; set; } = new();

        public XuProject()
        {
            for (int i = 0; i < 16; i++) Pads.Add(new XuPadParams());
            for (int i = 0; i < 16; i++)
            {
                var p = new XuPattern { Name = $"PATTERN {i + 1}" };
                if (i == 0) p.SetDemoBeat();
                Patterns.Add(p);
            }
        }

        public XuPattern Current => Patterns[Math.Clamp(CurrentPattern, 0, Patterns.Count - 1)];

        /// <summary>保存为 UTF-8 JSON；WriteIndented 让用户可以直接改文件。</summary>
        public void Save(string path)
        {
            var opt = new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
            File.WriteAllText(path, JsonSerializer.Serialize(this, opt), System.Text.Encoding.UTF8);
        }

        /// <summary>加载工程；字段缺失时用构造函数默认值兜底。</summary>
        public static XuProject Load(string path)
        {
            var opt = new JsonSerializerOptions { IncludeFields = true };
            var p = JsonSerializer.Deserialize<XuProject>(File.ReadAllText(path), opt);
            if (p == null) throw new InvalidDataException("工程文件为空或格式错误");
            // 防御性补齐：老版本工程可能没有 16 个 pattern
            while (p.Patterns.Count < 16) p.Patterns.Add(new XuPattern { Name = $"PATTERN {p.Patterns.Count + 1}" });
            while (p.Pads.Count < 16) p.Pads.Add(new XuPadParams());
            p.CurrentPattern = Math.Clamp(p.CurrentPattern, 0, p.Patterns.Count - 1);
            return p;
        }

        /// <summary>复制一份，供离线渲染使用，避免影响现场播放。</summary>
        public XuProject Clone()
        {
            // 用 JSON 往返做深拷贝：代码量最少，且不会漏字段
            var json = JsonSerializer.Serialize(this);
            return JsonSerializer.Deserialize<XuProject>(json) ?? new XuProject();
        }
    }

    public static class XuCond
    {
        public const int Always = 0, Fill = 1, NotFill = 2, Every2 = 3, Every4 = 4;
        public static readonly string[] Names = { "总是", "仅过门", "非过门", "每2循环", "每4循环" };
    }
}
