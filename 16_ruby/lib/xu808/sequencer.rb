# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/sequencer.rb
# 作用：Step / Pattern 数据结构 + 采样级步进音序器。
# 重点：所有触发时刻都用“绝对采样数”表示，swing、micro、ratchet 全部在
#       采样域里算，绝不依赖 UI 定时器。徐某最恨“用 sleep 当时钟”的做法。
# =============================================================================
require_relative 'constants'

module Xu808
  # ---------------------------------------------------------------------------
  # 一个步进格的全部属性（规格第 4 节）。
  # velocity 1–127、probability 0–100、micro 单位是样本、ratchet 1–8。
  # ---------------------------------------------------------------------------
  class Step
    attr_accessor :on, :velocity, :accent, :probability, :micro, :ratchet, :condition

    def initialize(on: false, velocity: 100, accent: false, probability: 100,
                   micro: 0, ratchet: 1, condition: 'ALWAYS')
      @on = on
      @velocity = velocity
      @accent = accent
      @probability = probability
      @micro = micro
      @ratchet = ratchet
      @condition = condition
    end

    # 深拷贝：复制 pattern 时不能只复制引用，否则改一个格全场跟着变。
    def dup_step
      Step.new(on: @on, velocity: @velocity, accent: @accent,
               probability: @probability, micro: @micro,
               ratchet: @ratchet, condition: @condition)
    end

    # 转成 JSON 友好的哈希。键名写全，方便人眼调试工程文件。
    def to_h
      {
        'on' => @on, 'velocity' => @velocity, 'accent' => @accent,
        'probability' => @probability, 'micro' => @micro,
        'ratchet' => @ratchet, 'condition' => @condition
      }
    end

    # 从哈希恢复；缺失字段用默认值，未知字段忽略（前向/后向兼容）。
    def self.from_h(h)
      h ||= {}
      cond = h['condition'].to_s
      cond = 'ALWAYS' unless CONDITIONS.include?(cond)
      new(
        on: h.fetch('on', false) ? true : false,
        velocity: (h.fetch('velocity', 100).to_i).clamp(1, 127),
        accent: h.fetch('accent', false) ? true : false,
        probability: (h.fetch('probability', 100).to_i).clamp(0, 100),
        micro: (h.fetch('micro', 0).to_i).clamp(-2400, 2400),
        ratchet: (h.fetch('ratchet', 1).to_i).clamp(1, 8),
        condition: cond
      )
    end
  end

  # ---------------------------------------------------------------------------
  # Pattern：16 轨 × 最多 64 步，外加每轨独立长度（polymeter）。
  # ---------------------------------------------------------------------------
  class Pattern
    attr_accessor :name, :length, :track_lengths, :steps

    def initialize(length = 16, name = 'PATTERN')
      @name = name
      @length = length.clamp(1, 64)
      # 每轨默认跟着 pattern 总长走；想玩 polymeter 就把某轨改短。
      @track_lengths = Array.new(VOICES.length, @length)
      # 防御式建格：先确保每一格都存在，UI/音序器就不用到处判 nil。
      @steps = Array.new(VOICES.length) do
        Array.new(64) { Step.new }
      end
    end

    # 取某轨某步。轨道步号在外部按 global_step % track_length 算好再传进来。
    def step(track, index)
      @steps[track][index]
    end

    # 修改 pattern 长度时，同步把“仍然跟随总长”的轨道也改掉。
    # 已经被用户单独改短的轨道保持原样，这是徐某对 polymeter 的理解。
    def length=(v)
      nv = v.to_i.clamp(1, 64)
      old = @length
      @track_lengths.each_index do |t|
        @track_lengths[t] = nv if @track_lengths[t] == old
      end
      @length = nv
    end

    def dup_pattern
      p = Pattern.new(@length, @name)
      p.track_lengths = @track_lengths.dup
      p.steps = @steps.map { |row| row.map(&:dup_step) }
      p
    end

    def to_h
      {
        'name' => @name,
        'length' => @length,
        'track_lengths' => @track_lengths,
        'steps' => @steps.map { |row| row.map(&:to_h) }
      }
    end

    def self.from_h(h)
      h ||= {}
      p = new((h['length'] || 16).to_i, h['name'] || 'PATTERN')
      tl = h['track_lengths']
      if tl.is_a?(Array)
        VOICES.each_index do |t|
          p.track_lengths[t] = (tl[t] || p.length).to_i.clamp(1, 64)
        end
      end
      rows = h['steps']
      return p unless rows.is_a?(Array)
      VOICES.each_index do |t|
        row = rows[t]
        next unless row.is_a?(Array)
        row.each_with_index do |sh, s|
          break if s >= 64
          p.steps[t][s] = Step.from_h(sh)
        end
      end
      p
    end
  end

  # ---------------------------------------------------------------------------
  # Sequencer：只负责“什么时候该响”，不负责发声。
  # 它把未来一个音频块内的事件算出来，交给 Engine 在精确样本位置触发。
  # ---------------------------------------------------------------------------
  class Sequencer
    # 对外暴露的只读状态，UI 画播放头 / 显示 pattern 用。
    attr_reader :global_step, :step_in_pattern, :loop_count,
                :display_step, :display_pattern
    attr_accessor :playing, :pattern_index

    def initialize(sr = SAMPLE_RATE)
      @sr = sr.to_f
      @playing = false
      @next_step = Rational(0, 1)      # 用 Rational 累积步长，避免浮点漂移
      @global_step = 0
      @step_in_pattern = 0
      @loop_count = 0
      @pattern_index = 0
      @display_step = 0
      @display_pattern = 0
      @pending_pattern = nil
      @song_pos = 0
      @song_left = 1
      @song_first = true
      @rng = 0x1234_5678               # xorshift32 状态，概率触发用
    end

    # 设置随机种子。工程加载时恢复同一个种子，保证“随机”可复现。
    def seed=(s)
      @rng = (s.to_i | 1) & 0xFFFF_FFFF
    end

    attr_reader :rng

    # 从指定绝对采样帧开始播放。重置步号，但保留当前 pattern 选择。
    def start(frame)
      @playing = true
      @next_step = Rational(frame, 1)
      @global_step = 0
      @step_in_pattern = 0
      @loop_count = 0
      @display_step = 0
      @display_pattern = @pattern_index
      @pending_pattern = nil
      @song_pos = 0
      @song_left = 1
      @song_first = true
    end

    def stop
      @playing = false
    end

    # 请求在下一个步边界切换 pattern。播放中调用不会截断当前步。
    def request_pattern(index)
      @pending_pattern = index.to_i
    end

    # 核心：算出 [block_start, block_end) 内所有要触发的事件。
    # opts = {bpm:, swing:, patterns:, song_mode:, song:, fill:}
    # 返回数组，每项 [绝对采样帧, 轨道号, 力度, 是否重音]。
    def schedule(block_end, opts)
      events = []
      return events unless @playing

      bpm = opts[:bpm].to_f.clamp(40.0, 300.0)
      swing = opts[:swing].to_f.clamp(0.0, 0.75)
      patterns = opts[:patterns]
      return events if patterns.nil? || patterns.empty?

      # 16 分音符长度 = sr * 60 / bpm / 4，用 Rational 累加。
      step_rat = Rational(2_880_000, (bpm * 4.0).round)
      step_f = @sr * 60.0 / (bpm * 4.0)

      # 一个块里可能挤进多个步（极快 BPM + 大步长），所以用 while。
      while @next_step < block_end
        step_start = @next_step.to_i

        # 到步边界才应用 pattern 切换，保证听觉上与 UI 的切换同步。
        if @pending_pattern && @pending_pattern != @pattern_index
          @pattern_index = @pending_pattern
          @step_in_pattern = 0
          @pending_pattern = nil
        end

        pat = patterns[@pattern_index] || patterns[0]
        plen = pat.length

        # 记录 UI 播放头：显示“正在响的这一步”。
        @display_step = @global_step % plen
        @display_pattern = @pattern_index

        # 奇数步（1,3,5…）向后 swing；注意 global_step 从 0 开始，取奇数判断。
        swing_off = (@global_step.odd? ? swing * step_f * 0.5 : 0.0).round

        VOICES.each_index do |t|
          tl = pat.track_lengths[t] || plen
          idx = @global_step % tl
          s = pat.step(t, idx)
          next unless s.on
          next unless condition_pass?(s.condition, opts[:fill])
          next if s.probability < 100 && rand100 >= s.probability

          # 微时偏移：正数延后，负数提前，最后和 swing 相加。
          base = step_start + swing_off + s.micro
          vel = s.velocity.clamp(1, 127)
          n = s.ratchet.clamp(1, 8)
          i = 0
          while i < n
            # ratchet 把一步均分；第 i 个子事件力度递减 8%，听起来像“滚奏”。
            off = (i * step_f / n).round
            v = (vel * (1.0 - i * 0.08)).round.clamp(1, 127)
            events << [base + off, t, v, s.accent]
            i += 1
          end
        end

        # 推进到下一步，并累计步内计数。
        @next_step += step_rat
        @global_step += 1
        @step_in_pattern += 1

        # 到达 pattern 末尾：处理 song / 循环，下一拍重新从第 0 步显示。
        next unless @step_in_pattern >= plen

        @step_in_pattern = 0
        @loop_count += 1
        if opts[:song_mode] && opts[:song] && !opts[:song].empty?
          song = opts[:song]
          if @song_first
            # 第一小节就是 song[0]，这里只结算它的剩余重复次数。
            @song_first = false
            entry = song[0] || {}
            @song_left = [(entry[:repeats] || 1).to_i - 1, 0].max
          else
            @song_left -= 1 if @song_left.positive?
            if @song_left <= 0
              @song_pos = (@song_pos + 1) % song.length
              entry = song[@song_pos] || {}
              @pattern_index = (entry[:pattern] || 0).to_i.clamp(0, patterns.length - 1)
              # 新 entry 从下一小节开始算第 1 遍，所以这里存完整的 repeats；
              # 每过一小节在下一轮循环里减 1，减到 0 才切下一个 entry。
              @song_left = [(entry[:repeats] || 1).to_i, 1].max
            end
          end
        end
      end
      events
    end

    # 条件触发判定。EVERY_2/EVERY_4 按“pattern 循环次数”算，
    # 这样 4 小节过一次门才符合鼓机手感。
    def condition_pass?(cond, fill)
      case cond
      when 'FILL' then fill ? true : false
      when 'NOT_FILL' then fill ? false : true
      when 'EVERY_2' then (@loop_count % 2).zero?
      when 'EVERY_4' then (@loop_count % 4).zero?
      else true
      end
    end

    # xorshift32 取 0–99。音频线程里调用，无分配、无锁。
    def rand100
      x = @rng
      x ^= (x << 13) & 0xFFFF_FFFF
      x ^= (x >> 17)
      x ^= (x << 5) & 0xFFFF_FFFF
      @rng = x & 0xFFFF_FFFF
      @rng % 100
    end
  end
end
