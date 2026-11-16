# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/engine.rb
# 作用：把 DSP、音序器、16 轨参数、主效果链拼成一台完整的 808 鼓机引擎。
# 线程模型：
#   - 音频线程只调用 render_block；它自己推进 sample 计数器 @frame；
#   - UI 线程只改 target 参数/pattern，不直接操作 voice；
#   - 两边通过 Ruby 对象读写（MRI 有 GVL，赋值是原子的），没有互斥锁。
# 徐某提醒：本文件里所有时间量都是“样本”，不要混入毫秒或浮点秒。
# =============================================================================
require_relative 'constants'
require_relative 'dsp'
require_relative 'sequencer'

module Xu808
  class Engine
    # UI 需要读取的状态；写操作走下面显式的方法，避免 UI 乱改内部结构。
    attr_reader :sr, :patterns, :sequencer, :frame, :level_l, :level_r,
                :peak, :note_serial
    attr_accessor :bpm, :swing, :master_volume, :drive,
                  :hp_on, :hp_freq, :lp_freq, :lp_q,
                  :fill, :song_mode, :song, :seed, :xrun,
                  :clip_mode

    def initialize(sr = SAMPLE_RATE)
      @sr = sr
      @frame = 0
      @note_serial = 0
      @bpm = 120.0
      @swing = 0.0
      @master_volume = 0.85
      @drive = 2.0
      @hp_on = false
      @hp_freq = 30.0
      @lp_freq = 18_000.0
      @lp_q = 0.707
      @fill = false
      @song_mode = false
      @seed = 0x8080_8080
      @xrun = 0
      @clip_mode = false

      # 16 轨 × 4 voice，一次性建好；播放中不再 new Voice。
      @voices = Array.new(VOICES.length) do |t|
        Array.new(VOICES_PER_TRACK) { |k| Voice.new(@sr, (@seed + t * 97 + k * 13) & 0xFFFF_FFFF) }
      end

      # 当前（平滑后）参数 / 目标参数 / 静音独奏标志。
      @cur_params = Array.new(VOICES.length) { |t| default_params(t) }
      @target_params = Array.new(VOICES.length) { |t| default_params(t) }
      @mute = Array.new(VOICES.length, false)
      @solo = Array.new(VOICES.length, false)

      # 效果器：左右声道各一套高通/低通，状态跨块保留。
      # 徐某特意分成 L/R 两组：如果共用一个滤波器状态，左右声道会互相串扰，
      # 声像一偏就会听到“相位打架”的怪声。
      @fx_hp_l = Biquad.new
      @fx_hp_r = Biquad.new
      @fx_lp_l = Biquad.new
      @fx_lp_r = Biquad.new
      @fx_cur = { hp_freq: @hp_freq, lp_freq: @lp_freq, lp_q: @lp_q,
                  drive: @drive, master: @master_volume }
      @fx_tgt = @fx_cur.dup

      # 预分配混音缓冲，render_block 里只 fill(0) 不 new 数组。
      @buf_l = Array.new(BLOCK_FRAMES, 0.0)
      @buf_r = Array.new(BLOCK_FRAMES, 0.0)
      @level_l = 0.0
      @level_r = 0.0
      @peak = 0.0

      # 音序器和默认工程内容。
      @sequencer = Sequencer.new(@sr)
      @sequencer.seed = @seed
      @patterns = [build_default_pattern(0)]
      @song = [{ pattern: 0, repeats: 4 }]
      @pending_events = []
    end

    # 每轨默认参数：先从 VOICES 里取该声部专属参数，再补齐 level/pan。
    def default_params(track)
      p = { tune: 0.5, decay: 0.5, tone: 0.5, snappy: 0.5, tail: 0.5,
            level: DEFAULT_LEVEL, pan: DEFAULT_PAN }
      VOICES[track][:params].each { |k, v| p[k.to_sym] = v }
      p
    end

    # 默认 pattern：放一个能直接听的 808 基本节奏，打开程序不是一片空白。
    # 注意：这只是初始内容，用户可随意改；selftest 会另建测试 pattern。
    def build_default_pattern(index)
      pat = Pattern.new(16, "PATTERN #{index + 1}")
      # 底鼓：0/4/8/12 四拍，第 0 拍加重音。
      [0, 4, 8, 12].each { |s| pat.step(0, s).on = true }
      pat.step(0, 0).accent = true
      # 军鼓：4/12 反拍。
      pat.step(1, 4).on = true
      pat.step(1, 12).on = true
      # 闭镲：每个 8 分音符，力度稍轻。
      (0...16).step(2) { |s| pat.step(15, s).on = true; pat.step(15, s).velocity = 82 }
      # 开镲：第 15 步，正好被下一拍闭镲掐断，能听出 choke。
      pat.step(14, 15).on = true
      # 拍手：第 12 步补一下。
      pat.step(10, 12).on = true
      # 下面这些“点缀”让 16 个声部都能在默认 pattern 里露面，
      # 打开程序随便导出分轨，不会一半是空文件。
      pat.step(2, 3).on = true   # 低嗵鼓
      pat.step(3, 11).on = true  # 中嗵鼓
      pat.step(4, 14).on = true  # 高嗵鼓
      pat.step(5, 2).on = true   # 低康加
      pat.step(6, 7).on = true   # 中康加
      pat.step(7, 10).on = true  # 高康加
      pat.step(8, 15).on = true  # 边击
      pat.step(9, 9).on = true   # 击木
      (1...16).step(2) { |s| pat.step(11, s).on = true; pat.step(11, s).velocity = 60 } # 沙锤
      pat.step(12, 6).on = true  # 牛铃
      pat.step(13, 0).on = true  # 吊镲，第一拍
      pat
    end

    # ---- 参数读写 ------------------------------------------------------------
    # UI 调参时只改 target；音频线程按块一阶平滑，避免 zipper noise。
    def param(track, key)
      @target_params[track][key.to_sym]
    end

    def set_param(track, key, value)
      k = key.to_sym
      @target_params[track][k] = value.to_f.clamp(0.0, 1.0)
    end

    def param_keys(track)
      VOICES[track][:params].map { |k, _| k.to_sym } + %i[level pan]
    end

    def mute?(track)
      @mute[track]
    end

    def solo?(track)
      @solo[track]
    end

    def toggle_mute(track)
      @mute[track] = !@mute[track]
    end

    def toggle_solo(track)
      @solo[track] = !@solo[track]
    end

    def set_mute(track, v)
      @mute[track] = v ? true : false
    end

    def set_solo(track, v)
      @solo[track] = v ? true : false
    end

    def any_solo?
      @solo.any?
    end

    # ---- Pattern 操作 --------------------------------------------------------
    def pattern
      @patterns[@sequencer.pattern_index] || @patterns[0]
    end

    def current_pattern_index
      @sequencer.pattern_index
    end

    def select_pattern(index)
      i = index.to_i.clamp(0, @patterns.length - 1)
      if @sequencer.playing
        @sequencer.request_pattern(i)  # 播放中在下一步边界切换
      else
        @sequencer.pattern_index = i
      end
    end

    def add_pattern
      @patterns << build_default_pattern(@patterns.length)
      @patterns.length - 1
    end

    # 工程加载时整体替换 pattern 列表，顺带把当前下标夹回合法范围。
    def replace_patterns(pats)
      @patterns = pats
      @patterns = [build_default_pattern(0)] if @patterns.empty?
      @sequencer.pattern_index = @sequencer.pattern_index.clamp(0, @patterns.length - 1)
    end

    # 返回目标参数的深拷贝，保存工程用（存用户想要的音色，不是平滑中间值）。
    def params_snapshot
      @target_params.map(&:dup)
    end

    # 载入某轨参数，缺失键保留默认值。
    def set_params(track, h)
      return unless h.is_a?(Hash)
      h.each do |k, v|
        key = k.to_sym
        next unless @target_params[track].key?(key)
        @target_params[track][key] = v.to_f.clamp(0.0, 1.0)
        @cur_params[track][key] = @target_params[track][key]
      end
    end

    # ---- 播放控制 ------------------------------------------------------------
    def playing?
      @sequencer.playing
    end

    # 是否真的在发声：音序器在跑，或还有余音没衰减完。
    # 音频后端用它决定要不要统计 xrun，空闲时不算，避免 UI 重绘背锅。
    def busy?
      return true if @sequencer.playing
      @voices.each do |row|
        row.each { |v| return true if v.active }
      end
      false
    end

    def play
      @pending_events.clear
      @sequencer.seed = @seed
      # Song 模式从链条第一项开始，而不是从 UI 当前选中的 pattern 开始。
      if @song_mode && !@song.empty?
        entry = @song[0] || {}
        @sequencer.pattern_index = (entry[:pattern] || 0).to_i.clamp(0, @patterns.length - 1)
      end
      @sequencer.start(@frame)
    end

    def stop
      @sequencer.stop
      # 停止后不立刻杀掉 voice：让余音自然衰减，符合“上一个音过后要停”的要求。
    end

    def toggle_play
      playing? ? stop : play
    end

    # Tap Tempo：记录最近 8 次点击时刻，去掉最大/最小间隔后取平均。
    # 间隔换算成 BPM，超出 40–300 的异常值直接丢弃（手抖/双击）。
    def tap!
      @tap_times ||= []
      now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      # 超过 3 秒没点就重新开一组，避免把上一段的节奏混进来。
      @tap_times = [] if !@tap_times.empty? && (now - @tap_times.last) > 3.0
      @tap_times << now
      @tap_times.shift while @tap_times.length > 8
      return @bpm if @tap_times.length < 2
      intervals = []
      i = 1
      while i < @tap_times.length
        dt = @tap_times[i] - @tap_times[i - 1]
        bpm = 60.0 / dt
        intervals << dt if bpm >= 30.0 && bpm <= 400.0
        i += 1
      end
      return @bpm if intervals.empty?
      if intervals.length >= 3
        intervals.delete_at(intervals.index(intervals.max))
        intervals.delete_at(intervals.index(intervals.min))
      end
      avg = intervals.sum / intervals.length
      @bpm = (60.0 / avg).clamp(40.0, 300.0)
      @bpm.round(1)
    end

    # 紧急静音：所有 voice 立即释放（切换工程或退出时用）。
    def panic
      @voices.each { |row| row.each(&:kill!) }
      @pending_events.clear
    end

    # 所有 voice 快速淡出（关闭窗口时比 panic 更不“咔”）。
    def release_all
      @voices.each do |row|
        row.each { |v| v.choke! if v.active }
      end
    end

    # 供 UI 点击声部名试听。
    def trigger(track, velocity = 110, accent = false)
      note_on(track, velocity, accent)
    end

    # 真正触发一个 voice，包含 choke group 与 voice stealing。
    def note_on(track, velocity = 100, accent = false)
      t = track.to_i.clamp(0, VOICES.length - 1)
      code = VOICES[t][:code]

      # choke group 1：闭镲掐开镲/吊镲，开镲反过来掐闭镲。
      case code
      when 'CH'
        choke_tracks(%w[OH CY])
      when 'OH'
        choke_tracks(%w[CH])
      end

      row = @voices[t]
      # 先找空闲 voice；4 个都忙就抢最老的那个（voice stealing）。
      slot = row.find { |v| !v.active }
      slot ||= row.min_by(&:started_at)
      @note_serial += 1
      slot.trigger(code, @cur_params[t], accent, @note_serial, velocity)
      slot
    end

    # 掐断指定代号的声部。
    def choke_tracks(codes)
      codes.each do |code|
        t = VOICE_INDEX[code]
        next unless t
        @voices[t].each { |v| v.choke! if v.active }
      end
    end

    # ---- 音频渲染 ------------------------------------------------------------
    # 返回 self；调用者读 last_buf_l / last_buf_r 或直接取 mix 数组。
    def render_block(frames = BLOCK_FRAMES)
      frames = frames.to_i
      return self if frames <= 0
      grow_buffers(frames) if frames > @buf_l.length

      block_start = @frame
      block_end = block_start + frames

      smooth_params(frames)
      update_fx_coeffs

      # 1) 让音序器算出本块内的触发事件（可能在块外，先存 pending）。
      if @sequencer.playing
        new_events = @sequencer.schedule(block_end,
                                         bpm: @bpm, swing: @swing,
                                         patterns: @patterns,
                                         song_mode: @song_mode,
                                         song: @song,
                                         fill: @fill)
        @pending_events.concat(new_events) unless new_events.empty?
      end

      # 2) 把本块要处理的事件挑出来，剩下的留到下一块。
      due = []
      if @pending_events.empty?
        # 快速路径：没有事件就不用分配临时数组。
      else
        keep = []
        @pending_events.each do |e|
          if e[0] < block_end
            due << e
          else
            keep << e
          end
        end
        @pending_events = keep
        due.sort_by! { |e| e[0] }
      end

      # 3) 清空混音缓冲（fill 复用同一数组，不 new）。
      @buf_l.fill(0.0, 0, frames)
      @buf_r.fill(0.0, 0, frames)

      # 4) 按事件时刻切段渲染，保证事件落点误差为 0 个样本。
      ei = 0
      pos = 0
      while pos < frames
        while ei < due.length && due[ei][0] <= block_start + pos
          e = due[ei]
          note_on(e[1], e[2], e[3])
          ei += 1
        end
        nxt = ei < due.length ? (due[ei][0] - block_start) : frames
        nxt = frames if nxt > frames || nxt < 0
        nxt = pos + 1 if nxt <= pos
        render_segment(pos, nxt - pos)
        pos = nxt
      end

      # 5) 主效果链 + 电平表。
      apply_fx(frames)
      @frame = block_end
      self
    end

    def last_buf_l
      @buf_l
    end

    def last_buf_r
      @buf_r
    end

    # 供离线渲染：把两个声道打包成 16bit PCM 字符串。
    def block_pcm(frames)
      out = Array.new(frames * 2)
      i = 0
      while i < frames
        out[i * 2] = (@buf_l[i] * 32_767.0).round.clamp(-32_768, 32_767)
        out[i * 2 + 1] = (@buf_r[i] * 32_767.0).round.clamp(-32_768, 32_767)
        i += 1
      end
      out.pack('s<*')
    end

    # 离线渲染 seconds 秒，返回 [pcm 字符串, 峰值]。
    # 复用与实时完全相同的 render_block，所以导出不会“变味”。
    def render_seconds(seconds, block = BLOCK_FRAMES)
      total = (seconds.to_f * @sr).round
      chunks = +''
      peak = 0.0
      done = 0
      while done < total
        n = [block, total - done].min
        render_block(n)
        chunks << block_pcm(n)
        peak = @peak if @peak > peak
        done += n
      end
      [chunks, peak]
    end

    private

    # 缓冲区不够就扩容（正常 512 永远够，离线用别的块长时才需要）。
    def grow_buffers(frames)
      @buf_l = Array.new(frames, 0.0)
      @buf_r = Array.new(frames, 0.0)
    end

    # 参数一阶平滑：每块算一次等效系数，等价于逐样本 0.002 的指数接近。
    # 公式：alpha_block = 1 - (1 - 0.002)^frames。
    def smooth_params(frames)
      a = 1.0 - (0.998**frames)
      t = 0
      while t < VOICES.length
        cur = @cur_params[t]
        tgt = @target_params[t]
        cur.each_key do |k|
          cur[k] += (tgt[k] - cur[k]) * a
        end
        t += 1
      end
      # 主效果参数同样平滑，拖滑杆时不会“啪”一声。
      @fx_tgt[:drive] = @drive
      @fx_tgt[:master] = @master_volume
      @fx_tgt[:hp_freq] = @hp_freq
      @fx_tgt[:lp_freq] = @lp_freq
      @fx_tgt[:lp_q] = @lp_q
      @fx_cur.each_key do |k|
        @fx_cur[k] += (@fx_tgt[k] - @fx_cur[k]) * a
      end
    end

    # 每块更新一次双二阶系数。系数平滑后，即使扫频也不会爆音。
    def update_fx_coeffs
      @fx_hp_l.set_highpass(@fx_cur[:hp_freq], 0.707, @sr)
      @fx_hp_r.set_highpass(@fx_cur[:hp_freq], 0.707, @sr)
      @fx_lp_l.set_lowpass(@fx_cur[:lp_freq], @fx_cur[:lp_q], @sr)
      @fx_lp_r.set_lowpass(@fx_cur[:lp_freq], @fx_cur[:lp_q], @sr)
      @drive_norm = 1.0 / Math.tanh(@fx_cur[:drive].clamp(1.0, 8.0))
    end

    # 渲染 [off, off+len) 这一段：所有轨道的所有活动 voice 直接叠加。
    def render_segment(off, len)
      any_solo = @solo.any?
      t = 0
      while t < VOICES.length
        g = if any_solo
              @solo[t] ? 1.0 : 0.0
            else
              @mute[t] ? 0.0 : 1.0
            end
        if g > 0.0
          row = @voices[t]
          k = 0
          while k < row.length
            v = row[k]
            v.render_into(@buf_l, @buf_r, off, len, g, g) if v.active
            k += 1
          end
        end
        t += 1
      end
    end

    # 主效果链：HP -> LP -> tanh 过载 -> 软限幅 -> 主音量。
    # 性能关键：把双二阶滤波内联成局部变量运算，避免每样本两次方法调用。
    def apply_fx(frames)
      hp_on = @hp_on
      hb0 = @fx_hp_l.b0; hb1 = @fx_hp_l.b1; hb2 = @fx_hp_l.b2
      ha1 = @fx_hp_l.a1; ha2 = @fx_hp_l.a2
      hz1l = @fx_hp_l.z1; hz2l = @fx_hp_l.z2
      hz1r = @fx_hp_r.z1; hz2r = @fx_hp_r.z2
      lb0 = @fx_lp_l.b0; lb1 = @fx_lp_l.b1; lb2 = @fx_lp_l.b2
      la1 = @fx_lp_l.a1; la2 = @fx_lp_l.a2
      lz1l = @fx_lp_l.z1; lz2l = @fx_lp_l.z2
      lz1r = @fx_lp_r.z1; lz2r = @fx_lp_r.z2
      drive = @fx_cur[:drive].clamp(1.0, 8.0)
      dn = @drive_norm
      master = @fx_cur[:master].clamp(0.0, 1.0)
      bl = @buf_l
      br = @buf_r
      pk = 0.0
      i = 0
      while i < frames
        xl = bl[i]
        xr = br[i]
        if hp_on
          # 左声道高通
          yl = hb0 * xl + hz1l
          hz1l = hb1 * xl - ha1 * yl + hz2l
          hz2l = hb2 * xl - ha2 * yl
          xl = yl
          # 右声道高通
          yr = hb0 * xr + hz1r
          hz1r = hb1 * xr - ha1 * yr + hz2r
          hz2r = hb2 * xr - ha2 * yr
          xr = yr
        end
        # 左声道低通
        yl = lb0 * xl + lz1l
        lz1l = lb1 * xl - la1 * yl + lz2l
        lz2l = lb2 * xl - la2 * yl
        xl = yl
        # 右声道低通
        yr = lb0 * xr + lz1r
        lz1r = lb1 * xr - la1 * yr + lz2r
        lz2r = lb2 * xr - la2 * yr
        xr = yr
        # tanh 过载：drive 越大谐波越丰富，归一化保证 0dB 输入不被无限放大。
        xl = Math.tanh(xl * drive) * dn
        xr = Math.tanh(xr * drive) * dn
        # 软限幅 x/(1+|x|)：永远不会超过 ±1，且过渡平滑。
        xl = xl / (1.0 + xl.abs)
        xr = xr / (1.0 + xr.abs)
        xl *= master
        xr *= master
        bl[i] = xl
        br[i] = xr
        al = xl.abs
        ar = xr.abs
        pk = al if al > pk
        pk = ar if ar > pk
        i += 1
      end
      @fx_hp_l.z1 = hz1l; @fx_hp_l.z2 = hz2l
      @fx_hp_r.z1 = hz1r; @fx_hp_r.z2 = hz2r
      @fx_lp_l.z1 = lz1l; @fx_lp_l.z2 = lz2l
      @fx_lp_r.z1 = lz1r; @fx_lp_r.z2 = lz2r
      @peak = pk
      # 电平表带下落：上升立刻跟上，下降按 0.82 指数回落。
      @level_l = pk > @level_l ? pk : @level_l * 0.82
      @level_r = pk > @level_r ? pk : @level_r * 0.82
      @clip_mode = pk > 0.999
    end
  end
end
