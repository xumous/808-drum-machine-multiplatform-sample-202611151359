# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/wav.rb
# 作用：把引擎渲染出的 float 样本写成标准 16bit/48kHz/立体声 WAV。
# 徐某提醒：WAV 头里的字节率、块对齐一旦算错，DAW 就会显示“损坏”，
# 所以下面每个字段都按 RIFF 规范逐字节核对过。
# =============================================================================
require_relative 'constants'

module Xu808
  module Wav
    # 写文件；pcm 是已经打包好的 16bit 小端样本串。
    def self.write(path, pcm, sr: SAMPLE_RATE, channels: CHANNELS, bits: 16)
      data_size = pcm.bytesize
      block_align = channels * bits / 8
      byte_rate = sr * block_align
      # 'RIFF' + 文件长度 + 'WAVE'
      header = +'RIFF'
      header << [36 + data_size].pack('V')   # 除前 8 字节外的总长度
      header << 'WAVE'
      # fmt 子块：16 字节 PCM 格式
      header << 'fmt '
      header << [16].pack('V')               # 子块大小
      header << [1].pack('v')                # 1 = PCM
      header << [channels].pack('v')         # 声道数
      header << [sr].pack('V')               # 采样率
      header << [byte_rate].pack('V')        # 每秒字节数
      header << [block_align].pack('v')      # 每帧字节数
      header << [bits].pack('v')             # 位深
      # data 子块
      header << 'data'
      header << [data_size].pack('V')
      File.binwrite(path, header + pcm)
      path
    end

    # 用引擎离线渲染并写文件，返回 [路径, 字节数, 峰值]。
    def self.render(engine, path, seconds)
      pcm, peak = engine.render_seconds(seconds)
      write(path, pcm)
      [path, File.size(path), peak]
    end

    # 分轨导出：每轨单独 solo 一个副本，渲染成独立 WAV。
    # 用 Marshal 深拷贝引擎，保证每轨从头开始、互不影响。
    def self.render_stems(engine, dir, seconds)
      Dir.mkdir(dir) unless Dir.exist?(dir)
      files = []
      VOICES.each_index do |t|
        copy = Marshal.load(Marshal.dump(engine))
        VOICES.each_index { |k| copy.set_solo(k, k == t) }
        copy.play
        path = File.join(dir, format('%02d_%s_%s.wav', t + 1, VOICES[t][:code], VOICES[t][:name]))
        render(copy, path, seconds)
        files << path
      end
      files
    end
  end
end
