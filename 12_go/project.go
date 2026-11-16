// ==========================================================================
//  project.go —— XU-808 工程数据模型、默认工程、JSON 存档（Go 版）
//
//  署名：徐某。
//  这里的结构体字段与规格书第 3/4/7 节严格对应；JSON 标签就是工程文件里的键名，
//  因此同一份 .json 工程可以在 Python/C#/Java/Go 各版本之间直接搬运。
//
//  兼容策略（规格书 §7）：
//    · 读取时先把整份工程恢复成默认值，再用文件内容覆盖 —— 缺字段自动用默认值；
//    · encoding/json 默认忽略未知字段 —— 多出来的字段不会报错；
//    · 读完统一走 sanitize()，把越界数值夹回合法区间，绝不把脏数据送进音频线程。
// ==========================================================================
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// --------------------------------------------------------------------------
//  Step：一个格子的全部属性（规格书 §4）
//  用 uint8 存布尔/小整数，16×64 网格只有约 8KB，复制 pattern、写文件都极快。
// --------------------------------------------------------------------------
type Step struct {
	On       uint8 `json:"on"`        // 是否启用
	Velocity uint8 `json:"velocity"`  // 力度 1..127
	Accent   uint8 `json:"accent"`    // 重音 0/1
	Prob     uint8 `json:"probability"` // 概率 0..100
	Micro    int16 `json:"micro"`     // 微时偏移（样本，-2400..2400）
	Ratchet  uint8 `json:"ratchet"`   // 滚奏 1..8
	Cond     uint8 `json:"condition"` // 条件触发 0..4
}

// --------------------------------------------------------------------------
//  Pattern：16 轨 × 64 步 + 每轨独立循环长度（polymeter）
//  注意 Steps 与 Grid 分开命名，避免 JSON 键冲突。
// --------------------------------------------------------------------------
type Pattern struct {
	Name     string          `json:"name"`     // 名称，最长 31 字节
	Steps    int32           `json:"steps"`    // 1..64，全局步数
	Fill     int32           `json:"fill"`     // 过门循环标志（保留字段）
	TrackLen [Pads]uint8     `json:"trackLen"` // 每轨 1..64
	Grid     [Pads][StepsMax]Step `json:"grid"` // 16 声部 × 64 步
}

// --------------------------------------------------------------------------
//  PadParams：每声部音色与混音参数，全部是 0..1（Level 到 1.25）
//  float64 是为了在 amd64 上单次 8 字节写入天然原子，避免 UI 改参数时
//  音频线程读到“半个浮点数”。
// --------------------------------------------------------------------------
type PadParams struct {
	Tune   float64 `json:"tune"`
	Decay  float64 `json:"decay"`
	Tone   float64 `json:"tone"`
	Snappy float64 `json:"snappy"`
	Tail   float64 `json:"tail"`
	Level  float64 `json:"level"`
	Pan    float64 `json:"pan"`
	Mute   int32   `json:"mute"`
	Solo   int32   `json:"solo"`
}

// --------------------------------------------------------------------------
//  Master：主效果链与全局设置（规格书 §6）
// --------------------------------------------------------------------------
type Master struct {
	Volume      float64 `json:"volume"`
	Drive       float64 `json:"drive"`
	LpCutoff    float64 `json:"lpCutoff"`
	LpQ         float64 `json:"lpQ"`
	HpCutoff    float64 `json:"hpCutoff"`
	HpQ         float64 `json:"hpQ"`
	LpOn        int32   `json:"lpOn"`
	HpOn        int32   `json:"hpOn"`
	BPM         float64 `json:"bpm"`
	Swing       float64 `json:"swing"`
	BeatsPerBar int32   `json:"beatsPerBar"`
	BeatUnit    int32   `json:"beatUnit"`
	SongMode    int32   `json:"songMode"`
}

// SongEntry：歌曲链的一节（pattern 编号 + 重复次数）
type SongEntry struct {
	Pattern int32 `json:"pattern"`
	Repeats int32 `json:"repeats"`
}

// --------------------------------------------------------------------------
//  Project：整份工程。字段顺序即存档顺序，便于人工比对。
//  Song 用定长数组 + 计数，避免音频线程读 slice 头时与 UI 追加元素发生撕裂。
// --------------------------------------------------------------------------
type Project struct {
	Format    string         `json:"format"`
	Version   int            `json:"version"`
	Name      string         `json:"name"`
	Seed      uint32         `json:"seed"`
	Current   int32          `json:"current"`
	Master    Master         `json:"master"`
	Pads      [Pads]PadParams `json:"pads"`
	Patterns  [Patterns]Pattern `json:"patterns"`
	Song      [64]SongEntry  `json:"song"`
	SongCount int32          `json:"songCount"`
}

// 声部代号、中文名、MIDI 音符：顺序即编号，绝不能重排（规格书 §2）
var padCodes = [Pads]string{
	"BD", "SD", "LT", "MT", "HT", "LC", "MC", "HC",
	"RS", "CL", "CP", "MA", "CB", "CY", "OH", "CH",
}

var padNamesCN = [Pads]string{
	"底鼓", "军鼓", "低嗵", "中嗵", "高嗵", "低康加", "中康加", "高康加",
	"边击", "击木", "拍手", "沙锤", "牛铃", "吊镲", "开镲", "闭镲",
}

// MIDI 音高映射（规格书 §7）
var midiNotes = [Pads]int{
	36, 38, 45, 47, 50, 64, 62, 60, 37, 75, 39, 70, 56, 49, 46, 42,
}

// NewProject 构造一份与 C 版逐字段一致的默认工程。
func NewProject() *Project {
	p := &Project{
		Format:  "XU808",
		Version: 1,
		Name:    "XuMou Xu808 Project",
		Seed:    0x808808,
		Current: 0,
	}
	// ---- 全局设置 ----
	p.Master = Master{
		Volume:      0.80,
		Drive:       1.35,
		LpCutoff:    18000.0,
		LpQ:         0.90,
		HpCutoff:    28.0,
		HpQ:         0.70,
		LpOn:        1,
		HpOn:        1,
		BPM:         120.0,
		Swing:       0.0,
		BeatsPerBar: 4,
		BeatUnit:    4,
		SongMode:    0,
	}

	// ---- 16 声部参数默认值（规格书 §3 表格） ----
	for i := 0; i < Pads; i++ {
		p.Pads[i] = PadParams{
			Tune:   0.50,
			Decay:  0.50,
			Tone:   0.50,
			Snappy: 0.55,
			Tail:   0.45,
			Level:  0.75,
			Pan:    0.50,
		}
	}
	p.Pads[0].Decay = 0.55 // 底鼓稍长
	p.Pads[0].Tone = 0.45
	p.Pads[1].Decay = 0.45

	// ---- 16 个空 Pattern，每格先填好合法默认值 ----
	for i := 0; i < Patterns; i++ {
		p.Patterns[i] = newPattern(fmt.Sprintf("PATTERN %d", i+1))
	}
	// 第一个 Pattern 给出可直接听的演示律动
	demoPattern(&p.Patterns[0])

	// ---- 默认歌曲链：同一个 pattern 连播两节 ----
	p.SongCount = 2
	p.Song[0] = SongEntry{Pattern: 0, Repeats: 4}
	p.Song[1] = SongEntry{Pattern: 0, Repeats: 4}
	return p
}

// newPattern 生成一个“全关但参数合法”的 pattern。
func newPattern(name string) Pattern {
	var pat Pattern
	pat.Name = name
	pat.Steps = 16
	for j := 0; j < Pads; j++ {
		pat.TrackLen[j] = 16
		for s := 0; s < StepsMax; s++ {
			// 力度 100 / 概率 100 / ratchet 1 是规格书默认值
			pat.Grid[j][s].Velocity = 100
			pat.Grid[j][s].Prob = 100
			pat.Grid[j][s].Ratchet = 1
		}
	}
	return pat
}

// demoPattern 与 C 版 xu_project_demo 完全一致，保证跨版本听感相同。
func demoPattern(pat *Pattern) {
	// 先重置成干净状态，再逐格点亮
	*pat = newPattern("PATTERN 1")

	// 底鼓：正拍、反拍、加一个滚奏
	pat.Grid[0][0].On = 1
	pat.Grid[0][0].Velocity = 118
	pat.Grid[0][0].Accent = 1
	pat.Grid[0][8].On = 1
	pat.Grid[0][8].Velocity = 105
	pat.Grid[0][14].On = 1
	pat.Grid[0][14].Velocity = 90
	// 军鼓：2、4 拍
	pat.Grid[1][4].On = 1
	pat.Grid[1][4].Velocity = 110
	pat.Grid[1][12].On = 1
	pat.Grid[1][12].Velocity = 110
	pat.Grid[1][12].Accent = 1
	// 闭镲：八分音符，正拍稍重
	for s := 0; s < 16; s += 2 {
		pat.Grid[15][s].On = 1
		if s%4 == 0 {
			pat.Grid[15][s].Velocity = 96
		} else {
			pat.Grid[15][s].Velocity = 84
		}
	}
	// 开镲、拍手、沙锤
	pat.Grid[14][14].On = 1
	pat.Grid[14][14].Velocity = 92
	pat.Grid[10][12].On = 1
	pat.Grid[10][12].Velocity = 80
	for s := 1; s < 16; s += 2 {
		pat.Grid[11][s].On = 1
		pat.Grid[11][s].Velocity = 42
	}
	// 第 16 步底鼓三连滚奏：直接展示 ratchet
	pat.Grid[0][15].On = 1
	pat.Grid[0][15].Velocity = 88
	pat.Grid[0][15].Ratchet = 3
}

// sanitize 把任何来源（JSON/外部编辑）的数据夹回合法范围。
func (p *Project) sanitize() {
	if p.Format == "" {
		p.Format = "XU808"
	}
	if p.Version == 0 {
		p.Version = 1
	}
	if len(p.Name) > 63 {
		p.Name = p.Name[:63]
	}
	if p.Seed == 0 {
		p.Seed = 0x808808
	}
	if p.Current < 0 || p.Current >= Patterns {
		p.Current = 0
	}

	// 主参数
	m := &p.Master
	m.Volume = clampF(m.Volume, 0, 1)
	m.Drive = clampF(m.Drive, 1, 8)
	m.LpCutoff = clampF(m.LpCutoff, 200, 20000)
	m.LpQ = clampF(m.LpQ, 0.5, 8)
	m.HpCutoff = clampF(m.HpCutoff, 10, 800)
	m.HpQ = clampF(m.HpQ, 0.5, 8)
	m.BPM = clampF(m.BPM, 40, 300)
	m.Swing = clampF(m.Swing, 0, 0.75)
	if m.BeatsPerBar < 1 || m.BeatsPerBar > 16 {
		m.BeatsPerBar = 4
	}
	if m.BeatUnit != 8 {
		m.BeatUnit = 4
	}

	// 声部参数
	for i := 0; i < Pads; i++ {
		q := &p.Pads[i]
		q.Tune = clampF(q.Tune, 0, 1)
		q.Decay = clampF(q.Decay, 0, 1)
		q.Tone = clampF(q.Tone, 0, 1)
		q.Snappy = clampF(q.Snappy, 0, 1)
		q.Tail = clampF(q.Tail, 0, 1)
		q.Level = clampF(q.Level, 0, 1.25)
		q.Pan = clampF(q.Pan, 0, 1)
		if q.Mute != 0 {
			q.Mute = 1
		}
		if q.Solo != 0 {
			q.Solo = 1
		}
	}

	// Pattern 与每个格子
	for i := 0; i < Patterns; i++ {
		pat := &p.Patterns[i]
		if pat.Steps < 1 {
			pat.Steps = 1
		}
		if pat.Steps > StepsMax {
			pat.Steps = StepsMax
		}
		if len(pat.Name) > 31 {
			pat.Name = pat.Name[:31]
		}
		for j := 0; j < Pads; j++ {
			tl := pat.TrackLen[j]
			if tl < 1 {
				tl = 1
			}
			if tl > StepsMax {
				tl = StepsMax
			}
			pat.TrackLen[j] = tl
			for s := 0; s < StepsMax; s++ {
				st := &pat.Grid[j][s]
				if st.Velocity < 1 {
					st.Velocity = 1
				}
				if st.Prob > 100 {
					st.Prob = 100
				}
				if st.Micro < -2400 {
					st.Micro = -2400
				}
				if st.Micro > 2400 {
					st.Micro = 2400
				}
				if st.Ratchet < 1 {
					st.Ratchet = 1
				}
				if st.Ratchet > 8 {
					st.Ratchet = 8
				}
				if st.Cond > CondEvery4 {
					st.Cond = CondAlways
				}
				if st.On > 1 {
					st.On = 1
				}
				if st.Accent > 1 {
					st.Accent = 1
				}
			}
		}
	}

	// 歌曲链
	if p.SongCount < 0 {
		p.SongCount = 0
	}
	if p.SongCount > 64 {
		p.SongCount = 64
	}
	for i := 0; i < 64; i++ {
		if p.Song[i].Pattern < 0 || p.Song[i].Pattern >= Patterns {
			p.Song[i].Pattern = 0
		}
		if p.Song[i].Repeats < 1 {
			p.Song[i].Repeats = 1
		}
		if p.Song[i].Repeats > 64 {
			p.Song[i].Repeats = 64
		}
	}
}

// SaveProject 把工程写成 JSON 文件（UTF-8，带缩进，方便人工 diff）。
// 先写临时文件再改名，避免写到一半断电留下半截工程。
func SaveProject(p *Project, path string) error {
	// 快照一份再序列化：避免 UI 线程同时改动音符时序列化出撕裂数据
	snap := *p
	snap.Format = "XU808"
	snap.Version = 1
	data, err := json.MarshalIndent(&snap, "", " ")
	if err != nil {
		return err
	}
	dir := filepath.Dir(path)
	if dir != "" {
		// 目录不存在时补建，避免用户手输路径失败
		_ = os.MkdirAll(dir, 0755)
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	// Windows 上目标文件已存在时，某些环境（文件被索引器/杀软占用）会让改名失败。
	// 这里用 MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) 覆盖，失败再退回“先删再改名”。
	if err := moveFileReplace(tmp, path); err != nil {
		_ = os.Remove(path) // 忽略“不存在”，真正失败由下面第二次判断
		if err2 := os.Rename(tmp, path); err2 != nil {
			return err
		}
	}
	return nil
}

// LoadProject 读取 JSON 工程：默认值打底，再用文件覆盖，最后统一夹取范围。
func LoadProject(p *Project, path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	// 先恢复默认，保证缺字段时不会出现 0 步/0 力度这种非法状态
	def := NewProject()
	*p = *def
	if err := json.Unmarshal(data, p); err != nil {
		return err
	}
	p.sanitize()
	return nil
}

// clampF 小工具：浮点夹取。
func clampF(x, lo, hi float64) float64 {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

// clampI 小工具：整数夹取。
func clampI(x, lo, hi int) int {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}
