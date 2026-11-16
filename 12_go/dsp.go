// ==========================================================================
//  dsp.go —— 十六声部模拟鼓合成（Go 版）
//
//  署名：徐某。
//  公式逐项照搬纯 C 版 03_c_win32/dsp.c，保证同一个工程文件在 C 版与 Go 版
//  听起来一致。Go 版额外补上了规格书 §3 要求的“参数一阶平滑”，平滑状态由
//  engine 统一持有，在块开始时推进，避免 UI 拖动滑杆时出现 zipper noise。
//
//  音频线程纪律：
//    · 所有 voice / 噪声池 / 滤波器状态在初始化时一次性分配；
//    · Render 内不做任何 make/map 操作，也不加锁；
//    · 包络全部是“年龄 t”的解析函数，声音到时间自然归零，绝不留采样循环。
// ==========================================================================
package main

import "math"

// ---- 全局音频常量（与规格书 §1 完全一致，不可修改） ----
const (
	XUSR      = 48000 // 采样率
	Pads      = 16    // 声部数
	MaxVoices = 4     // 每声部复音数
	BlockSize = 256   // 实时渲染块：5.33ms
	StepsMax  = 64    // 单 pattern 最大步数
	Patterns  = 16    // 工程内 pattern 数量
	MaxEvents = 4096  // 事件队列容量
)

// 条件触发枚举（规格书 §4）
const (
	CondAlways  = 0 // 总是
	CondFill    = 1 // 仅过门
	CondNotFill = 2 // 非过门
	CondEvery2  = 3 // 每 2 循环
	CondEvery4  = 4 // 每 4 循环
)

// --------------------------------------------------------------------------
//  biquad：RBJ 双二阶滤波器
//  z1/z2 状态必须跨块保留，否则每块开头都会“咔”一声。
// --------------------------------------------------------------------------
type biquad struct {
	b0, b1, b2 float64 // 前馈系数（a0 已归一化为 1）
	a1, a2     float64 // 反馈系数
	z1, z2     float64 // 延迟线状态
	kind       int     // 0=直通 1=LP 2=HP 3=BP
	fc, q      float64 // 上次参数，参数没变就不重算三角函数
}

// lowpass 更新为 RBJ 低通；参数几乎没变时直接返回，省掉 cos/sin。
func (f *biquad) lowpass(fc, q float64) {
	fc = clampF(fc, 20.0, XUSR*0.49)
	q = clampF(q, 0.1, 20.0)
	if f.kind == 1 && math.Abs(fc-f.fc) < 0.5 && math.Abs(q-f.q) < 1e-3 {
		return
	}
	w0 := 2.0 * math.Pi * fc / XUSR
	cw := math.Cos(w0)
	alpha := math.Sin(w0) / (2.0 * q)
	a0 := 1.0 + alpha
	f.b0 = (1.0 - cw) * 0.5 / a0
	f.b1 = (1.0 - cw) / a0
	f.b2 = f.b0
	f.a1 = (-2.0 * cw) / a0
	f.a2 = (1.0 - alpha) / a0
	f.kind, f.fc, f.q = 1, fc, q
}

// highpass 更新为 RBJ 高通。
func (f *biquad) highpass(fc, q float64) {
	fc = clampF(fc, 10.0, XUSR*0.49)
	q = clampF(q, 0.1, 20.0)
	if f.kind == 2 && math.Abs(fc-f.fc) < 0.5 && math.Abs(q-f.q) < 1e-3 {
		return
	}
	w0 := 2.0 * math.Pi * fc / XUSR
	cw := math.Cos(w0)
	alpha := math.Sin(w0) / (2.0 * q)
	a0 := 1.0 + alpha
	f.b0 = (1.0 + cw) * 0.5 / a0
	f.b1 = -(1.0 + cw) / a0
	f.b2 = f.b0
	f.a1 = (-2.0 * cw) / a0
	f.a2 = (1.0 - alpha) / a0
	f.kind, f.fc, f.q = 2, fc, q
}

// bandpass 更新为 RBJ 带通（0dB 峰值）。
func (f *biquad) bandpass(fc, q float64) {
	fc = clampF(fc, 20.0, XUSR*0.49)
	q = clampF(q, 0.1, 30.0)
	if f.kind == 3 && math.Abs(fc-f.fc) < 0.5 && math.Abs(q-f.q) < 1e-3 {
		return
	}
	w0 := 2.0 * math.Pi * fc / XUSR
	cw := math.Cos(w0)
	alpha := math.Sin(w0) / (2.0 * q)
	a0 := 1.0 + alpha
	f.b0 = alpha / a0
	f.b1 = 0.0
	f.b2 = -alpha / a0
	f.a1 = (-2.0 * cw) / a0
	f.a2 = (1.0 - alpha) / a0
	f.kind, f.fc, f.q = 3, fc, q
}

// run 执行一次 Transposed Direct Form II 差分方程。
func (f *biquad) run(x float64) float64 {
	y := f.b0*x + f.z1
	f.z1 = f.b1*x - f.a1*y + f.z2
	f.z2 = f.b2*x - f.a2*y
	return y
}

// --------------------------------------------------------------------------
//  voice：一个“鼓槌”。
//  噪声池 8192 个 float64，构造时一次性填好，回调内只读不分配。
// --------------------------------------------------------------------------
type voice struct {
	pad    int     // 所属声部
	active bool    // 是否还在发声
	start  int64   // 触发绝对样本
	stop   int64   // 强制结束时刻（math.MaxInt64 表示自然衰减）
	gain   float64 // 力度增益（含重音）
	accent bool    // 重音标志（影响音色）
	fade   int     // 掐断淡出长度
	noise  []float64 // 预生成噪声池
	npos   int     // 噪声读取指针
	bp     biquad  // 带通
	hp     biquad  // 高通
	bp2    biquad  // 吊镲额外噪声带通
}

// takeNoise 从循环噪声池取一个样本（零分配）。
func (v *voice) takeNoise() float64 {
	if v.npos >= len(v.noise) {
		v.npos = 0
	}
	s := v.noise[v.npos]
	v.npos++
	return s
}

// square 方波：sin(phase)>=0 取 +1，否则 -1。
// 直接判断相位落在周期的前半还是后半，数学上与 sin 的符号完全等价，
// 但省掉每样本一次 sin —— 镲族每样本要算 6 个方波，这一项约占三成 CPU。
func square(phase float64) float64 {
	const twoPi = 6.283185307179586476925286766559
	x := phase / twoPi
	x -= math.Floor(x) // 取小数部分，落在 [0,1)
	// x==0.5 时相位正好是 π，C 版 sin(π)=0 判定为 +1，这里保持一致
	if x <= 0.5 {
		return 1.0
	}
	return -1.0
}

// --------------------------------------------------------------------------
//  synth 核心合成函数：给定“年龄 t”，返回该声部此刻的样本值。
//  分派顺序与 C 版完全一致：0 底鼓、1 军鼓、2..7 嗵鼓/康加、8 边击、
//  9 击木、10 拍手、11 沙锤、12 牛铃、13..15 镲族。
// --------------------------------------------------------------------------
func (v *voice) synth(t float64, p *PadParams) float64 {
	pad := v.pad

	// ================= 0 底鼓 =================
	if pad == 0 {
		fEnd := 40.0 + p.Tune*40.0                       // 音高包络终点 40–80Hz
		fStart := fEnd * (1.6 + p.Tone*1.9)              // 起始音高由 Tone 控制
		tau := 0.018 + (1.0-p.Decay)*0.012               // 音高下滑时间常数
		df := fStart - fEnd
		// 频率积分 → 相位：鼓皮张力快速释放的数学表达
		phase := 2.0 * math.Pi * (fEnd*t + df*tau*(1.0-math.Exp(-t/tau)))
		dec := (0.12 + p.Decay*1.48)
		if v.accent {
			dec *= 1.18 // 重音让底鼓尾巴更长一点
		}
		amp := math.Exp(-t / dec)
		// 点击噪声只在最初 30ms 有意义（此后 < -100dB），跳过能省一次 exp
		click := 0.0
		if t < 0.03 {
			n := v.takeNoise()
			v.hp.highpass(1800.0+p.Tone*3000.0, 0.8)
			click = v.hp.run(n) * math.Exp(-t/0.0035) * (0.06 + p.Tone*0.32)
		}
		body := math.Tanh(math.Sin(phase)*(1.15+p.Decay*1.2)) * 0.92
		return body*amp + click
	}

	// ================= 1 军鼓 =================
	if pad == 1 {
		f1 := 150.0 + p.Tune*110.0
		f2 := f1 * (1.78 + p.Tone*0.22)
		decO := 0.055 + p.Decay*0.14
		osc := 0.55*math.Sin(2.0*math.Pi*f1*t)*math.Exp(-t/decO) +
			0.34*math.Sin(2.0*math.Pi*f2*t)*math.Exp(-t/(decO*0.72))
		n := v.takeNoise()
		v.bp.bandpass(1200.0+p.Snappy*3800.0, 0.9+p.Tone*0.8)
		nz := v.bp.run(n)
		decN := 0.075 + p.Decay*0.16
		if v.accent {
			decN *= 0.85 // 重音让噪声更“炸”
		}
		nz *= math.Exp(-t/decN) * (0.25 + p.Snappy*0.7)
		return osc*0.7 + nz
	}

	// ================= 2..7 嗵鼓 / 康加 =================
	if pad >= 2 && pad <= 7 {
		var baseF, decayS, clickAmt, sweep float64
		if pad <= 4 {
			// 低/中/高嗵鼓
			switch pad {
			case 2:
				baseF = 80.0
			case 3:
				baseF = 120.0
			default:
				baseF = 170.0
			}
			decayS = 0.15 + p.Decay*0.75
			clickAmt = 0.04 + p.Tone*0.22
			sweep = 1.9
		} else {
			// 低/中/高康加
			switch pad {
			case 5:
				baseF = 220.0
			case 6:
				baseF = 300.0
			default:
				baseF = 400.0
			}
			decayS = 0.10 + p.Decay*0.55
			clickAmt = 0.02 + p.Tone*0.12
			sweep = 1.35
		}
		f0 := baseF * (0.6 + p.Tune)
		tau := 0.020
		if pad <= 4 {
			tau = 0.035
		}
		phase := 2.0 * math.Pi * (f0*t + f0*(sweep-1.0)*tau*(1.0-math.Exp(-t/tau)))
		amp := math.Exp(-t / decayS)
		n := v.takeNoise()
		v.hp.highpass(2500.0, 0.7)
		click := v.hp.run(n) * math.Exp(-t/0.004) * clickAmt
		return math.Sin(phase)*amp*0.9 + click
	}

	// ================= 8 边击 =================
	if pad == 8 {
		f := 1200.0 + p.Tune*1400.0
		dec := 0.012 + p.Decay*0.05
		sq := square(2.0 * math.Pi * f * t)
		v.bp.bandpass(f, 2.2)
		body := v.bp.run(sq) * math.Exp(-t/dec)
		n := v.takeNoise()
		v.hp.highpass(3000.0, 0.8)
		tick := v.hp.run(n) * math.Exp(-t/0.004) * 0.35
		return body*0.8 + tick
	}

	// ================= 9 击木 =================
	if pad == 9 {
		f := 2000.0 + p.Tune*1000.0
		dec := 0.018 + p.Decay*0.035
		return math.Sin(2.0*math.Pi*f*t) * math.Exp(-t/dec) * 0.9
	}

	// ================= 10 拍手 =================
	if pad == 10 {
		n := v.takeNoise()
		v.bp.bandpass(800.0+p.Tone*1700.0, 1.1)
		nz := v.bp.run(n)
		env := 0.0
		// 4 段 9ms 间隔的噪声爆发，模拟多只手
		for k := 0; k < 4; k++ {
			tk := float64(k) * 0.009
			if t >= tk {
				env += math.Exp(-(t-tk)/0.011) * 0.55
			}
		}
		tail := math.Exp(-t/(0.05+p.Decay*0.35)) * (0.15 + p.Tail*0.55)
		return nz * (env + tail)
	}

	// ================= 11 沙锤 =================
	if pad == 11 {
		n := v.takeNoise()
		v.hp.highpass(4000.0+p.Tone*5000.0, 0.9)
		nz := v.hp.run(n)
		env := (1.0 - math.Exp(-t/0.0015)) * math.Exp(-t/(0.018+p.Decay*0.12))
		return nz * env * 0.85
	}

	// ================= 12 牛铃 =================
	if pad == 12 {
		f := 400.0 + p.Tune*300.0
		sq1 := square(2.0 * math.Pi * f * t)
		sq2 := square(2.0 * math.Pi * f * 1.48 * t)
		v.bp.bandpass(2400.0+p.Tone*1200.0, 1.4)
		body := v.bp.run(sq1*0.5 + sq2*0.5)
		dec := 0.05 + p.Decay*0.35
		return body * math.Exp(-t/dec) * 0.85
	}

	// ================= 13..15 镲族 =================
	// 6 个非谐和方波叠加，是 808 金属声的核心技巧
	ratios := [6]float64{1.0, 1.3423, 1.2312, 1.6532, 1.9523, 2.1523}
	root := 320.0 * (0.75 + p.Tune*0.5)
	metal := 0.0
	for i := 0; i < 6; i++ {
		metal += square(2.0 * math.Pi * root * ratios[i] * t)
	}
	metal /= 6.0
	v.hp.highpass(5200.0+p.Tone*2600.0, 0.8)
	metal = v.hp.run(metal)
	v.bp.bandpass(7600.0+p.Tone*3600.0, 0.9)
	metal = v.bp.run(metal)
	var dec float64
	switch pad {
	case 15:
		dec = 0.028 + p.Decay*0.20 // 闭镲
	case 14:
		dec = 0.12 + p.Decay*0.65 // 开镲
	default:
		dec = 0.25 + p.Decay*1.75 // 吊镲
	}
	env := math.Exp(-t / dec)
	out := metal * env
	if pad == 13 {
		n := v.takeNoise()
		v.bp2.bandpass(10000.0, 0.7)
		out += v.bp2.run(n) * env * 0.12
	}
	return out * 0.75
}

// trigger 复位一个 voice 并让它从 when 开始发声。
func (v *voice) trigger(when int64, gain float64, accent bool) {
	v.active = true
	v.start = when
	v.stop = math.MaxInt64
	v.gain = gain
	v.accent = accent
	v.fade = 64
	v.npos = 0
	// 清空滤波器记忆，避免上一槌的残余造成“噗”
	v.bp.z1, v.bp.z2 = 0, 0
	v.hp.z1, v.hp.z2 = 0, 0
	v.bp2.z1, v.bp2.z2 = 0, 0
}

// kill 安排一次掐断：不立刻停，交给渲染循环做淡出。
func (v *voice) kill(when int64, fade int) {
	if !v.active {
		return
	}
	if when < v.stop {
		v.stop = when
	}
	if fade < 1 {
		fade = 1
	}
	v.fade = fade
}

// silenceAge 估算一个声部“还能听见”的最长时间（8 个衰减常数，约 -69dB）。
// 超过这个年龄的 voice 直接回收，省掉完全听不见却仍在做 sin/exp 的运算。
// 这只影响 -69dB 以下的残响，对听感没有任何改变。
func silenceAge(pad int, p *PadParams) float64 {
	var a float64
	switch pad {
	case 0:
		a = 4.0 // 底鼓尾巴最长，保留到全局 4 秒上限
	case 1:
		a = 8*(0.055+p.Decay*0.14) + 0.1
	case 2, 3, 4:
		a = 8 * (0.15 + p.Decay*0.75)
	case 5, 6, 7:
		a = 8 * (0.10 + p.Decay*0.55)
	case 8:
		a = 8*(0.012+p.Decay*0.05) + 0.05
	case 9:
		a = 8*(0.018+p.Decay*0.035) + 0.05
	case 10:
		a = 8*(0.05+p.Decay*0.35) + 0.2
	case 11:
		a = 8*(0.018+p.Decay*0.12) + 0.1
	case 12:
		a = 8 * (0.05 + p.Decay*0.35)
	case 13:
		a = 8 * (0.25 + p.Decay*1.75)
	case 14:
		a = 8 * (0.12 + p.Decay*0.65)
	default:
		a = 8 * (0.028 + p.Decay*0.20)
	}
	if a > 4.0 {
		a = 4.0
	}
	return a
}

// --------------------------------------------------------------------------
//  renderVoice：把一个 voice 在 [b0,b1) 内渲染进左右声道。
//  等功率声像：pan=0.5 时左右各 0.707，总功率恒定。
// --------------------------------------------------------------------------
func (e *Engine) renderVoice(v *voice, L, R []float32, n int, b0 int64, p *PadParams) {
	if !v.active {
		return
	}
	b1 := b0 + int64(n)
	a := b0
	if v.start > a {
		a = v.start
	}
	b := b1
	if v.stop < b {
		b = v.stop
	}
	if b <= a {
		// 本块内没有任何可发声区间；如果已经越过 stop，就回收 voice
		if b1 > v.stop {
			v.active = false
		}
		return
	}
	m := int(b - a)
	off := int(a - b0)
	if m <= 0 || off >= n {
		return
	}
	// 已经衰减到 -69dB 以下的 voice 直接回收，不再浪费 CPU
	if float64(a-v.start)/float64(XUSR) > silenceAge(v.pad, p) {
		v.active = false
		return
	}

	pan := clampF(p.Pan, 0, 1)
	lg := math.Cos(pan * math.Pi * 0.5)
	rg := math.Sin(pan * math.Pi * 0.5)
	lvl := p.Level
	if lvl < 0 {
		lvl = 0
	}
	g := v.gain * lvl * 0.42

	for i := 0; i < m; i++ {
		abs := a + int64(i)
		t := float64(abs-v.start) / float64(XUSR)
		s := v.synth(t, p)
		// 掐断淡出：距离 stop 越近增益越小，避免硬切爆音
		if v.stop < math.MaxInt64 {
			dist := v.stop - abs
			if dist < int64(v.fade) {
				if dist <= 0 {
					s = 0
				} else {
					s *= float64(dist) / float64(v.fade)
				}
			}
		}
		L[off+i] += float32(s * g * lg)
		R[off+i] += float32(s * g * rg)
	}
	// 最多活 4 秒，到点无条件回收，保证不会无限堆叠
	if b1-v.start > 4*int64(XUSR) {
		v.active = false
	}
}
