# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/midi.rb
# 作用：把 pattern 导出成 SMF Format 1（PPQ=480）的 .mid 文件。
# 规格第 7 节：16 分音符 = 120 tick，微时偏移 ±24 tick 内保留，ratchet 展开。
# 徐某写 MIDI 时最怕变长量（VLQ）写错——一个字节错位，整个文件就废了。
# =============================================================================
require_relative 'constants'

module Xu808
  module Midi
    module_function

    # MIDI 变长量：每字节 7 位，最高位表示“后面还有”。
    # 例如 0x400 要写成 0x88 0x00。必须从低到高取 7 位再反转。
    def vlq(value)
      v = value.to_i
      v = 0 if v < 0
      bytes = [v & 0x7F]
      v >>= 7
      while v.positive?
        bytes << ((v & 0x7F) | 0x80)
        v >>= 7
      end
      bytes.reverse.pack('C*')
    end

    # 16 分音符的 tick 数（PPQ 480 / 4）。
    TICKS_PER_STEP = 120
    NOTE_LEN = 10

    # 导出 pattern。path 为输出文件，pattern_index 默认当前 pattern。
    def export(engine, path, pattern_index: nil)
      pat = engine.patterns[(pattern_index || engine.current_pattern_index)] || engine.patterns[0]
      bpm = engine.bpm.to_f.clamp(40.0, 300.0)
      # micro 是“样本”，换算成 tick：micro * bpm / 6000（推导见 README）。
      events = []
      s = 0
      while s < pat.length
        VOICES.each_index do |t|
          step = pat.step(t, s % (pat.track_lengths[t] || pat.length))
          next unless step.on
          # 条件/概率是实时演奏语义，导出时一律照谱面写出，不掷骰子，
          # 否则同一个工程每次导出的 MIDI 都不一样，DAW 里没法复现。
          tick_off = (step.micro * bpm / 6000.0).round.clamp(-24, 24)
          base_tick = s * TICKS_PER_STEP + tick_off
          vel = (step.velocity * (step.accent ? 1.3 : 1.0)).round.clamp(1, 127)
          n = step.ratchet.clamp(1, 8)
          i = 0
          while i < n
            tk = base_tick + (i * TICKS_PER_STEP / n)
            note = VOICES[t][:note]
            # 同 tick 上先 note-off（排序键 0）后 note-on（排序键 1），
            # 避免新音刚触发就被同一 tick 的旧 note-off 掐掉。
            events << [tk, 1, 0x90, note, vel]
            events << [tk + NOTE_LEN, 0, 0x80, note, 0]
            i += 1
          end
        end
        s += 1
      end
      events.sort_by! { |e| [e[0], e[1]] }

      # 第一轨：只放速度与拍号元事件。字符串强制 BINARY，
      # 否则 UTF-8 默认编码和二进制字节混拼会抛 Encoding::CompatibilityError。
      conductor = String.new(encoding: Encoding::BINARY)
      conductor << vlq(0) << "\xFF\x51\x03".b
      us = (60_000_000.0 / bpm).round
      conductor << [(us >> 16) & 0xFF, (us >> 8) & 0xFF, us & 0xFF].pack('C3')
      conductor << vlq(0) << "\xFF\x58\x04".b << [4, 2, 24, 8].pack('C4')
      conductor << vlq(0) << "\xFF\x2F\x00".b

      # 第二轨：全部鼓音符。轨道名用 ASCII，避免某些 DAW 把 UTF-8 中文显示成乱码。
      notes = String.new(encoding: Encoding::BINARY)
      last = 0
      name = "XU-808 #{pat.name} by Xu"
      notes << vlq(0) << "\xFF\x03".b << vlq(name.bytesize) << name.b
      events.each do |e|
        delta = e[0] - last
        last = e[0]
        notes << vlq(delta)
        notes << [e[2], e[3], e[4]].pack('C3')
      end
      notes << vlq(0) << "\xFF\x2F\x00".b

      # 文件头与两个 MTrk 块全部用 BINARY 拼接，避免任何编码协商。
      data = String.new(encoding: Encoding::BINARY)
      data << 'MThd'.b << [6, 1, 2, 480].pack('Nnnn')
      data << 'MTrk'.b << [conductor.bytesize].pack('N') << conductor
      data << 'MTrk'.b << [notes.bytesize].pack('N') << notes
      File.binwrite(path, data)
      path
    end
  end
end
