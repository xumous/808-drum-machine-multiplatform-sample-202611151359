# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/midi_in.rb
# 作用：用 winmm midiIn 接收外部 MIDI 键盘/时钟。
# 设计：回调运行在系统 MIDI 线程，只做“解析字节 + 调引擎方法”，不碰 UI。
#       没有 MIDI 设备时 start 返回 false，程序照常运行。
# 支持：
#   - Note On 触发对应声部（按规格第 7 节 GM 音符映射）；
#   - CC7 主音量、CC74 低通截止、CC71 低通 Q；
#   - Program Change 切换 pattern；
#   - 0xFA 开始 / 0xFC 停止；0xF8 时钟用于估算 BPM（每 24 tick 一拍）。
# 徐某说明：不做“采样级从属时钟”，因为 Windows MIDI 时间戳本身有抖动；
#            这里把外部时钟换算成 BPM，让内部采样时钟继续做权威时钟，更稳。
# =============================================================================
require 'fiddle'
require_relative 'constants'
require_relative 'win32'

module Xu808
  class MidiInput
    MIM_DATA = 0x03C3
    CALLBACK_FUNCTION = 0x0003_0000

    attr_reader :last_error

    def initialize(engine)
      @engine = engine
      @handle = nil
      @proc = nil
      @clock_ticks = 0
      @last_beat_ms = nil
      @last_error = nil
    end

    def start
      return true if @handle
      return false if Win32.midiInGetNumDevs.zero?

      # 回调必须常驻，否则被 GC 回收后系统回调会踩空。
      @proc = Fiddle::Closure::BlockCaller.new(
        Fiddle::TYPE_VOID,
        [Fiddle::TYPE_VOIDP, Fiddle::TYPE_INT, Fiddle::TYPE_LONG_LONG,
         Fiddle::TYPE_LONG_LONG, Fiddle::TYPE_LONG_LONG]
      ) do |_hmi, msg, _inst, p1, p2|
        begin
          on_message(msg, p1, p2)
        rescue StandardError => e
          warn "MIDI 回调异常: #{e.message}"
        end
      end

      hptr = Fiddle::Pointer.malloc(8)
      res = Win32.midiInOpen(hptr, 0, @proc.to_i, 0, CALLBACK_FUNCTION)
      if res != 0
        @last_error = format('midiInOpen 失败=0x%08X', res)
        return false
      end
      @handle = hptr[0, 8].unpack1('Q')
      Win32.midiInStart(@handle)
      true
    rescue StandardError => e
      @last_error = e.message
      false
    end

    def stop
      return unless @handle
      begin
        Win32.midiInStop(@handle)
        Win32.midiInClose(@handle)
      rescue StandardError
        nil
      end
      @handle = nil
      @proc = nil
    end

    private

    # dwParam1 的低 3 字节就是 MIDI 消息：status, data1, data2。
    def on_message(msg, p1, p2)
      return unless msg == MIM_DATA
      status = p1 & 0xFF
      d1 = (p1 >> 8) & 0xFF
      d2 = (p1 >> 16) & 0xFF
      case status & 0xF0
      when 0x90
        return if d2.zero?                 # Note On velocity 0 = Note Off
        track = NOTE_TO_TRACK[d1]
        @engine.note_on(track, d2, d2 > 110) if track
      when 0xB0
        case d1
        when 7 then @engine.master_volume = d2 / 127.0
        when 74 then @engine.lp_freq = 200.0 + (d2 / 127.0)**2 * 19_800.0
        when 71 then @engine.lp_q = 0.5 + d2 / 127.0 * 7.5
        end
      when 0xC0
        @engine.select_pattern(d2 % @engine.patterns.length)
      when 0xF0
        case status
        when 0xFA then @engine.play
        when 0xFC then @engine.stop
        when 0xF8 then clock_tick(p2)
        end
      end
    end

    # 每 24 个 F8 = 一个四分音符，用时间差估算 BPM。
    def clock_tick(ms)
      @clock_ticks += 1
      return unless (@clock_ticks % 24).zero?
      now = ms.to_i
      if @last_beat_ms && now > @last_beat_ms
        dt = now - @last_beat_ms
        bpm = 60_000.0 / dt
        @engine.bpm = bpm if bpm >= 40.0 && bpm <= 300.0
      end
      @last_beat_ms = now
    end
  end
end
