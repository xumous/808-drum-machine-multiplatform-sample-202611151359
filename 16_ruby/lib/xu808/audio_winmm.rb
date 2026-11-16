# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/audio_winmm.rb
# 作用：用 winmm 的 waveOut 把引擎渲染的 16bit 立体声送到声卡。
# 线程模型（与规格第 1 节一致）：
#   - 独立 Ruby 音频线程，主时钟是引擎自己的 @frame 采样计数；
#   - 6 个预分配缓冲轮转，waveOutWrite 之后由驱动异步播放；
#   - UI 线程只读电平/播放头，绝不参与混音。
# 说明：waveOut 的回调函数模式在 Ruby/Fiddle 下容易被 GC 和 GVL 干扰，
#       所以这里采用“专用线程 + CALLBACK_EVENT”的等价方案，效果相同且更稳。
#       缓冲区 512 帧 ≈ 10.67ms，6 块 ≈ 64ms 总延迟。
# =============================================================================
require_relative 'constants'
require_relative 'win32'

module Xu808
  class WinMMAudio
    attr_reader :last_error, :xruns, :overruns, :render_peak

    # WAVEHDR 的 WHDR_DONE 标志位。
    WHDR_DONE = 0x0000_0001

    def initialize(engine, frames: BLOCK_FRAMES, buffers: 6)
      @engine = engine
      @frames = frames
      @nbuf = buffers
      @bytes = frames * CHANNELS * 2       # 16bit = 2 字节/样本
      @handle = nil
      @headers = []
      @data = []
      @thread = nil
      @running = false
      @last_error = nil
      @xruns = 0
      @overruns = 0
      @blocks = 0
      @render_peak = 0.0
    end

    def running?
      @running
    end

    # 打开设备并启动音频线程。返回 true/false，失败原因放在 last_error。
    def start
      return true if @running
      open_device
      @running = true
      @thread = Thread.new { audio_loop }
      # 音频线程优先级调高，尽量少被 UI 抢走 GVL/CPU。
      begin
        @thread.priority = 2
      rescue StandardError
        # 某些 Ruby 构建不支持线程优先级，忽略即可。
      end
      true
    rescue StandardError => e
      @last_error = e.message
      cleanup
      false
    end

    # 停止并释放设备。先让线程退出，再 reset/close，顺序不能反。
    def stop
      @running = false
      if @thread
        @thread.join(2.0)
        @thread = nil
      end
      cleanup
    end

    private

    # waveOutOpen：WAVE_MAPPER(0xFFFFFFFF) 表示系统默认输出设备。
    # 用 CALLBACK_EVENT 模式：驱动每播完一块就 SetEvent，音频线程在
    # WaitForSingleObject 上睡眠，不用 sleep 轮询，时序更准、CPU 更省。
    def open_device
      Win32.timeBeginPeriod(1)             # 把系统定时器精度提到 1ms
      @event = Win32.CreateEventW(0, 0, 0, 0).to_i
      raise 'CreateEventW 失败' if @event.zero?

      handle_ptr = Fiddle::Pointer.malloc(8)
      # WAVEFORMATEX: wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec,
      #               nBlockAlign, wBitsPerSample, cbSize
      fmt = [1, CHANNELS, SAMPLE_RATE, SAMPLE_RATE * CHANNELS * 2,
             CHANNELS * 2, 16, 0].pack('S< S< L< L< S< S< S<')
      res = Win32.waveOutOpen(handle_ptr, 0xFFFF_FFFF, fmt, @event, 0, 0x0005_0000)
      raise "waveOutOpen 失败 mmresult=#{res}（0x#{res.to_s(16)}，可能没有音频设备）" if res != 0

      @handle = handle_ptr
      @headers = Array.new(@nbuf) { Fiddle::Pointer.malloc(48) }
      @data = Array.new(@nbuf) { Fiddle::Pointer.malloc(@bytes) }
      @nbuf.times do |i|
        dptr = @data[i]
        dptr[0, @bytes] = "\x00".b * @bytes
        # WAVEHDR: lpData, dwBufferLength, dwBytesRecorded, dwUser,
        #          dwFlags, dwLoops, lpNext, reserved
        @headers[i][0, 48] = [dptr.to_i, @bytes, 0, 0, 0, 0, 0, 0]
                             .pack('Q L L Q L L Q Q')
        r = Win32.waveOutPrepareHeader(handle_value, @headers[i], 48)
        raise "waveOutPrepareHeader 失败=#{r}" if r != 0
      end
    end

    # waveOutOpen 需要的是“指向 HWAVEOUT 的指针”，后续调用要传 HWAVEOUT 值。
    def handle_value
      @handle[0, 8].unpack1('Q')
    end

    def audio_loop
      # 先把全部缓冲填满并排队。
      @nbuf.times do |i|
        break unless @running
        render_and_write(i)
      end
      while @running
        # 最多等 20ms；即使设备卡死也能醒来检查 @running。
        Win32.WaitForSingleObject(@event, 20)
        break unless @running
        # 一次可能有多块完成，全部补上，避免队列被抽空。
        filled = 0
        @nbuf.times do |i|
          next unless header_done?(i)
          render_and_write(i)
          filled += 1
        end
        # 所有缓冲同时空闲 = 队列已经放空，记一次真正的 xrun。
        @xruns += 1 if filled >= @nbuf && @engine.busy?
        sleep 0.001 if filled.zero?
      end
    rescue StandardError => e
      @last_error = e.message
      @running = false
    end

    # 某块是否已经播完（驱动会置 WHDR_DONE）。
    def header_done?(idx)
      (@headers[idx][24, 4].unpack1('L') & WHDR_DONE) != 0
    end

    # 渲染一块并写进队列。
    def render_and_write(idx)
      t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      @engine.render_block(@frames)
      pcm = @engine.block_pcm(@frames)
      @data[idx][0, pcm.bytesize] = pcm
      hdr = @headers[idx]
      # 重新排队前把 flags 复位成已准备；驱动会自己清 DONE、置 INQUEUE。
      hdr[24, 4] = [2].pack('L')
      res = Win32.waveOutWrite(handle_value, hdr, 48)
      raise "waveOutWrite 失败=#{res}" if res != 0

      # 渲染耗时统计：单块偶尔超过 3 个块时长（约 32ms）记一次 overrun。
      # 6 块缓冲共 64ms，能吸收 50ms 级别的 GC 尖峰；真正播不出来会在
      # wait_done 超时里记 xrun。两者分开显示，避免把“慢”误报成“炸音”。
      @blocks += 1
      elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
      if @blocks > 8 && @engine.busy? && elapsed > (@frames.to_f / SAMPLE_RATE) * 3.0
        @overruns += 1
      end
      @engine.xrun = @xruns
      @render_peak = @engine.peak if @engine.peak > @render_peak
    end

    def cleanup
      if @handle
        begin
          hv = handle_value
          Win32.waveOutReset(hv)
          @headers.each do |h|
            Win32.waveOutUnprepareHeader(hv, h, 48)
          end
          Win32.waveOutClose(hv)
        rescue StandardError
          # 关闭阶段任何异常都不该阻止退出。
        end
        @handle = nil
      end
      if @event && !@event.zero?
        Win32.CloseHandle(@event)
        @event = nil
      end
      @headers = []
      @data = []
      begin
        Win32.timeEndPeriod(1)
      rescue StandardError
        nil
      end
    end
  end
end

