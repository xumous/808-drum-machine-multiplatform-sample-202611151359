// ==========================================================================
//  engine.go —— 音序器、混音、主效果链（Go 版）
//
//  署名：徐某。
//  实时纪律与 C 版完全一致：
//    1. samplePos（int64 样本计数）是唯一时钟，绝不依赖 UI 定时器；
//    2. 所有事件先展开成绝对样本时刻，再按块切段渲染；
//    3. Render 内不 make、不加锁、不读写文件；
//    4. 停止时给 voice 10ms 淡出，既即时安静又不“咔哒”。
//
//  线程模型：
//    · UI 线程：改工程参数、点鼓垫（写入无锁环形队列）；
//    · 音频线程：唯一调用 Render 的人，独占 voices / 事件展开；
//    · 跨线程标量全部用 atomic，避免 Go 的内存模型把“播放标志”缓存住。
// ==========================================================================
package main

import (
	"math"
	"sync/atomic"
)

// evt：内部事件（绝对样本时刻 + 声部 + 力度 + 重音）
type evt struct {
	t      int64
	pad    int
	vel    float64
	accent bool
}

// --------------------------------------------------------------------------
//  eventRing：单生产者（UI）/ 单消费者（音频）的无锁环形队列。
//  UI 点鼓垫、MIDI 输入都在 UI 线程里 push，音频线程在块开始处 pop，
//  因此不需要任何互斥锁，也不会让音频线程等待 UI。
// --------------------------------------------------------------------------
const manualCap = 256

type eventRing struct {
	buf  [manualCap]evt
	head atomic.Uint32 // 生产者写
	tail atomic.Uint32 // 消费者写
}

func (r *eventRing) push(ev evt) {
	h := r.head.Load()
	t := r.tail.Load()
	if h-t >= manualCap {
		return // 满了就丢最新的：宁可少一下，也绝不阻塞 UI
	}
	r.buf[h%manualCap] = ev
	r.head.Store(h + 1)
}

func (r *eventRing) pop() (evt, bool) {
	t := r.tail.Load()
	h := r.head.Load()
	if t == h {
		return evt{}, false
	}
	ev := r.buf[t%manualCap]
	r.tail.Store(t + 1)
	return ev, true
}

// --------------------------------------------------------------------------
//  Engine：音序器 + 混音 + 主效果 + 统计
// --------------------------------------------------------------------------
type Engine struct {
	proj *Project

	voices [Pads][MaxVoices]voice
	sm     [Pads]PadParams // 参数平滑后的实际使用值（音频线程独占）

	// ---- 跨线程标志 ----
	playing        atomic.Int32 // 1=正在播放
	playReq        atomic.Int32 // UI 请求播放（音频线程消费）
	stopReq        atomic.Int32 // UI 请求停止
	stepIndex      atomic.Int32 // 播放头（UI 只读）
	uiPos          atomic.Int64 // 当前样本位置镜像（UI 只读）
	audioActive    atomic.Bool  // 音频设备是否在跑
	pendingPattern atomic.Int32 // 下一步生效的 pattern，-1 表示无
	extPulses      atomic.Int32 // 外部 MIDI 时钟脉冲计数
	xruns          atomic.Int32 // 欠载计数
	cpuBits        atomic.Uint64 // CPU 占用（float64 位模式）
	meterLBits     atomic.Uint64 // 左声道峰值
	meterRBits     atomic.Uint64 // 右声道峰值
	clockExternal  atomic.Int32 // 是否使用外部 MIDI 时钟

	// ---- 音频线程独占 ----
	samplePos   int64
	nextStepAbs int64
	globalStep  int64
	cycleCount  int
	fillNow     int
	songIndex   int
	songRepeat  int
	rng         uint32
	soloOverride int32 // >=0 时离线分轨只让该轨出声

	carry      [MaxEvents]evt // 上一块留下的“未来事件”
	carryCount int
	scratch    [MaxEvents]evt // 本块处理缓冲

	manual eventRing

	// 主效果滤波器与平滑状态
	hpL, hpR, lpL, lpR               biquad
	smHpCut, smHpQ, smLpCut, smLpQ  float64
	smVol, smDrive                   float64
	clockPulse                       int
}

// NewEngine 构造引擎：填噪声池、拷贝默认参数、准备滤波器状态。
func NewEngine(p *Project) *Engine {
	e := &Engine{proj: p}
	e.pendingPattern.Store(-1)
	e.soloOverride = -1
	e.rng = p.Seed
	if e.rng == 0 {
		e.rng = 0x808808
	}
	// 噪声池用确定性伪随机：同一台机器每次启动音色完全一致
	for i := 0; i < Pads; i++ {
		for v := 0; v < MaxVoices; v++ {
			vo := &e.voices[i][v]
			vo.pad = i
			vo.noise = make([]float64, 8192)
			st := uint32(0x808 + i*7919)
			for k := 0; k < 8192; k++ {
				st = st*1664525 + 1013904223
				vo.noise[k] = float64(int32(st>>8))/8388608.0 - 1.0
			}
			vo.stop = math.MaxInt64
		}
	}
	// 平滑状态初值 = 目标值，避免启动瞬间的“滑动”
	for i := 0; i < Pads; i++ {
		e.sm[i] = p.Pads[i]
	}
	e.smHpCut = p.Master.HpCutoff
	e.smHpQ = p.Master.HpQ
	e.smLpCut = p.Master.LpCutoff
	e.smLpQ = p.Master.LpQ
	e.smVol = p.Master.Volume
	e.smDrive = p.Master.Drive
	return e
}

// --------------------------------------------------------------------------
//  传输控制：UI 只发请求，真正的状态切换发生在音频线程块首，
//  这样“播放/停止”与渲染永远不会撞在一起。
// --------------------------------------------------------------------------
func (e *Engine) Play() {
	if e.playing.Load() != 0 {
		return
	}
	if !e.audioActive.Load() {
		e.startNow()
		return
	}
	e.playReq.Store(1)
}

func (e *Engine) Stop() {
	if e.audioActive.Load() {
		e.stopReq.Store(1)
		return
	}
	e.stopNow(e.samplePos)
}

// startNow 由音频线程（或无声卡时的 UI）调用：把时钟归零到当前样本位置。
func (e *Engine) startNow() {
	e.playing.Store(1)
	e.stepIndex.Store(0)
	e.globalStep = 0
	e.cycleCount = 0
	e.fillNow = 0
	e.songIndex = 0
	e.songRepeat = 0
	e.nextStepAbs = e.samplePos
	e.carryCount = 0 // 丢弃上一轮排程的尾巴，避免“幽灵鼓点”
}

// stopNow 立刻停表：所有 voice 安排 10ms 淡出，排程清空。
func (e *Engine) stopNow(pos int64) {
	e.playing.Store(0)
	for i := 0; i < Pads; i++ {
		for v := 0; v < MaxVoices; v++ {
			e.voices[i][v].kill(pos, 480) // 480 样本 = 10ms
		}
	}
	e.carryCount = 0
	e.stepIndex.Store(0)
}

// RequestPattern 切换 pattern：播放中要等循环边界才生效（规格书 §4）。
func (e *Engine) RequestPattern(idx int) {
	if idx < 0 || idx >= Patterns {
		return
	}
	if e.playing.Load() != 0 {
		e.pendingPattern.Store(int32(idx))
	} else {
		atomic.StoreInt32(&e.proj.Current, int32(idx))
	}
}

// TriggerPad 手动触发一声（点鼓垫 / 键盘 / MIDI）。
func (e *Engine) TriggerPad(pad, vel int, accent bool) {
	if pad < 0 || pad >= Pads {
		return
	}
	vel = clampI(vel, 1, 127)
	when := e.uiPos.Load() + 32 // 32 样本的调度余量，保证不会被当成“过去”
	e.manual.push(evt{t: when, pad: pad, vel: float64(vel) / 127.0, accent: accent})
}

// ExternalPulse 记一次 MIDI 时钟脉冲；音频线程每 6 个脉冲推进一步。
func (e *Engine) ExternalPulse() {
	if e.clockExternal.Load() == 0 {
		return
	}
	e.extPulses.Add(1)
}

// StepLen 计算当前 BPM 下“一个 16 分步”的样本数。
func (e *Engine) StepLen() int {
	bpm := clampF(e.proj.Master.BPM, 40, 300)
	n := int(float64(XUSR)*60.0/bpm/4.0 + 0.5)
	if n < 64 {
		n = 64
	}
	return n
}

// xorshift32：确定性随机，用于概率触发判定。
func (e *Engine) nextRand() uint32 {
	x := e.rng
	x ^= x << 13
	x ^= x >> 17
	x ^= x << 5
	e.rng = x
	return x
}

// condOK 判定条件触发（规格书 §4 的 5 种条件）。
func (e *Engine) condOK(cond uint8) bool {
	switch cond {
	case CondAlways:
		return true
	case CondFill:
		return e.fillNow != 0
	case CondNotFill:
		return e.fillNow == 0
	case CondEvery2:
		return e.cycleCount%2 == 0
	case CondEvery4:
		return e.cycleCount%4 == 0
	}
	return true
}

// emitStep 展开一个 16 分步：swing / micro / ratchet / 概率 / 条件全部在这里定死。
func (e *Engine) emitStep(stepAbs int64, out []evt, count *int) {
	cur := atomic.LoadInt32(&e.proj.Current)
	if cur < 0 || cur >= Patterns {
		cur = 0
	}
	pat := &e.proj.Patterns[cur]
	sl := e.StepLen()

	// swing：奇数 16 分步向后偏移 swing * stepSamples * 0.5
	swingOff := 0
	if e.stepIndex.Load()&1 == 1 {
		swingOff = int(float64(sl) * clampF(e.proj.Master.Swing, 0, 0.75) * 0.5)
	}
	base := stepAbs + int64(swingOff)

	for pad := 0; pad < Pads; pad++ {
		tl := int(pat.TrackLen[pad])
		if tl < 1 {
			tl = 1
		}
		if tl > StepsMax {
			tl = StepsMax
		}
		// polymeter：轨道步号 = 全局步号 % 轨道长度
		idx := int(e.globalStep % int64(tl))
		st := &pat.Grid[pad][idx]
		if st.On == 0 {
			continue
		}
		if !e.condOK(st.Cond) {
			continue
		}
		if st.Prob == 0 {
			continue
		}
		// 概率判定：每个触发时刻只掷一次骰子
		if st.Prob < 100 && int(e.nextRand()%100) >= int(st.Prob) {
			continue
		}

		vel := float64(st.Velocity) / 127.0
		vel = clampF(vel, 0.02, 1.0)
		if st.Accent != 0 {
			vel *= 1.30 // 重音 +35% 音量（合成层另有音色变化）
		}
		vel = clampF(vel, 0, 1.25)

		r := int(st.Ratchet)
		r = clampI(r, 1, 8)
		for i := 0; i < r; i++ {
			t := base + int64(st.Micro) + int64(float64(i)*float64(sl)/float64(r))
			vv := vel * math.Pow(0.92, float64(i)) // 每个子事件递减 8%
			if *count >= len(out) {
				return
			}
			out[*count] = evt{t: t, pad: pad, vel: vv, accent: st.Accent != 0}
			*count++
		}
	}
}

// advanceStep 步进推进：pattern 切换 / 歌曲链只在循环边界发生。
func (e *Engine) advanceStep() {
	cur := atomic.LoadInt32(&e.proj.Current)
	if cur < 0 || cur >= Patterns {
		cur = 0
	}
	pat := &e.proj.Patterns[cur]
	steps := atomic.LoadInt32(&pat.Steps)
	if steps < 1 {
		steps = 1
	}
	e.stepIndex.Add(1)
	e.globalStep++
	if e.stepIndex.Load() < steps {
		return
	}
	// 一个循环结束
	e.stepIndex.Store(0)
	e.cycleCount++
	e.fillNow = 0
	if e.cycleCount%4 == 3 {
		e.fillNow = 1 // 每 4 个循环来一次过门
	}

	// 待切换的 pattern 优先
	if pend := e.pendingPattern.Load(); pend >= 0 {
		n := pend
		if n >= Patterns {
			n = Patterns - 1
		}
		atomic.StoreInt32(&e.proj.Current, n)
		e.pendingPattern.Store(-1)
		e.songRepeat = 0
		return
	}

	// 歌曲链
	if atomic.LoadInt32(&e.proj.Master.SongMode) != 0 && e.proj.SongCount > 0 {
		si := e.songIndex % int(e.proj.SongCount)
		en := &e.proj.Song[si]
		e.songRepeat++
		rep := int(en.Repeats)
		if rep < 1 {
			rep = 1
		}
		if e.songRepeat >= rep {
			e.songRepeat = 0
			e.songIndex = (e.songIndex + 1) % int(e.proj.SongCount)
			np := e.proj.Song[e.songIndex].Pattern
			if np < 0 || np >= Patterns {
				np = 0
			}
			atomic.StoreInt32(&e.proj.Current, np)
		}
	}
}

// audible 判断某声部此刻是否可闻（在混音阶段处理 mute/solo）。
func (e *Engine) audible(pad int) bool {
	if e.soloOverride >= 0 {
		return int32(pad) == e.soloOverride
	}
	anySolo := false
	for i := 0; i < Pads; i++ {
		if atomic.LoadInt32(&e.proj.Pads[i].Solo) != 0 {
			anySolo = true
			break
		}
	}
	p := &e.proj.Pads[pad]
	if atomic.LoadInt32(&p.Mute) != 0 {
		return false
	}
	if anySolo && atomic.LoadInt32(&p.Solo) == 0 {
		return false
	}
	return true
}

// trigger 触发一个 voice，并处理 choke 与 voice stealing。
func (e *Engine) trigger(pad int, when int64, vel float64, accent bool) {
	// 闭镲掐开镲：同属 CHOKE 组 1
	if pad == 15 {
		for i := 0; i < MaxVoices; i++ {
			e.voices[14][i].kill(when, 16)
		}
	}
	// 开镲重触发先掐旧的，避免叠出“嗡嗡”尾巴
	if pad == 14 {
		for i := 0; i < MaxVoices; i++ {
			e.voices[14][i].kill(when, 16)
		}
	}

	target := -1
	for i := 0; i < MaxVoices; i++ {
		if !e.voices[pad][i].active {
			target = i
			break
		}
	}
	if target < 0 {
		// 4 个 voice 全忙：抢占最老的一支
		target = 0
		for i := 1; i < MaxVoices; i++ {
			if e.voices[pad][i].start < e.voices[pad][target].start {
				target = i
			}
		}
		e.voices[pad][target].kill(when, 64)
	}
	e.voices[pad][target].trigger(when, vel, accent)
}

// renderVoices 渲染 [a,b) 区间内的所有可闻 voice。
func (e *Engine) renderVoices(L, R []float32, n int, b0, a, b int64) {
	if b <= a {
		return
	}
	for pad := 0; pad < Pads; pad++ {
		if !e.audible(pad) {
			continue
		}
		p := &e.sm[pad]
		for v := 0; v < MaxVoices; v++ {
			vo := &e.voices[pad][v]
			if vo.active {
				e.renderVoice(vo, L, R, n, b0, p)
			}
		}
	}
}

// smoothPad 按块推进参数平滑（等价于每样本系数 0.002 的一阶低通）。
func (e *Engine) smoothPad(i int, k float64) {
	t := &e.proj.Pads[i]
	s := &e.sm[i]
	s.Tune += (t.Tune - s.Tune) * k
	s.Decay += (t.Decay - s.Decay) * k
	s.Tone += (t.Tone - s.Tone) * k
	s.Snappy += (t.Snappy - s.Snappy) * k
	s.Tail += (t.Tail - s.Tail) * k
	s.Level += (t.Level - s.Level) * k
	s.Pan += (t.Pan - s.Pan) * k
}

// collect 收集本块要处理的事件：carry + 手点 + 音序器。
// 返回的事件已按时间排序，存放在 e.scratch 中；未来事件留在 e.carry。
func (e *Engine) collect(b0, b1 int64) int {
	n := 0
	scratch := e.scratch[:]

	// 1) 上一块遗留的未来事件
	for i := 0; i < e.carryCount; i++ {
		scratch[n] = e.carry[i]
		n++
	}
	e.carryCount = 0

	// 2) UI / MIDI 的手点事件（全部取走，未来时刻的进 carry）
	for n < len(scratch) {
		ev, ok := e.manual.pop()
		if !ok {
			break
		}
		if ev.t < b0 {
			ev.t = b0 // 迟到的触发立即发声，不往后堆
		}
		scratch[n] = ev
		n++
	}

	// 3) 音序器推进：把本块内到点的步全部展开
	if e.playing.Load() != 0 && e.clockExternal.Load() == 0 {
		guard := 0
		for e.nextStepAbs < b1 && guard < 64 {
			guard++
			at := e.nextStepAbs
			if at < b0 {
				at = b0
			}
			e.emitStep(at, scratch, &n)
			e.nextStepAbs += int64(e.StepLen())
			e.advanceStep()
			if n >= len(scratch) {
				break
			}
		}
	} else if e.playing.Load() != 0 {
		// 外部时钟：每 6 个 0xF8 脉冲（24 PPQN）推进一步
		pulses := int(e.extPulses.Swap(0))
		for k := 0; k < pulses; k++ {
			e.clockPulse++
			if e.clockPulse < 6 {
				continue
			}
			e.clockPulse = 0
			e.emitStep(b0, scratch, &n)
			e.advanceStep()
			e.nextStepAbs = e.samplePos + int64(e.StepLen())
			if n >= len(scratch) {
				break
			}
		}
	}

	// 4) 把未来事件挑出来留在 carry，其余留在本块
	m := 0
	for i := 0; i < n; i++ {
		if scratch[i].t < b1 {
			scratch[m] = scratch[i]
			m++
		} else if e.carryCount < len(e.carry) {
			e.carry[e.carryCount] = scratch[i]
			e.carryCount++
		}
	}

	// 5) 按时间插入排序（事件数很小，插入排序最快且无分配）
	for i := 1; i < m; i++ {
		key := scratch[i]
		j := i - 1
		for j >= 0 && scratch[j].t > key.t {
			scratch[j+1] = scratch[j]
			j--
		}
		scratch[j+1] = key
	}
	return m
}

// --------------------------------------------------------------------------
//  Render：整个程序唯一的声音出口。
//  L/R 长度必须 >= n；函数内部只写 [0,n)。
// --------------------------------------------------------------------------
func (e *Engine) Render(n int, L, R []float32) {
	b0 := e.samplePos
	b1 := b0 + int64(n)

	// 控制命令在块首统一消费
	if e.stopReq.Swap(0) != 0 {
		e.stopNow(b0)
	}
	if e.playReq.Swap(0) != 0 {
		e.startNow()
	}

	// 清空输出缓冲
	for i := 0; i < n; i++ {
		L[i] = 0
		R[i] = 0
	}

	// 参数一阶平滑：0.002/样本 换算到整块
	k := 1.0 - math.Pow(0.998, float64(n))
	for i := 0; i < Pads; i++ {
		e.smoothPad(i, k)
	}
	mst := &e.proj.Master
	e.smHpCut += (clampF(mst.HpCutoff, 10, 800) - e.smHpCut) * k
	e.smHpQ += (clampF(mst.HpQ, 0.5, 8) - e.smHpQ) * k
	e.smLpCut += (clampF(mst.LpCutoff, 200, 20000) - e.smLpCut) * k
	e.smLpQ += (clampF(mst.LpQ, 0.5, 8) - e.smLpQ) * k
	e.smDrive += (clampF(mst.Drive, 1, 8) - e.smDrive) * k

	// ---- 事件 → 分段渲染 ----
	cnt := e.collect(b0, b1)
	cur := b0
	for i := 0; i < cnt; i++ {
		ev := &e.scratch[i]
		if ev.t > cur {
			e.renderVoices(L, R, n, b0, cur, ev.t)
			cur = ev.t
		}
		if ev.t < b1 {
			e.trigger(ev.pad, ev.t, ev.vel, ev.accent)
		}
	}
	if cur < b1 {
		e.renderVoices(L, R, n, b0, cur, b1)
	}

	// ---- 主效果链（顺序固定，规格书 §6） ----
	// 3) 高通
	if atomic.LoadInt32(&mst.HpOn) != 0 {
		e.hpL.highpass(e.smHpCut, e.smHpQ)
		e.hpR.highpass(e.smHpCut, e.smHpQ)
		for i := 0; i < n; i++ {
			L[i] = float32(e.hpL.run(float64(L[i])))
			R[i] = float32(e.hpR.run(float64(R[i])))
		}
	}
	// 4) 低通
	if atomic.LoadInt32(&mst.LpOn) != 0 {
		e.lpL.lowpass(e.smLpCut, e.smLpQ)
		e.lpR.lowpass(e.smLpCut, e.smLpQ)
		for i := 0; i < n; i++ {
			L[i] = float32(e.lpL.run(float64(L[i])))
			R[i] = float32(e.lpR.run(float64(R[i])))
		}
	}
	// 5) 过载 → 6) 软限幅 → 7) 主音量
	drive := clampF(e.smDrive, 1, 8)
	norm := math.Tanh(drive)
	volTarget := clampF(mst.Volume, 0, 1)
	if volTarget < 0 {
		volTarget = 0
	}
	pl, pr := 0.0, 0.0
	for i := 0; i < n; i++ {
		xl := float64(L[i])
		xr := float64(R[i])
		if drive > 1.001 {
			xl = math.Tanh(xl*drive) / norm
			xr = math.Tanh(xr*drive) / norm
		}
		xl = xl / (1.0 + math.Abs(xl)) // 软限幅，永不超过 ±1
		xr = xr / (1.0 + math.Abs(xr))
		// 主音量逐样本一阶平滑，拖滑杆不会“咔”
		e.smVol += (volTarget - e.smVol) * 0.002
		xl *= e.smVol
		xr *= e.smVol
		L[i] = float32(xl)
		R[i] = float32(xr)
		if axl := math.Abs(xl); axl > pl {
			pl = axl
		}
		if axr := math.Abs(xr); axr > pr {
			pr = axr
		}
	}

	// 电平表：峰值保持 + 缓慢回落（每块 0.75 倍）
	oldL := math.Float64frombits(e.meterLBits.Load()) * 0.75
	oldR := math.Float64frombits(e.meterRBits.Load()) * 0.75
	if pl < oldL {
		pl = oldL
	}
	if pr < oldR {
		pr = oldR
	}
	e.meterLBits.Store(math.Float64bits(pl))
	e.meterRBits.Store(math.Float64bits(pr))

	e.samplePos = b1
	e.uiPos.Store(b1)

	// 超时回收：超过 4 秒的 voice 无条件释放
	for p := 0; p < Pads; p++ {
		for v := 0; v < MaxVoices; v++ {
			vo := &e.voices[p][v]
			if vo.active && b0-vo.start > 4*int64(XUSR) {
				vo.active = false
			}
		}
	}
}

// TickIdle 在“没有可用音频设备”时由 UI 定时器调用：
// 让播放/停止按钮仍然有反馈，也让手动触发不会积压事件。
func (e *Engine) TickIdle() {
	if e.stopReq.Swap(0) != 0 {
		e.stopNow(e.samplePos)
	}
	if e.playReq.Swap(0) != 0 {
		e.startNow()
	}
}

// SetCPU 记录一次渲染耗时占比（音频线程写，UI 只读）。
func (e *Engine) SetCPU(v float64) {
	e.cpuBits.Store(math.Float64bits(clampF(v, 0, 10)))
}

// CPU 读取 CPU 占比。
func (e *Engine) CPU() float64 {
	return math.Float64frombits(e.cpuBits.Load())
}

// Meters 读取左右电平（0..1）。
func (e *Engine) Meters() (float64, float64) {
	return math.Float64frombits(e.meterLBits.Load()), math.Float64frombits(e.meterRBits.Load())
}
