# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/selftest.rb
# 作用：无人值守自检。必须在没有声卡、没有窗口的环境里也能跑完。
# 检查项：
#   1) 16 个声部逐一触发，都能发出非静音声音；
#   2) 16 声部同时触发不产生 NaN、不削波失控；
#   3) 开镲会被闭镲掐断（choke group）；
#   4) 同一轨连打 8 次，活动 voice 不超过 4（voice stealing）；
#   5) 底鼓衰减完会自动释放，不会“响完不关”；
#   6) 音序器 5 秒离线渲染出非空 WAV，且打点数量符合 BPM；
#   7) MIDI 导出有正确 MThd 头；
#   8) 工程 JSON 保存/加载往返一致。
# 任何一项失败都会写 FAIL 并以退出码 1 结束，绝不假装成功。
# =============================================================================
require_relative 'constants'
require_relative 'engine'
require_relative 'wav'
require_relative 'midi'
require_relative 'project'

module Xu808
  module SelfTest
    module_function

    def run(out_dir = ROOT)
      lines = []
      ok = true
      # 小工具：记录一行结果；&&= 保证只要有一条 FAIL，整体就 FAIL。
      check = lambda do |cond, msg|
        lines << (cond ? "OK   #{msg}" : "FAIL #{msg}")
        ok = false unless cond
      end

      lines << "XU-808 Ruby SelfTest  Ruby #{RUBY_VERSION}  #{RUBY_PLATFORM}"

      # ---- 1. 16 声部逐一发声 ------------------------------------------------
      VOICES.each_index do |t|
        e = Engine.new
        e.note_on(t, 127, true)
        peak = 0.0
        16.times do
          e.render_block(512)
          peak = e.peak if e.peak > peak
        end
        code = VOICES[t][:code]
        check.call(peak > 0.004 && !peak.nan?, format('%s 单声部峰值=%.4f', code, peak))
      end

      # ---- 2. 16 声部同时触发 ------------------------------------------------
      e_all = Engine.new
      VOICES.each_index { |t| e_all.note_on(t, 110, t.zero?) }
      peak_all = 0.0
      16.times do
        e_all.render_block(512)
        peak_all = e_all.peak if e_all.peak > peak_all
      end
      check.call(peak_all > 0.05 && peak_all <= 1.0 && !peak_all.nan?,
                 format('16 声部齐奏峰值=%.4f（软限幅内）', peak_all))

      # ---- 3. choke：闭镲掐开镲 ---------------------------------------------
      e_choke = Engine.new
      e_choke.note_on(14, 120, false)          # OH
      e_choke.render_block(512)
      oh_before = e_choke.instance_variable_get(:@voices)[14].count(&:active)
      e_choke.note_on(15, 120, false)          # CH
      4.times { e_choke.render_block(512) }
      oh_after = e_choke.instance_variable_get(:@voices)[14].count(&:active)
      check.call(oh_before.positive? && oh_after.zero?,
                 "闭镲 choke 开镲：之前 #{oh_before} 个活动，之后 #{oh_after} 个")

      # ---- 4. voice stealing ------------------------------------------------
      e_steal = Engine.new
      8.times { e_steal.note_on(0, 100, false) }
      active_n = e_steal.instance_variable_get(:@voices)[0].count(&:active)
      check.call(active_n <= VOICES_PER_TRACK,
                 "同轨连打 8 次，活动 voice=#{active_n}（上限 #{VOICES_PER_TRACK}）")

      # ---- 5. 自然衰减后释放 -------------------------------------------------
      e_decay = Engine.new
      e_decay.note_on(0, 127, false)
      decay_done = false
      # 3 秒（约 281 个块）足够让默认底鼓彻底衰减。
      282.times do
        e_decay.render_block(512)
        if e_decay.instance_variable_get(:@voices)[0].none?(&:active)
          decay_done = true
          break
        end
      end
      check.call(decay_done, '底鼓自然衰减后 voice 自动释放')

      # ---- 5b. Song 链 repeats ------------------------------------------------
      e_song = Engine.new
      e_song.replace_patterns([Pattern.new(2, 'A'), Pattern.new(2, 'B')])
      e_song.song = [{ pattern: 0, repeats: 2 }, { pattern: 1, repeats: 1 }]
      e_song.song_mode = true
      e_song.play
      bars = []
      6.times do
        e_song.render_block(6000)           # 每小节第 1 步
        bars << e_song.sequencer.pattern_index
        e_song.render_block(6000)           # 每小节第 2 步
      end
      check.call(bars == [0, 0, 1, 0, 0, 1], "Song 链 bars=#{bars.inspect}（期望 A,A,B,A,A,B）")

      # ---- 6. 音序器离线渲染 WAV --------------------------------------------
      e = Engine.new
      pat = Pattern.new(16, 'SELFTEST')
      # 每个声部在第 0 步各敲一下，保证 16 个 voice 都进入 WAV。
      VOICES.each_index do |t|
        st = pat.step(t, 0)
        st.on = true
        st.velocity = (70 + t * 3).clamp(1, 127)
      end
      # 底鼓每步都响，提供连续的低频能量，方便看波形。
      (0...16).each { |s| pat.step(0, s).on = true }
      pat.step(0, 8).accent = true
      # 拍手 ratchet=3，验证一步内多个子事件。
      pat.step(10, 4).on = true
      pat.step(10, 4).ratchet = 3
      # 沙锤 50% 概率，验证随机触发不会崩。
      pat.step(11, 6).on = true
      pat.step(11, 6).probability = 50
      # 击木 +300 样本微时偏移，验证采样级延后。
      pat.step(9, 8).on = true
      pat.step(9, 8).micro = 300
      # 闭镲在第 14 步，正好再掐一次开镲。
      pat.step(15, 14).on = true
      pat.step(14, 2).on = true
      e.replace_patterns([pat])
      e.bpm = 120.0
      e.swing = 0.20
      e.seed = 0x1234_5678
      e.sequencer.seed = e.seed
      e.play

      pcm = +''
      peak_seq = 0.0
      total = 240_000                 # 5 秒
      done = 0
      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      while done < total
        n = [BLOCK_FRAMES, total - done].min
        e.render_block(n)
        pcm << e.block_pcm(n)
        peak_seq = e.peak if e.peak > peak_seq
        done += n
      end
      render_secs = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0

      wav_path = File.join(out_dir, 'selftest_ruby.wav')
      Wav.write(wav_path, pcm)
      wav_size = File.size(wav_path)
      expected = 44 + total * 4
      nonzero = pcm.each_byte.count { |b| b != 0 }
      check.call(wav_size == expected, "WAV 大小=#{wav_size}（期望 #{expected}）")
      check.call(nonzero > 10_000, "WAV 非静音字节数=#{nonzero}")
      check.call(peak_seq > 0.05, format('音序器渲染峰值=%.4f', peak_seq))
      steps = e.sequencer.global_step
      check.call(steps.between?(39, 41), "5 秒 @120BPM 走了 #{steps} 步（期望 40）")
      check.call(render_secs.positive?, format('离线渲染 5 秒耗时 %.2fs（%.1fx 实时）',
                                               render_secs, 5.0 / render_secs))

      # ---- 7. MIDI 导出 ------------------------------------------------------
      mid_path = File.join(out_dir, 'selftest_ruby.mid')
      Midi.export(e, mid_path, pattern_index: 0)
      mid = File.binread(mid_path)
      check.call(mid.bytesize > 100 && mid[0, 4] == 'MThd',
                 "MIDI 导出=#{mid.bytesize} 字节，MThd 头正确")

      # ---- 8. 工程保存/加载 --------------------------------------------------
      proj_path = File.join(out_dir, 'selftest_ruby_project.json')
      Project.save(e, proj_path)
      e2 = Project.load(proj_path)
      same_bpm = (e2.bpm - e.bpm).abs < 0.001
      same_step = e2.patterns[0].step(0, 0).on == true
      check.call(same_bpm && same_step, "工程 JSON 往返：bpm=#{e2.bpm}, 第0轨第0步=#{e2.patterns[0].step(0, 0).on}")

      # ---- 汇总 --------------------------------------------------------------
      lines << format('WAV=%s (%d 字节)', wav_path, wav_size)
      lines << format('MIDI=%s (%d 字节)', mid_path, mid.bytesize)
      puts lines.join("\n")

      if ok
        puts 'SELFTEST OK'
        File.write(File.join(out_dir, 'selftest_ruby.txt'),
                   "SELFTEST OK\n" + lines.join("\n") + "\n")
        0
      else
        puts 'SELFTEST FAIL'
        File.write(File.join(out_dir, 'selftest_ruby.txt'),
                   "SELFTEST FAIL\n" + lines.join("\n") + "\n")
        1
      end
    end
  end
end
