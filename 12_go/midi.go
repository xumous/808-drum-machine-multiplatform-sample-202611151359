// ==========================================================================
//  midi.go —— MIDI 输入（winmm midiIn，Go 版）
//
//  署名：徐某。
//  MIDI 的到达频率很低（最多几百条/秒），因此这里用一把轻量互斥锁把事件
//  抄进内存队列，UI 定时器每 33ms 取走一次再分派 —— 音频线程完全不碰锁。
//  Note On / CC7 / CC71 / CC74 / Program Change / 0xF8 时钟 / 0xFA / 0xFC
//  的处理与规格书 §8 一致。
// ==========================================================================
package main

import (
	"sync"
	"syscall"
	"unsafe"
)

// midiEvent 一条三字节 MIDI 消息（短消息够用，SysEx 直接忽略）。
type midiEvent struct {
	st, d1, d2 byte
}

var (
	midiMu      sync.Mutex
	midiQueue   []midiEvent
	midiHandle  uintptr
	midiCbOnce  bool
	midiCbPtr   uintptr
)

// midiCallback 由 winmm 的系统线程调用，只做“入队”这一件小事。
func midiCallback(hmi, msg, inst, p1, p2 uintptr) uintptr {
	if uint32(msg) != 0x3C3 { // MIM_DATA
		return 0
	}
	ev := midiEvent{
		st: byte(p1 & 0xFF),
		d1: byte((p1 >> 8) & 0xFF),
		d2: byte((p1 >> 16) & 0xFF),
	}
	midiMu.Lock()
	if len(midiQueue) < 1024 {
		midiQueue = append(midiQueue, ev)
	}
	midiMu.Unlock()
	return 0
}

// StartMIDI 打开 0 号 MIDI 输入设备；没有设备时安静返回 false。
func StartMIDI() bool {
	if midiHandle != 0 {
		return true
	}
	if n, _, _ := pMidiInGetNumDevs.Call(); n == 0 {
		return false
	}
	if !midiCbOnce {
		midiCbPtr = syscall.NewCallback(midiCallback)
		midiCbOnce = true
	}
	var h uintptr
	rc, _, _ := pMidiInOpen.Call(
		uintptr(unsafe.Pointer(&h)),
		0,
		midiCbPtr,
		0,
		callbackFunc,
	)
	if uint32(rc) != mmSysErrNoError || h == 0 {
		return false
	}
	pMidiInStart.Call(h)
	midiHandle = h
	return true
}

// StopMIDI 关闭 MIDI 输入。
func StopMIDI() {
	if midiHandle == 0 {
		return
	}
	pMidiInStop.Call(midiHandle)
	pMidiInReset.Call(midiHandle)
	pMidiInClose.Call(midiHandle)
	midiHandle = 0
}

// TakeMIDIEvents 取走当前排队的全部事件（UI 线程调用）。
func TakeMIDIEvents() []midiEvent {
	midiMu.Lock()
	if len(midiQueue) == 0 {
		midiMu.Unlock()
		return nil
	}
	out := midiQueue
	midiQueue = nil
	midiMu.Unlock()
	return out
}
