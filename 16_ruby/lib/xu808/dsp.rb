# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/dsp.rb
# 作用：XU-808 的模拟建模 DSP 核心——16 种鼓声、包络、噪声、滤波器。
# 设计原则（徐某反复强调）：
#   1) 全部实时合成，绝不播放预录采样，所以 tune/decay/tone 能真正改变音色；
#   2) 每个 voice 自己带 xorshift 噪声种子和相位，互不干扰；
#   3) 所有衰减都用指数包络，衰减到阈值就自动 active=false，绝不“响完不关”；
#   4) 热循环里只用局部变量和查表，避免在音频线程里 new 对象。
#
# 性能说明：Ruby 没有 JIT 时逐样本解释执行很贵，所以下面每个 render_* 都：
#   - 把 @ivars 先搬到局部变量（局部变量访问比实例变量快）；
#   - 相位用 0..4095 的整数索引，避免每样本浮点乘 4096 再 to_i；
#   - 噪声 xorshift 内联，省掉一次方法调用；
#   - 滤波器状态也搬到局部变量，循环结束再写回。
# 徐某为此把每个声部都手写了一遍 while 循环，虽然啰嗦，但能跑实时。
# =============================================================================
require_relative 'constants'

module Xu808
  # ---------------------------------------------------------------------------
  # RBJ Audio EQ Cookbook 双二阶滤波器。
  # 为什么用它？——公式公开、稳定、每样本只要 5 次乘加，Ruby 也扛得住。
  # 采用“转置直接 II 型”，状态只有 z1/z2 两个，切换系数时不容易爆音。
  # ---------------------------------------------------------------------------
  class Biquad
    attr_reader :b0, :b1, :b2, :a1, :a2
    # z1/z2 暴露出来，是为了让 voice 的热循环内联滤波、少一次方法调用。
    attr_accessor :z1, :z2

    # 新建时先把状态清零。徐某说：滤波器最怕带着上一次的脏状态出生。
    def initialize
      @b0 = 1.0
      @b1 = 0.0
      @b2 = 0.0
      @a1 = 0.0
      @a2 = 0.0
      @z1 = 0.0
      @z2 = 0.0
    end

    # 清空内部延迟。换音色/重新触发 voice 时调用，防止旧尾音串进来。
    def reset
      @z1 = 0.0
      @z2 = 0.0
    end

    # 低通：fc 截止频率，q 品质因数，sr 采样率。
    # 这一行行抄的是 RBJ 公式，徐某对着草稿纸核过三遍，错一个符号就会变“电话音”。
    def set_lowpass(fc, q, sr)
      fc = 20.0 if fc < 20.0                       # 低于 20Hz 没有意义，还会让系数病态
      fc = sr * 0.45 if fc > sr * 0.45             # 逼近奈奎斯特会不稳定，留 5% 余量
      w0 = TAU * fc / sr                           # 数字角频率
      cw = Math.cos(w0)                            # 余弦
      sw = Math.sin(w0)                            # 正弦
      alpha = sw / (2.0 * q)                       # Q 决定的带宽
      a0 = 1.0 + alpha                             # 归一化分母
      @b0 = (1.0 - cw) * 0.5 / a0                  # b0
      @b1 = (1.0 - cw) / a0                        # b1
      @b2 = @b0                                    # 低通 b2 与 b0 相同
      @a1 = -2.0 * cw / a0                         # a1
      @a2 = (1.0 - alpha) / a0                     # a2
      self
    end

    # 高通：和低通同一套公式，只是分子换成 (1+cos)/2。
    def set_highpass(fc, q, sr)
      fc = 20.0 if fc < 20.0
      fc = sr * 0.45 if fc > sr * 0.45
      w0 = TAU * fc / sr
      cw = Math.cos(w0)
      sw = Math.sin(w0)
      alpha = sw / (2.0 * q)
      a0 = 1.0 + alpha
      @b0 = (1.0 + cw) * 0.5 / a0
      @b1 = -(1.0 + cw) / a0
      @b2 = @b0
      @a1 = -2.0 * cw / a0
      @a2 = (1.0 - alpha) / a0
      self
    end

    # 带通（峰值增益 0dB 的 constant skirt 版本）。
    # 军鼓噪声、拍手、牛铃都要它把能量聚到目标频段。
    def set_bandpass(fc, q, sr)
      fc = 20.0 if fc < 20.0
      fc = sr * 0.45 if fc > sr * 0.45
      w0 = TAU * fc / sr
      cw = Math.cos(w0)
      sw = Math.sin(w0)
      alpha = sw / (2.0 * q)
      a0 = 1.0 + alpha
      @b0 = alpha / a0
      @b1 = 0.0
      @b2 = -alpha / a0
      @a1 = -2.0 * cw / a0
      @a2 = (1.0 - alpha) / a0
      self
    end

    # 处理一个样本。转置直接 II 型：
    #   y  = b0*x + z1
    #   z1 = b1*x - a1*y + z2
    #   z2 = b2*x - a2*y
    # 只要 5 乘 4 加，徐某在纸上画过信号流图，确认没有多算。
    def process(x)
      y = @b0 * x + @z1
      @z1 = @b1 * x - @a1 * y + @z2
      @z2 = @b2 * x - @a2 * y
      y
    end
  end

  # ---------------------------------------------------------------------------
  # 单个鼓声 voice。一个对象反复复用，触发时 reset/trigger，衰减完 active=false。
  # 这样音频线程不会在播放过程中 new Voice，避免 GC 抖动。
  # ---------------------------------------------------------------------------
  class Voice
    attr_reader :code, :started_at
    attr_accessor :active

    # 构造时只做一次内存分配：相位数组、滤波器、噪声种子。
    def initialize(sr, seed)
      @sr = sr.to_f
      @inv_sr = 1.0 / @sr
      @active = false
      @code = 'BD'
      @started_at = 0
      @ph = Array.new(6, 0)            # 6 个振荡器相位（整数 0..4095）
      @inc = Array.new(6, 0)           # 对应相位增量（整数）
      @noise = (seed | 1) & 0xFFFF_FFFF # xorshift32 种子，保证非 0
      @f_bp = Biquad.new               # 带通
      @f_hp = Biquad.new               # 高通
      @f_lp = Biquad.new               # 低通（部分声部给噪声整形）
      @burst_env = Array.new(4, 0.0)   # 拍手的 4 段爆发包络
      @burst_times = Array.new(4, 0)
      reset_state
    end

    # 把一次触发要用到的状态全部归零。注意：不重新 new 对象。
    def reset_state
      @t = 0
      @env = 1.0
      @env_dec = 0.999
      @amp = 0.75
      @pan_l = 0.7071
      @pan_r = 0.7071
      @click_env = 0.0
      @click_dec = 0.99
      @click_amp = 0.0
      @pe = 1.0
      @pe_dec = 0.999
      @f_start = 100.0
      @f_end = 50.0
      @tone_mix = 1.0
      @noise_mix = 0.0
      @tail_env = 0.0
      @tail_dec = 0.999
      @tail_amt = 0.0
      @burst_idx = 0
      @burst_dec = 0.99
      @f_bp.reset
      @f_hp.reset
      @f_lp.reset
      @ph.fill(0)
      @burst_env.fill(0.0)
    end

    # 触发一次。参数 p 是已经平滑过的 0..1 哈希；accent 为 true 时同时改变
    # 音量、包络长度和音色（规格第 4 节要求，不能只乘增益）。
    def trigger(code, p, accent, serial, velocity = 100)
      reset_state
      @code = code
      @active = true
      @started_at = serial
      # 等功率声像：pan=0 全左，pan=1 全右，中间两路都是 0.707。
      pan = p[:pan] || DEFAULT_PAN
      ang = pan * Math::PI * 0.5
      @pan_l = Math.cos(ang)
      @pan_r = Math.sin(ang)
      # 力度映射：velocity 1 不会完全没声，保留 30% 底噪，手感更接近真鼓机。
      vel_gain = 0.30 + 0.70 * (velocity.to_f.clamp(1.0, 127.0) / 127.0)
      # 力度/重音：重音 +35% 幅度，同时下面各声部会加长衰减、抬高起始音高。
      accel = accent ? 1.35 : 1.0
      @amp = (p[:level] || DEFAULT_LEVEL) * accel * vel_gain
      # 参数取值都做下限保护，防止 UI 传进 nil 或 0 把 DSP 搞崩。
      tune = (p[:tune] || 0.5).to_f
      decay = (p[:decay] || 0.5).to_f
      tone = (p[:tone] || 0.5).to_f
      snappy = (p[:snappy] || 0.5).to_f
      tail = (p[:tail] || 0.5).to_f
      # 重音让包络长 20%，这样“重”不只是响，还会更“厚”。
      decay *= 1.2 if accent

      case code
      when 'BD'
        # 底鼓：基频 40–80Hz，起始音高是终点的 3–5 倍，45ms 左右滑下去。
        @f_end = 40.0 + 40.0 * tune
        @f_start = @f_end * (3.0 + 2.0 * tone)
        @pe_dec = Math.exp(-1.0 / (0.045 * @sr))
        @env_dec = env_coef(0.12 + 1.88 * decay)
        # 点击噪声：tone 越大越“啪”，衰减只有 4ms。
        @click_amp = 0.05 + 0.30 * tone
        @click_env = 1.0
        @click_dec = Math.exp(-1.0 / (0.004 * @sr))
      when 'SD'
        # 军鼓：两个正弦 180/330Hz，tune 把整体乘 0.6–1.4。
        sc = 0.6 + 0.8 * tune
        @f_start = 180.0 * sc
        @f_end = 330.0 * sc
        @tone_mix = 1.0 - 0.35 * snappy
        @noise_mix = 0.35 + 0.75 * snappy
        @f_bp.set_bandpass(1200.0 + 3800.0 * snappy, 1.1, @sr)
        @env_dec = env_coef(0.08 + 0.42 * decay)
      when 'LT', 'MT', 'HT'
        # 嗵鼓：80/120/170Hz 三个基频，起始高 1.7 倍，60ms 滑落。
        base = code == 'LT' ? 80.0 : (code == 'MT' ? 120.0 : 170.0)
        @f_end = base * (0.6 + 1.0 * tune)
        @f_start = @f_end * 1.7
        @pe_dec = Math.exp(-1.0 / (0.060 * @sr))
        @env_dec = env_coef(0.15 + 0.75 * decay)
        @click_amp = 0.04 + 0.22 * tone
        @click_env = 1.0
        @click_dec = Math.exp(-1.0 / (0.006 * @sr))
      when 'LC', 'MC', 'HC'
        # 康加：220/300/400Hz，起始只高 1.25 倍，木味更重。
        base = code == 'LC' ? 220.0 : (code == 'MC' ? 300.0 : 400.0)
        @f_end = base * (0.6 + 1.0 * tune)
        @f_start = @f_end * 1.25
        @pe_dec = Math.exp(-1.0 / (0.050 * @sr))
        @env_dec = env_coef(0.10 + 0.60 * decay)
        @click_amp = 0.02
        @click_env = 1.0
        @click_dec = Math.exp(-1.0 / (0.004 * @sr))
      when 'RS'
        # 边击：1.2k–2.6kHz 的短脉冲，外加 3kHz 带通噪声。
        @f_start = 1200.0 + 1400.0 * tune
        @f_bp.set_bandpass(3000.0, 1.4, @sr)
        @env_dec = env_coef(0.020 + 0.100 * decay)
      when 'CL'
        # 击木：2.5kHz 正弦，30ms，极干净。
        @f_start = 1800.0 + 1400.0 * tune
        @env_dec = env_coef(0.030 + 0.050 * decay)
      when 'CP'
        # 拍手：4 段爆发（0/10/20/30ms），每段 7ms 衰减，之后接尾巴噪声。
        @f_bp.set_bandpass(800.0 + 1700.0 * tone, 1.3, @sr)
        @burst_times[0] = 0
        @burst_times[1] = (0.010 * @sr).to_i
        @burst_times[2] = (0.020 * @sr).to_i
        @burst_times[3] = (0.030 * @sr).to_i
        @burst_dec = Math.exp(-1.0 / (0.007 * @sr))
        @tail_dec = env_coef(0.10 + 0.50 * decay)
        @tail_amt = 0.55 * tail
        @env_dec = env_coef(0.10 + 0.50 * decay) # 给外部 choke 用
      when 'MA'
        # 沙锤：高通噪声，4k–9kHz。
        @f_hp.set_highpass(4000.0 + 5000.0 * tone, 0.8, @sr)
        @env_dec = env_coef(0.020 + 0.130 * decay)
      when 'CB'
        # 牛铃：540/800Hz 双方波，带通到 1.2kHz 附近。
        sc = 0.8 + 0.4 * tune
        @f_start = 540.0 * sc
        @f_end = 800.0 * sc
        @f_bp.set_bandpass(1200.0, 2.8, @sr)
        @env_dec = env_coef(0.08 + 0.72 * decay)
      when 'CY', 'OH', 'CH'
        # 金属簇：6 个非谐方波，经高通留下“呲呲”的金属感。
        base_t = 5200.0 + 2600.0 * tone
        @inc[0] = (base_t * 1.0000 * 4096.0 * @inv_sr).to_i
        @inc[1] = (base_t * 1.3423 * 4096.0 * @inv_sr).to_i
        @inc[2] = (base_t * 1.2312 * 4096.0 * @inv_sr).to_i
        @inc[3] = (base_t * 1.6532 * 4096.0 * @inv_sr).to_i
        @inc[4] = (base_t * 1.9523 * 4096.0 * @inv_sr).to_i
        @inc[5] = (base_t * 2.1523 * 4096.0 * @inv_sr).to_i
        @f_hp.set_highpass(6000.0, 0.7, @sr)
        # 三种镲的衰减范围不同：CH 最短、OH 中等、CY 最长。
        dt = case code
             when 'CH' then 0.03 + 0.22 * decay
             when 'OH' then 0.10 + 0.70 * decay
             else 0.20 + 1.80 * decay
             end
        @env_dec = env_coef(dt)
      end
      self
    end

    # 指数衰减系数：让包络在 time 秒内衰减到 -60dB（约 1/1000）。
    # 徐某用 ln(1000)=6.9078，这个数背下来能省一次 Math.log。
    def env_coef(time)
      time = 0.005 if time < 0.005
      Math.exp(-6.9078 / (time * @sr))
    end

    # 闭镲掐断开镲：不是立刻静音（会“咔”），而是 6ms 快速淡出再释放。
    def choke!
      return unless @active
      @env_dec = Math.exp(-6.9078 / (0.006 * @sr))
      @env = 0.75 if @env > 0.75
    end

    # 立即释放（停止播放/工程切换时用）。
    def kill!
      @active = false
      @env = 0.0
    end

    # 生成噪声：xorshift32 是 Marsaglia 的经典移位算法，周期 2^32-1，
    # 不用任何数组，三行异或移位，Ruby 里也够快。
    def noise_white
      x = @noise
      x ^= (x << 13) & 0xFFFF_FFFF
      x ^= (x >> 17)
      x ^= (x << 5) & 0xFFFF_FFFF
      @noise = x & 0xFFFF_FFFF
      ((@noise >> 8) * (1.0 / 8_388_608.0)) - 1.0
    end

    # 把本 voice 的声音叠加到左右缓冲区。
    # 关键：每个类型单独一个 while 循环，把 case 提到循环外，
    # 这样 512 个样本里不会反复判断 code，Ruby 的解释开销能少一大截。
    def render_into(l, r, off, n, gl, gr)
      return unless @active
      case @code
      when 'BD' then render_bd(l, r, off, n, gl, gr)
      when 'SD' then render_sd(l, r, off, n, gl, gr)
      when 'LT', 'MT', 'HT' then render_tom(l, r, off, n, gl, gr)
      when 'LC', 'MC', 'HC' then render_conga(l, r, off, n, gl, gr)
      when 'RS' then render_rim(l, r, off, n, gl, gr)
      when 'CL' then render_claves(l, r, off, n, gl, gr)
      when 'CP' then render_clap(l, r, off, n, gl, gr)
      when 'MA' then render_maracas(l, r, off, n, gl, gr)
      when 'CB' then render_cowbell(l, r, off, n, gl, gr)
      else render_metal(l, r, off, n, gl, gr)
      end
    end

    # ---- 底鼓 ----------------------------------------------------------------
    def render_bd(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      pe = @pe
      ce = @click_env
      ph = @ph[0]
      env_dec = @env_dec
      pe_dec = @pe_dec
      ce_dec = @click_dec
      f_end = @f_end
      f_span = @f_start - @f_end
      amp = @amp
      ca = @click_amp
      ns = @noise
      inv = @inv_sr
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        inc = ((f_end + f_span * pe) * 4096.0 * inv).to_i
        ph = (ph + inc) & 4095
        s = tbl[ph]
        # xorshift32 内联，避免每样本一次方法调用
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = ((ns >> 8) * (1.0 / 8_388_608.0) - 1.0) * ca * ce
        v = (s * env + nz) * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        pe *= pe_dec
        ce *= ce_dec
        @t += 1
        i += 1
      end
      @env = env
      @pe = pe
      @click_env = ce
      @ph[0] = ph
      @noise = ns
      @active = false if env <= 0.0008
    end

    # ---- 军鼓 ----------------------------------------------------------------
    def render_sd(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      ph0 = @ph[0]
      ph1 = @ph[1]
      inc0 = (@f_start * 4096.0 * @inv_sr).to_i
      inc1 = (@f_end * 4096.0 * @inv_sr).to_i
      tm = @tone_mix
      nm = @noise_mix
      amp = @amp
      env_dec = @env_dec
      ns = @noise
      b0 = @f_bp.b0; b1 = @f_bp.b1; b2 = @f_bp.b2
      a1 = @f_bp.a1; a2 = @f_bp.a2
      z1 = @f_bp.z1; z2 = @f_bp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        ph0 = (ph0 + inc0) & 4095
        ph1 = (ph1 + inc1) & 4095
        tone = tbl[ph0] + 0.7 * tbl[ph1]
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = (ns >> 8) * (1.0 / 8_388_608.0) - 1.0
        y = b0 * nz + z1
        z1 = b1 * nz - a1 * y + z2
        z2 = b2 * nz - a2 * y
        v = (tone * tm + y * nm) * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @ph[0] = ph0
      @ph[1] = ph1
      @noise = ns
      @f_bp.z1 = z1
      @f_bp.z2 = z2
      @active = false if env <= 0.0008
    end

    # ---- 嗵鼓 ----------------------------------------------------------------
    def render_tom(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      pe = @pe
      ce = @click_env
      ph = @ph[0]
      env_dec = @env_dec
      pe_dec = @pe_dec
      ce_dec = @click_dec
      f_end = @f_end
      f_span = @f_start - @f_end
      amp = @amp
      ca = @click_amp
      ns = @noise
      inv = @inv_sr
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        inc = ((f_end + f_span * pe) * 4096.0 * inv).to_i
        ph = (ph + inc) & 4095
        s = tbl[ph]
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = ((ns >> 8) * (1.0 / 8_388_608.0) - 1.0) * ca * ce
        v = (s * env + nz) * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        pe *= pe_dec
        ce *= ce_dec
        @t += 1
        i += 1
      end
      @env = env
      @pe = pe
      @click_env = ce
      @ph[0] = ph
      @noise = ns
      @active = false if env <= 0.0008
    end

    # ---- 康加 ----------------------------------------------------------------
    def render_conga(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      pe = @pe
      ce = @click_env
      ph = @ph[0]
      env_dec = @env_dec
      pe_dec = @pe_dec
      ce_dec = @click_dec
      f_end = @f_end
      f_span = @f_start - @f_end
      amp = @amp
      ca = @click_amp
      ns = @noise
      inv = @inv_sr
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        inc = ((f_end + f_span * pe) * 4096.0 * inv).to_i
        ph = (ph + inc) & 4095
        s = tbl[ph]
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = ((ns >> 8) * (1.0 / 8_388_608.0) - 1.0) * ca * ce
        v = (s * 0.95 + nz) * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        pe *= pe_dec
        ce *= ce_dec
        @t += 1
        i += 1
      end
      @env = env
      @pe = pe
      @click_env = ce
      @ph[0] = ph
      @noise = ns
      @active = false if env <= 0.0008
    end

    # ---- 边击 ----------------------------------------------------------------
    def render_rim(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      ph = @ph[0]
      inc = (@f_start * 4096.0 * @inv_sr).to_i
      amp = @amp
      env_dec = @env_dec
      ns = @noise
      b0 = @f_bp.b0; b1 = @f_bp.b1; b2 = @f_bp.b2
      a1 = @f_bp.a1; a2 = @f_bp.a2
      z1 = @f_bp.z1; z2 = @f_bp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        ph = (ph + inc) & 4095
        s = tbl[ph]
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = (ns >> 8) * (1.0 / 8_388_608.0) - 1.0
        y = b0 * nz + z1
        z1 = b1 * nz - a1 * y + z2
        z2 = b2 * nz - a2 * y
        v = (s * 0.8 + y * 0.55) * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @ph[0] = ph
      @noise = ns
      @f_bp.z1 = z1
      @f_bp.z2 = z2
      @active = false if env <= 0.0008
    end

    # ---- 击木 ----------------------------------------------------------------
    def render_claves(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      ph = @ph[0]
      inc = (@f_start * 4096.0 * @inv_sr).to_i
      amp = @amp
      env_dec = @env_dec
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        ph = (ph + inc) & 4095
        v = tbl[ph] * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @ph[0] = ph
      @active = false if env <= 0.0008
    end

    # ---- 拍手 ----------------------------------------------------------------
    def render_clap(l, r, off, n, gl, gr)
      i = 0
      e0 = @burst_env[0]; e1 = @burst_env[1]
      e2 = @burst_env[2]; e3 = @burst_env[3]
      be = @burst_dec
      te = @tail_env
      td = @tail_dec
      ta = @tail_amt
      idx = @burst_idx
      times0 = @burst_times[0]; times1 = @burst_times[1]
      times2 = @burst_times[2]; times3 = @burst_times[3]
      amp = @amp
      t = @t
      ns = @noise
      b0 = @f_bp.b0; b1 = @f_bp.b1; b2 = @f_bp.b2
      a1 = @f_bp.a1; a2 = @f_bp.a2
      z1 = @f_bp.z1; z2 = @f_bp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n
        # 到点就点燃一段爆发；4 段错开 10ms 才有“拍手”而不是“放屁”。
        if idx < 4
          if idx.zero? && t >= times0
            e0 = 1.0; idx = 1
          end
          if idx == 1 && t >= times1
            e1 = 1.0; idx = 2
          end
          if idx == 2 && t >= times2
            e2 = 1.0; idx = 3
          end
          if idx == 3 && t >= times3
            e3 = 1.0; te = 1.0; idx = 4
          end
        end
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = (ns >> 8) * (1.0 / 8_388_608.0) - 1.0
        y = b0 * nz + z1
        z1 = b1 * nz - a1 * y + z2
        z2 = b2 * nz - a2 * y
        v = y * ((e0 + e1 + e2 + e3) * 0.30 + te * ta) * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        e0 *= be; e1 *= be; e2 *= be; e3 *= be
        te *= td
        t += 1
        i += 1
        if idx >= 4 && te <= 0.0008
          @active = false
          break
        end
      end
      @burst_env[0] = e0; @burst_env[1] = e1
      @burst_env[2] = e2; @burst_env[3] = e3
      @tail_env = te
      @burst_idx = idx
      @t = t
      @noise = ns
      @f_bp.z1 = z1
      @f_bp.z2 = z2
    end

    # ---- 沙锤 ----------------------------------------------------------------
    def render_maracas(l, r, off, n, gl, gr)
      i = 0
      env = @env
      amp = @amp
      env_dec = @env_dec
      ns = @noise
      b0 = @f_hp.b0; b1 = @f_hp.b1; b2 = @f_hp.b2
      a1 = @f_hp.a1; a2 = @f_hp.a2
      z1 = @f_hp.z1; z2 = @f_hp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = (ns >> 8) * (1.0 / 8_388_608.0) - 1.0
        y = b0 * nz + z1
        z1 = b1 * nz - a1 * y + z2
        z2 = b2 * nz - a2 * y
        v = y * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @noise = ns
      @f_hp.z1 = z1
      @f_hp.z2 = z2
      @active = false if env <= 0.0008
    end

    # ---- 牛铃 ----------------------------------------------------------------
    def render_cowbell(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      ph0 = @ph[0]
      ph1 = @ph[1]
      inc0 = (@f_start * 4096.0 * @inv_sr).to_i
      inc1 = (@f_end * 4096.0 * @inv_sr).to_i
      amp = @amp
      env_dec = @env_dec
      b0 = @f_bp.b0; b1 = @f_bp.b1; b2 = @f_bp.b2
      a1 = @f_bp.a1; a2 = @f_bp.a2
      z1 = @f_bp.z1; z2 = @f_bp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        ph0 = (ph0 + inc0) & 4095
        ph1 = (ph1 + inc1) & 4095
        # 方波：正弦查表值 >= 0 就取 +1，否则 -1，比另建方波表省内存。
        sq1 = tbl[ph0] >= 0.0 ? 0.6 : -0.6
        sq2 = tbl[ph1] >= 0.0 ? 0.5 : -0.5
        x = sq1 + sq2
        y = b0 * x + z1
        z1 = b1 * x - a1 * y + z2
        z2 = b2 * x - a2 * y
        v = y * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @ph[0] = ph0
      @ph[1] = ph1
      @f_bp.z1 = z1
      @f_bp.z2 = z2
      @active = false if env <= 0.0008
    end

    # ---- 吊镲 / 开镲 / 闭镲 ---------------------------------------------------
    def render_metal(l, r, off, n, gl, gr)
      tbl = SINE_TABLE
      i = 0
      env = @env
      p0 = @ph[0]; p1 = @ph[1]; p2 = @ph[2]
      p3 = @ph[3]; p4 = @ph[4]; p5 = @ph[5]
      i0 = @inc[0]; i1 = @inc[1]; i2 = @inc[2]
      i3 = @inc[3]; i4 = @inc[4]; i5 = @inc[5]
      amp = @amp
      env_dec = @env_dec
      ns = @noise
      b0 = @f_hp.b0; b1 = @f_hp.b1; b2 = @f_hp.b2
      a1 = @f_hp.a1; a2 = @f_hp.a2
      z1 = @f_hp.z1; z2 = @f_hp.z2
      pl = @pan_l * gl
      pr = @pan_r * gr
      while i < n && env > 0.0008
        # 6 个非谐方波叠加，这是 808 金属声的核心配方。
        s = 0.0
        p0 = (p0 + i0) & 4095
        s += tbl[p0] >= 0.0 ? 1.0 : -1.0
        p1 = (p1 + i1) & 4095
        s += tbl[p1] >= 0.0 ? 1.0 : -1.0
        p2 = (p2 + i2) & 4095
        s += tbl[p2] >= 0.0 ? 1.0 : -1.0
        p3 = (p3 + i3) & 4095
        s += tbl[p3] >= 0.0 ? 1.0 : -1.0
        p4 = (p4 + i4) & 4095
        s += tbl[p4] >= 0.0 ? 1.0 : -1.0
        p5 = (p5 + i5) & 4095
        s += tbl[p5] >= 0.0 ? 1.0 : -1.0
        s *= 0.16
        # 方波簇过一次高通
        hp = b0 * s + z1
        z1 = b1 * s - a1 * hp + z2
        z2 = b2 * s - a2 * hp
        # 再让白噪声走同一个滤波器（线性滤波，等价于把两者相加后过滤），
        # 多出来的“沙沙”声让镲片更真实。
        ns ^= (ns << 13) & 0xFFFF_FFFF
        ns ^= (ns >> 17)
        ns ^= (ns << 5) & 0xFFFF_FFFF
        ns &= 0xFFFF_FFFF
        nz = (ns >> 8) * (1.0 / 8_388_608.0) - 1.0
        hn = b0 * nz + z1
        z1 = b1 * nz - a1 * hn + z2
        z2 = b2 * nz - a2 * hn
        v = (hp + hn * 0.20) * env * amp
        l[off + i] += v * pl
        r[off + i] += v * pr
        env *= env_dec
        @t += 1
        i += 1
      end
      @env = env
      @ph[0] = p0; @ph[1] = p1; @ph[2] = p2
      @ph[3] = p3; @ph[4] = p4; @ph[5] = p5
      @noise = ns
      @f_hp.z1 = z1
      @f_hp.z2 = z2
      @active = false if env <= 0.0008
    end
  end
end
