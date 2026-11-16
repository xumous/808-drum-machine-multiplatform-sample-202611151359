# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/project.rb
# 作用：工程文件的保存/加载（JSON，带 format/version）。
# 兼容策略：未知字段直接忽略，缺失字段用默认值——这样以后加功能，
# 老工程还能打开，新工程在老版本里也不会崩。徐某把这叫“留后路”。
# =============================================================================
require 'json'
require_relative 'constants'
require_relative 'engine'

module Xu808
  module Project
    module_function

    # 默认工程路径。UI 的 Save/Load 按钮用这个，不弹文件对话框也能一键工作。
    def default_path
      File.join(ROOT, 'xu808_project.json')
    end

    # 引擎 -> 可序列化哈希。
    def dump(engine)
      {
        'format' => 'XU808',
        'version' => 1,
        'bpm' => engine.bpm,
        'swing' => engine.swing,
        'master_volume' => engine.master_volume,
        'drive' => engine.drive,
        'hp_on' => engine.hp_on,
        'hp_freq' => engine.hp_freq,
        'lp_freq' => engine.lp_freq,
        'lp_q' => engine.lp_q,
        'fill' => engine.fill,
        'seed' => engine.seed,
        'song_mode' => engine.song_mode,
        'pattern_index' => engine.current_pattern_index,
        'patterns' => engine.patterns.map(&:to_h),
        'song' => engine.song.map { |e| { 'pattern' => e[:pattern], 'repeats' => e[:repeats] } },
        'mute' => (0...VOICES.length).map { |t| engine.mute?(t) },
        'solo' => (0...VOICES.length).map { |t| engine.solo?(t) },
        'params' => engine.params_snapshot.map { |h| h.transform_keys(&:to_s) }
      }
    end

    def save(engine, path = default_path)
      File.write(path, JSON.pretty_generate(dump(engine)))
      path
    end

    # 加载并返回一个全新的 Engine；旧引擎由调用者负责停止。
    def load(path = default_path)
      h = JSON.parse(File.read(path, encoding: 'UTF-8'))
      raise '不是 XU808 工程' unless h.is_a?(Hash) && h['format'] == 'XU808'

      eng = Engine.new
      eng.bpm = (h['bpm'] || 120).to_f.clamp(40.0, 300.0)
      eng.swing = (h['swing'] || 0).to_f.clamp(0.0, 0.75)
      eng.master_volume = (h['master_volume'] || 0.85).to_f.clamp(0.0, 1.0)
      eng.drive = (h['drive'] || 2.0).to_f.clamp(1.0, 8.0)
      eng.hp_on = h['hp_on'] ? true : false
      eng.hp_freq = (h['hp_freq'] || 30).to_f.clamp(20.0, 800.0)
      eng.lp_freq = (h['lp_freq'] || 18_000).to_f.clamp(200.0, 20_000.0)
      eng.lp_q = (h['lp_q'] || 0.707).to_f.clamp(0.5, 8.0)
      eng.fill = h['fill'] ? true : false
      eng.song_mode = h['song_mode'] ? true : false
      eng.seed = (h['seed'] || 0x8080_8080).to_i & 0xFFFF_FFFF
      eng.sequencer.seed = eng.seed

      pats = h['patterns']
      eng.replace_patterns(pats.map { |ph| Pattern.from_h(ph) }) if pats.is_a?(Array)

      song = h['song']
      if song.is_a?(Array) && !song.empty?
        eng.song = song.map do |e|
          { pattern: (e['pattern'] || 0).to_i, repeats: [(e['repeats'] || 1).to_i, 1].max }
        end
      end

      if h['mute'].is_a?(Array)
        VOICES.each_index { |t| eng.set_mute(t, h['mute'][t]) }
      end
      if h['solo'].is_a?(Array)
        VOICES.each_index { |t| eng.set_solo(t, h['solo'][t]) }
      end
      if h['params'].is_a?(Array)
        VOICES.each_index { |t| eng.set_params(t, h['params'][t]) }
      end
      idx = (h['pattern_index'] || 0).to_i
      eng.sequencer.pattern_index = idx.clamp(0, eng.patterns.length - 1)
      eng
    rescue StandardError => e
      raise "工程加载失败: #{e.message}"
    end
  end
end
