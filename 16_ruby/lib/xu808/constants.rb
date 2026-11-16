# frozen_string_literal: true
# =============================================================================
# 文件：lib/xu808/constants.rb
# 作用：XU-808（徐某）Ruby 版的全部“铁律常量”。
# 说明：这里的编号、顺序、默认值必须和 00_shared/XU808_SPEC.md 完全一致，
#       否则同一份工程文件在不同语言版本之间迁移时就会“变味”。
# 徐某写这一段时的想法：先把所有魔法数字集中到一个地方，后面 DSP、UI、
#       导出 MIDI 都从这里取，改一个数全场生效，绝不各写各的。
# =============================================================================
module Xu808
  # 采样率固定 48000 Hz。为什么不用 44100？——规格书第 1 节写死了 48000，
  # 这样 C/Java/Qt/Ruby 各版本的 WAV 可以直接做 A/B 盲听对比，徐某坚持统一。
  SAMPLE_RATE = 48_000

  # 立体声两声道。左右内容相同，靠每个 voice 的 pan 做等功率声像。
  CHANNELS = 2

  # 实时音频块大小：规格允许 256/512，Ruby 解释执行较慢，取 512 更稳。
  # 512 / 48000 = 10.67ms 一块，waveOut 用 6 块缓冲，总延迟约 64ms。
  BLOCK_FRAMES = 512

  # 每个声部最多同时存在的 voice 数（规格第 2 节：4 个，超过抢占最老的）。
  VOICES_PER_TRACK = 4

  # 16 个 voice 的圆周率两倍，省得每个采样都写 Math::PI * 2（常量折叠）。
  TAU = Math::PI * 2.0

  # 正弦查表长度。纯 Ruby 每秒要算几十万次 Math.sin 太慢，
  # 徐某改成 4096 点查表 + 线性索引，听感上 48k/4096 的误差完全听不出来。
  SINE_BITS = 12
  SINE_SIZE = 1 << SINE_BITS            # 4096
  SINE_MASK = SINE_SIZE - 1             # 4095
  # 查表本体在加载时一次性生成，之后音频线程只读，不分配、不加锁。
  SINE_TABLE = Array.new(SINE_SIZE) { |i| Math.sin(TAU * i / SINE_SIZE) }.freeze

  # ---------------------------------------------------------------------------
  # 16 个声部的固定定义（编号、代号、中文名、GM 音符号、可调参数与默认值）。
  # 顺序不可改：pattern 的二维数组第一维就是这里的下标。
  # ---------------------------------------------------------------------------
  VOICES = [
    # 0 底鼓：808 的灵魂，桥接 T 型振荡器用“正弦 + 快速音高包络”近似。
    { code: 'BD', name: '底鼓', note: 36,
      params: [['tune', 0.50], ['decay', 0.55], ['tone', 0.45]] },
    # 1 军鼓：两个正弦（180/330Hz）+ 带通噪声，snappy 控制噪声比例与中心频率。
    { code: 'SD', name: '军鼓', note: 38,
      params: [['tune', 0.50], ['snappy', 0.55], ['decay', 0.45]] },
    # 2 低嗵鼓 / 3 中嗵鼓 / 4 高嗵鼓：同一套下滑正弦模型，基频不同。
    { code: 'LT', name: '低嗵鼓', note: 45,
      params: [['tune', 0.50], ['decay', 0.50], ['tone', 0.35]] },
    { code: 'MT', name: '中嗵鼓', note: 47,
      params: [['tune', 0.50], ['decay', 0.50], ['tone', 0.35]] },
    { code: 'HT', name: '高嗵鼓', note: 50,
      params: [['tune', 0.50], ['decay', 0.50], ['tone', 0.35]] },
    # 5/6/7 康加鼓：比嗵鼓更“木”，噪声点击更少。
    { code: 'LC', name: '低康加', note: 64,
      params: [['tune', 0.50], ['decay', 0.40]] },
    { code: 'MC', name: '中康加', note: 62,
      params: [['tune', 0.50], ['decay', 0.40]] },
    { code: 'HC', name: '高康加', note: 60,
      params: [['tune', 0.50], ['decay', 0.40]] },
    # 8 边击：1.7kHz 短脉冲 + 一点噪声，模拟鼓棒敲鼓圈。
    { code: 'RS', name: '边击', note: 37,
      params: [['tune', 0.50], ['decay', 0.35]] },
    # 9 击木：2.5kHz 正弦，30ms，像两根木棍互敲。
    { code: 'CL', name: '击木', note: 75,
      params: [['tune', 0.50], ['decay', 0.30]] },
    # 10 拍手：4 段噪声爆发 + 尾巴，decay 控制尾巴，tone 控制带通。
    { code: 'CP', name: '拍手', note: 39,
      params: [['decay', 0.50], ['tone', 0.50], ['tail', 0.45]] },
    # 11 沙锤：高通噪声，40ms 左右，tone 决定高通截止。
    { code: 'MA', name: '沙锤', note: 70,
      params: [['decay', 0.35], ['tone', 0.55]] },
    # 12 牛铃：540/800Hz 双方波 + 带通，徐某小时候被这个音洗脑过。
    { code: 'CB', name: '牛铃', note: 56,
      params: [['tune', 0.50], ['decay', 0.40]] },
    # 13 吊镲：6 个非谐方波簇 + 高通，长衰减。
    { code: 'CY', name: '吊镲', note: 49,
      params: [['decay', 0.45], ['tone', 0.50]] },
    # 14 开镲：金属簇 + 较长衰减，会被闭镲“掐断”（choke group 1）。
    { code: 'OH', name: '开镲', note: 46,
      params: [['decay', 0.45], ['tone', 0.50]] },
    # 15 闭镲：金属簇 + 极短衰减。
    { code: 'CH', name: '闭镲', note: 42,
      params: [['decay', 0.45], ['tone', 0.50]] }
  ].freeze

  # 声部代号到下标的反查表，MIDI 输入 / 工程加载时用。
  VOICE_INDEX = VOICES.each_with_index.to_h { |v, i| [v[:code], i] }.freeze

  # GM 鼓音符 -> 声部下标。规格第 7 节写死的映射，少一个都不行。
  NOTE_TO_TRACK = VOICES.each_with_index.to_h { |v, i| [v[:note], i] }.freeze

  # 每轨参数默认值（除 level/pan 外，都在 VOICES 里单独定义）。
  # level 0.75、pan 0.5 是全轨统一默认；mute/solo 默认 false。
  DEFAULT_LEVEL = 0.75
  DEFAULT_PAN = 0.50

  # 步进属性里的条件触发枚举，顺序就是 UI 里循环切换的顺序。
  CONDITIONS = %w[ALWAYS FILL NOT_FILL EVERY_2 EVERY_4].freeze

  # 条件枚举的中文说明，UI 属性面板直接显示，省得再写翻译表。
  CONDITION_NAMES = {
    'ALWAYS' => '总是', 'FILL' => '仅过门', 'NOT_FILL' => '非过门',
    'EVERY_2' => '每2循环', 'EVERY_4' => '每4循环'
  }.freeze

  # 默认工程存放位置：优先脚本根目录，保证“双击 run.bat”就能找到。
  ROOT = File.expand_path('../..', __dir__)

  # 深色 808 配色（COLORREF 是 0x00BBGGRR，所以下面用 BGR 顺序写）。
  COLOR_BG       = 0x00121212 # #121212 主背景
  COLOR_PANEL    = 0x001C1C1C # #1c1c1c 面板
  COLOR_PANEL2   = 0x00232323 # 比面板稍亮，做分区
  COLOR_GRID     = 0x002E2E2E # #2e2e2e 网格线
  COLOR_ACCENT   = 0x00187AFF # #ff7a18 橙色强调（BGR）
  COLOR_ACCENT_D = 0x000F4C99 # 橙色暗版，按下态
  COLOR_ACCENT2  = 0x0066D1FF # #ffd166 重音（BGR）
  COLOR_PLAYHEAD = 0x00F0C94C # #4cc9f0 播放头（BGR）
  COLOR_TEXT     = 0x00E8E8E8 # 主文字
  COLOR_TEXT_DIM = 0x00909090 # 次要文字
  COLOR_OK       = 0x0078D868 # 绿色状态
  COLOR_WARN     = 0x004040E0 # 红色警告（BGR）
  COLOR_WHITE    = 0x00FFFFFF

  # 窗口客户区尺寸（逻辑像素），1180x780 能放下 16 轨 x 16 步 + 参数区。
  WIN_W = 1180
  WIN_H = 780
end
