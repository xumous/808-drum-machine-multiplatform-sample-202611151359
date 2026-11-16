package main

import (
	"testing"
	"time"
)

// 临时基准：渲染一块 256 样本需要多少时间；实时预算是 5.333ms。
func BenchmarkRender(b *testing.B) {
	p := NewProject()
	e := NewEngine(p)
	e.startNow()
	L := make([]float32, BlockSize)
	R := make([]float32, BlockSize)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		e.Render(BlockSize, L, R)
	}
}

// 临时诊断：统计一段播放里同时活跃的 voice 数量与每块耗时。
func TestPerfInfo(t *testing.T) {
	p := NewProject()
	e := NewEngine(p)
	e.startNow()
	L := make([]float32, BlockSize)
	R := make([]float32, BlockSize)
	total, max := 0, 0
	const n = 1000
	start := time.Now()
	for i := 0; i < n; i++ {
		e.Render(BlockSize, L, R)
		c := 0
		for pd := 0; pd < Pads; pd++ {
			for v := 0; v < MaxVoices; v++ {
				if e.voices[pd][v].active {
					c++
				}
			}
		}
		total += c
		if c > max {
			max = c
		}
	}
	el := time.Since(start)
	t.Logf("blocks=%d elapsed=%v perBlock=%v avgVoices=%.1f maxVoices=%d",
		n, el, el/time.Duration(n), float64(total)/float64(n), max)

	// 空转（不播放）基线：只看主效果链的成本
	e2 := NewEngine(p)
	start = time.Now()
	for i := 0; i < n; i++ {
		e2.Render(BlockSize, L, R)
	}
	el2 := time.Since(start)
	t.Logf("idle perBlock=%v", el2/time.Duration(n))
}
