#!/usr/bin/env ruby
# frozen_string_literal: true
# =============================================================================
# 文件：main.rb
# 作用：XU-808 Ruby 版唯一入口（徐某）。
# 用法：
#   ruby main.rb              # 打开 Win32 GDI 鼓机窗口
#   ruby main.rb --selftest   # 无人值守自检，生成 selftest_ruby.wav
#   ruby main.rb --play       # 打开窗口并自动播放
#   ruby main.rb --version    # 打印版本
# 说明：真正的工作分别交给 lib/xu808 下的 DSP / 音序器 / waveOut / UI 模块。
# =============================================================================
require_relative 'lib/xu808/constants'

# 版本号写在这里，UI 标题和自检都会引用。
XU808_RUBY_VERSION = '1.0.0'

# --version 只打印一行，方便脚本检查运行环境。
if ARGV.include?('--version') || ARGV.include?('-v')
  puts "XU-808 Ruby #{XU808_RUBY_VERSION} (#{RUBY_VERSION}, #{RUBY_PLATFORM})"
  exit 0
end

# --selftest 是硬门槛：必须在无声卡环境里跑完并写出非空 WAV。
if ARGV.include?('--selftest')
  require_relative 'lib/xu808/selftest'
  exit Xu808::SelfTest.run
end

# 无界面导出：给自动化/批量渲染用。
if ARGV.include?('--export-wav') || ARGV.include?('--export-midi') || ARGV.include?('--stems')
  require_relative 'lib/xu808/engine'
  require_relative 'lib/xu808/wav'
  require_relative 'lib/xu808/midi'
  eng = Xu808::Engine.new
  eng.play
  if ARGV.include?('--export-wav')
    path = File.join(Xu808::ROOT, 'xu808_export.wav')
    _p, size, peak = Xu808::Wav.render(eng, path, 8.0)
    puts "WAV #{path} #{size} bytes peak=#{format('%.3f', peak)}"
  elsif ARGV.include?('--export-midi')
    path = File.join(Xu808::ROOT, 'xu808_export.mid')
    Xu808::Midi.export(eng, path, pattern_index: 0)
    puts "MIDI #{path} #{File.size(path)} bytes"
  else
    dir = ARGV[ARGV.index('--stems') + 1] || File.join(Xu808::ROOT, 'stems')
    # 分轨渲染用 4 小节 = 8 秒，足够覆盖默认 pattern 的循环。
    files = Xu808::Wav.render_stems(eng, dir, 8.0)
    puts "STEMS #{files.length} files -> #{dir}"
    files.each { |f| puts "  #{File.basename(f)} #{File.size(f)}" }
  end
  exit 0
end

# 其余情况进入 GUI。UI 依赖 Fiddle 调 user32/gdi32/winmm，
# 所以非 Windows 平台会在这里给出明确提示而不是一堆堆栈。
begin
  require_relative 'lib/xu808/ui_win32'
rescue LoadError => e
  warn "无法加载 Win32 UI：#{e.message}"
  warn '本版本 GUI 仅支持 Windows；其它平台请使用 --selftest 验证 DSP。'
  exit 2
end

# --uismoke 是给自动化验证用的：窗口打开 1.8 秒、跑满 50 多次重绘后自动关闭，
# 打印 UISMOKE OK。它证明 GDI 绘制、消息循环、音频线程都不会崩。
if ARGV.include?('--uismoke')
  Xu808::UI.run(auto_close_ms: 1800)
  exit 0
end

Xu808::UI.run(auto_play: ARGV.include?('--play'))
