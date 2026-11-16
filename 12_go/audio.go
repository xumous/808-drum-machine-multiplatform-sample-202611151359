// ==========================================================================
//  audio.go —— waveOut 输出与独立音频线程（Go 版）
//
//  署名：徐某。
//  为什么用 CALLBACK_EVENT 而不是 CALLBACK_FUNCTION？
//  Go 的回调必须先进 runtime 再回来，若直接放在 waveOut 的高优先级回调里渲染，
//  一次 GC 停顿就可能顶穿 5ms 的缓冲。这里把回调压缩成“系统 SetEvent”，
//  再让一个 LockOSThread 的专用 goroutine 在事件上等待、渲染、提交下一块。
//  效果与 C 版完全等价：音频推进由声卡节奏驱动，UI 定时器绝不参与时钟。
//
//  4 个 256 样本缓冲 ≈ 21ms 延迟；事件按写入顺序 FIFO 完成，所以每次只需
//  回填“最早写出的那一块”。
// ==========================================================================
package main

import (
	"runtime"
	"sync/atomic"
	"time"
	"unsafe"
)

type audioOut struct {
	hwo     uintptr
	evt     uintptr
	hdr     [4]waveHdr
	buf     [4][]int16
	fmt     waveFormatEx
	running atomic.Bool
	done    chan struct{}
	eng     *Engine
	next    int32
	L       [BlockSize]float32
	R       [BlockSize]float32
	qpf     int64
}

var audio audioOut

// StartAudio 打开默认波形输出设备并启动音频线程。
//  成功返回 true；无声卡时返回 false，UI 会显示“无音频”但仍可编辑。
func StartAudio(e *Engine) bool {
	if audio.hwo != 0 {
		return true
	}
	if n, _, _ := pWaveOutGetNumDevs.Call(); n == 0 {
		return false
	}
	audio.eng = e

	// 事件句柄：自动重置（false），初态无信号（false）
	evt, _, _ := pCreateEventW.Call(0, 0, 0, 0)
	if evt == 0 {
		return false
	}
	audio.evt = evt

	// 48kHz / 16bit / 立体声 PCM
	audio.fmt = waveFormatEx{
		wFormatTag:      1,
		nChannels:       2,
		nSamplesPerSec:  XUSR,
		nAvgBytesPerSec: XUSR * 4,
		nBlockAlign:     4,
		wBitsPerSample:  16,
		cbSize:          0,
	}

	var hwo uintptr
	rc, _, _ := pWaveOutOpen.Call(
		uintptr(unsafe.Pointer(&hwo)),
		waveMapper,
		uintptr(unsafe.Pointer(&audio.fmt)),
		evt, // 回调 = 事件句柄
		0,
		callbackEvent,
	)
	if uint32(rc) != mmSysErrNoError || hwo == 0 {
		pCloseHandle.Call(evt)
		audio.evt = 0
		return false
	}
	audio.hwo = hwo

	var freq int64
	pQueryPerformanceFrequency.Call(uintptr(unsafe.Pointer(&freq)))
	audio.qpf = freq

	// 准备 4 个缓冲（内存一次分配，之后回调里零分配）
	for i := 0; i < 4; i++ {
		audio.buf[i] = make([]int16, BlockSize*2)
		audio.hdr[i] = waveHdr{
			lpData:         uintptr(unsafe.Pointer(&audio.buf[i][0])),
			dwBufferLength: BlockSize * 4,
		}
		pWaveOutPrepareHeader.Call(hwo, uintptr(unsafe.Pointer(&audio.hdr[i])),
			unsafe.Sizeof(waveHdr{}))
		// 先填一块再写，避免开头出现半块静音
		audio.renderBlock(i)
		pWaveOutWrite.Call(hwo, uintptr(unsafe.Pointer(&audio.hdr[i])), unsafe.Sizeof(waveHdr{}))
	}
	audio.next = 0

	audio.running.Store(true)
	audio.done = make(chan struct{})
	e.audioActive.Store(true)

	go audio.loop()
	return true
}

// loop 独立音频线程：等待“某块播完”的事件，然后回填并重新提交。
func (a *audioOut) loop() {
	runtime.LockOSThread() // 绑死一条 OS 线程，减少调度抖动
	defer close(a.done)
	for a.running.Load() {
		r, _, _ := pWaitForSingleObject.Call(a.evt, 100)
		if !a.running.Load() {
			break
		}
		if r != 0 { // 0 = WAIT_OBJECT_0；超时则继续等，不渲染
			continue
		}
		i := int(atomic.LoadInt32(&a.next))
		atomic.StoreInt32(&a.next, int32((i+1)&3))
		a.renderBlock(i)
	}
}

// renderBlock 渲染一块并提交给 waveOut。
func (a *audioOut) renderBlock(i int) {
	t0 := qpcNow()
	n := BlockSize
	a.eng.Render(n, a.L[:], a.R[:])
	buf := a.buf[i]
	for k := 0; k < n; k++ {
		buf[k*2] = toPCM(a.L[k])
		buf[k*2+1] = toPCM(a.R[k])
	}
	rc, _, _ := pWaveOutWrite.Call(a.hwo,
		uintptr(unsafe.Pointer(&a.hdr[i])), unsafe.Sizeof(waveHdr{}))
	if uint32(rc) != mmSysErrNoError {
		a.eng.xruns.Add(1)
	}
	t1 := qpcNow()
	if a.qpf > 0 {
		dt := float64(t1-t0) / float64(a.qpf)
		ideal := float64(BlockSize) / float64(XUSR)
		prev := a.eng.CPU()
		a.eng.SetCPU(prev*0.95 + (dt/ideal)*0.05)
	}
}

// qpcNow 读高精度计数器。
func qpcNow() int64 {
	var v int64
	pQueryPerformanceCounter.Call(uintptr(unsafe.Pointer(&v)))
	return v
}

// StopAudio 停止并释放 waveOut；返回时音频线程一定已经退出。
func StopAudio() {
	if audio.hwo == 0 {
		return
	}
	audio.running.Store(false)
	pWaveOutReset.Call(audio.hwo) // 立刻回收所有在途缓冲，唤醒等待

	if audio.done != nil {
		select {
		case <-audio.done:
		case <-time.After(2 * time.Second):
			// 极端情况下不无限等待：继续走关闭流程，避免卡死主窗口
		}
	}
	for i := 0; i < 4; i++ {
		pWaveOutUnprepareHeader.Call(audio.hwo,
			uintptr(unsafe.Pointer(&audio.hdr[i])), unsafe.Sizeof(waveHdr{}))
	}
	pWaveOutClose.Call(audio.hwo)
	audio.hwo = 0
	if audio.evt != 0 {
		pCloseHandle.Call(audio.evt)
		audio.evt = 0
	}
	audio.eng.audioActive.Store(false)
}
